#include "web_state.h"
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <unistd.h>

namespace SOS2::State {
namespace {
constexpr std::string_view suffixes[] = {
#include "../../third_party/publicsuffix/rules.inc"
};

std::string lower(std::string_view input) {
    std::string result(input);
    for (char &c : result)
        if (c >= 'A' && c <= 'Z') c += 32;
    return result;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

bool hostValid(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.find("..") != host.npos)
        return false;
    for (char c : host)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    return true;
}

bool ipAddress(std::string_view host) { return host.find_first_not_of("0123456789.") == host.npos; }

bool domainMatches(std::string_view host, std::string_view domain) {
    return host == domain ||
           (!ipAddress(host) && host.size() > domain.size() && host.ends_with(domain) &&
            host[host.size() - domain.size() - 1] == '.');
}

bool pathMatches(std::string_view request, std::string_view cookie) {
    return request == cookie || (request.starts_with(cookie) &&
                                 (cookie.ends_with('/') || (request.size() > cookie.size() &&
                                                            request[cookie.size()] == '/')));
}

size_t suffixLength(std::string_view host) {
    if (host.empty()) return 0;
    // A DNS root dot stays part of the site identity but not the PSL lookup.
    if (host.ends_with('.')) return suffixLength(host.substr(0, host.size() - 1)) + 1;
    auto has = [](std::string_view value) {
        return std::binary_search(std::begin(suffixes), std::end(suffixes), value);
    };
    size_t result = host.size() - host.rfind('.') - 1;
    if (host.find('.') == host.npos) result = host.size();
    for (size_t at = 0; at < host.size();) {
        auto part = host.substr(at);
        auto dot = part.find('.');
        if (has("!" + std::string(part))) return dot == part.npos ? 0 : part.size() - dot - 1;
        if (part == "sos2.local" || has(part)) result = std::max(result, part.size());
        if (dot == part.npos) break;
        if (has("*." + std::string(part.substr(dot + 1)))) result = std::max(result, part.size());
        at += dot + 1;
    }
    return result;
}

bool originValid(std::string_view origin) {
    return origin.size() <= 512 &&
           (origin.starts_with("https://") || origin.starts_with("http://")) &&
           origin.find('\0') == origin.npos;
}

struct Statement {
    sqlite3_stmt *value = nullptr;
    int status = SQLITE_OK;

    Statement(sqlite3 *db, const char *sql) {
        status = sqlite3_prepare_v2(db, sql, -1, &value, nullptr);
    }

    ~Statement() { sqlite3_finalize(value); }

    void text(int index, std::string_view data) {
        if (status == SQLITE_OK)
            status = sqlite3_bind_text(value, index, data.empty() ? "" : data.data(),
                                       int(data.size()), SQLITE_TRANSIENT);
    }

    void blob(int index, const std::u16string &data) {
        if (status == SQLITE_OK)
            status = sqlite3_bind_blob(value, index,
                                       data.empty() ? static_cast<const void *>("") : data.data(),
                                       int(data.size() * 2), SQLITE_TRANSIENT);
    }

    void number(int index, int64_t number) {
        if (status == SQLITE_OK) status = sqlite3_bind_int64(value, index, number);
    }

    int step() { return status == SQLITE_OK ? sqlite3_step(value) : status; }

    int64_t number(int index) { return sqlite3_column_int64(value, index); }

    std::string text(int index) {
        auto *data = sqlite3_column_text(value, index);
        int size = sqlite3_column_bytes(value, index);
        return data ? std::string(reinterpret_cast<const char *>(data), size) : std::string();
    }

    std::u16string blob(int index) {
        int size = sqlite3_column_bytes(value, index);
        std::u16string result(size / 2, u'\0');
        if (size) memcpy(result.data(), sqlite3_column_blob(value, index), size);
        return result;
    }
};
enum class SameSite { None, Lax, Strict };

struct Cookie {
    std::string name, value, domain, path;
    bool hostOnly = true, secure = false, httpOnly = false;
    SameSite sameSite = SameSite::Lax;
    int64_t expires = 0, created = 0;

    bool identity(const Cookie &other) const {
        return name == other.name && domain == other.domain && path == other.path;
    }

    bool operator==(const Cookie &) const = default;

    size_t size() const { return name.size() + value.size() + domain.size() + path.size() + 64; }
};

bool cookieName(std::string_view name) {
    if (name.empty()) return false;
    for (unsigned char c : name)
        if (c <= 32 || c >= 127 || std::string_view("()<>@,;:\\\"/[]?={}").find(c) != name.npos)
            return false;
    return true;
}

bool cookieValue(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    for (unsigned char c : value)
        if (c < 0x21 || c > 0x7e || c == '"' || c == ',' || c == ';' || c == '\\') return false;
    return true;
}

std::optional<int> digits(std::string_view value, unsigned minimum, unsigned maximum) {
    unsigned count = 0;
    int result = 0;
    while (count < value.size() && value[count] >= '0' && value[count] <= '9') {
        if (count >= maximum) return {};
        result = result * 10 + value[count++] - '0';
    }
    if (count < minimum) return {};
    return result;
}

std::optional<int64_t> cookieDate(std::string_view input) {
    std::optional<int> day, month, year;
    int hour = -1, minute = -1, second = -1;
    auto delimiter = [](unsigned char c) {
        return c == 9 || (c >= 0x20 && c <= 0x2f) || (c >= 0x3b && c <= 0x40) ||
               (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e);
    };
    while (!input.empty()) {
        while (!input.empty() && delimiter(input.front())) input.remove_prefix(1);
        size_t count = 0;
        while (count < input.size() && !delimiter(input[count])) ++count;
        auto token = input.substr(0, count);
        input.remove_prefix(count);
        if (token.empty()) continue;
        auto a = token.find(':'), b = a == token.npos ? a : token.find(':', a + 1);
        if (hour < 0 && a != token.npos && b != token.npos) {
            auto h = digits(token.substr(0, a), 1, 2),
                 m = digits(token.substr(a + 1, b - a - 1), 1, 2),
                 s = digits(token.substr(b + 1), 1, 2);
            if (h && m && s && a <= 2 && b - a - 1 <= 2) {
                hour = *h;
                minute = *m;
                second = *s;
                continue;
            }
        }
        if (!day)
            if (auto n = digits(token, 1, 2)) {
                day = n;
                continue;
            }
        if (!month && token.size() >= 3) {
            auto name = lower(token.substr(0, 3));
            auto index = std::string_view("janfebmaraprmayjunjulaugsepoctnovdec").find(name);
            if (index != std::string_view::npos && index % 3 == 0) {
                month = int(index / 3) + 1;
                continue;
            }
        }
        if (!year) year = digits(token, 2, 4);
    }
    if (!day || !month || !year || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59)
        return {};
    int y = *year, m = *month, d = *day;
    if (y >= 70 && y <= 99) y += 1900;
    else if (y < 70) y += 2000;
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max = days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    if (y < 1601 || d < 1 || d > max) return {};
    y -= m <= 2;
    int era = y / 400;
    unsigned yo = y - era * 400;
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t date = int64_t(era) * 146097 + yo * 365 + yo / 4 - yo / 100 + doy - 719468;
    return date * 86400 + hour * 3600 + minute * 60 + second;
}

bool allowedContext(const CookieContext &context) {
    return context.credentials && context.url.valid() &&
           (context.topLevel || sameSite(context.url, context.firstParty));
}

std::optional<Cookie> parseCookie(const CookieContext &context, std::string_view header,
                                  int64_t now) {
    if (!allowedContext(context) || header.size() > 8192 || now < 1 ||
        now > INT64_MAX - 400 * 86400)
        return {};
    for (unsigned char c : header)
        if ((c < 32 && c != '\t') || c == 127) return {};
    auto first = header.find(';');
    auto pair = header.substr(0, first);
    auto equal = pair.find('=');
    if (equal == pair.npos) return {};
    Cookie result;
    result.name = trim(pair.substr(0, equal));
    result.value = trim(pair.substr(equal + 1));
    if (result.name.size() + result.value.size() > 4096 || !cookieName(result.name) ||
        !cookieValue(result.value))
        return {};
    result.domain = context.url.host;
    auto slash = context.url.path.rfind('/');
    result.path = slash == std::string::npos || !slash ? "/" : context.url.path.substr(0, slash);
    std::optional<int64_t> expires, maxAge;
    bool domainAttribute = false, rootPath = false, partitioned = false;
    while (first != header.npos) {
        header.remove_prefix(first + 1);
        first = header.find(';');
        auto part = trim(header.substr(0, first));
        equal = part.find('=');
        auto name = lower(trim(part.substr(0, equal)));
        auto value = equal == part.npos ? std::string_view() : trim(part.substr(equal + 1));
        if (name == "domain") {
            domainAttribute = true;
            if (value.starts_with('.')) value.remove_prefix(1);
            auto domain = lower(value);
            if (!hostValid(domain) || domain.ends_with('.') || ipAddress(context.url.host) ||
                !domainMatches(context.url.host, domain) || publicSuffix(domain))
                return {};
            result.domain = std::move(domain);
            result.hostOnly = false;
        } else if (name == "path") {
            if (value.starts_with('/')) {
                result.path = value;
                rootPath = value == "/";
            }
        } else if (name == "secure") result.secure = true;
        else if (name == "httponly") result.httpOnly = true;
        else if (name == "partitioned") partitioned = true;
        else if (name == "samesite") {
            auto mode = lower(value);
            result.sameSite = mode == "none"     ? SameSite::None
                              : mode == "strict" ? SameSite::Strict
                                                 : SameSite::Lax;
        } else if (name == "expires") {
            if (auto date = cookieDate(value)) expires = date;
        } else if (name == "max-age" && !value.empty()) {
            bool negative = value.front() == '-';
            if (negative) value.remove_prefix(1);
            if (value.empty() || value.find_first_not_of("0123456789") != value.npos) continue;
            int64_t seconds = 0;
            for (char c : value) seconds = std::min<int64_t>(400 * 86400, seconds * 10 + c - '0');
            maxAge = negative ? 0 : seconds;
        }
    }
    if (result.path.size() > 1024 || partitioned || (context.dom && result.httpOnly) ||
        (result.secure && !context.url.secure()) ||
        (result.sameSite == SameSite::None && !result.secure))
        return {};
    for (unsigned char c : result.path)
        if (c < 32 || c == 127) return {};
    auto prefix = lower(result.name);
    if ((prefix.starts_with("__secure-") || prefix.starts_with("__host-") ||
         prefix.starts_with("__http-")) &&
        (!result.secure || !context.url.secure()))
        return {};
    if (prefix.starts_with("__host-") && (domainAttribute || !rootPath)) return {};
    if ((prefix.starts_with("__http-") || prefix.starts_with("__host-http-")) &&
        (!result.httpOnly || context.dom))
        return {};
    // Cross-site responses in embedded contexts cannot set Lax/Strict cookies.
    if (!context.topLevel && !context.sameSite && result.sameSite != SameSite::None) return {};
    if (maxAge) result.expires = *maxAge ? now + *maxAge : 1;
    else if (expires) result.expires = std::max<int64_t>(1, std::min(*expires, now + 400 * 86400));
    return result;
}
}

bool Address::valid() const {
    return (scheme == "http" || scheme == "https") && hostValid(host) && port <= 65535 &&
           !path.empty() && path.front() == '/';
}

std::string Address::origin() const {
    return valid() ? scheme + "://" + host + ":" + std::to_string(port) : std::string();
}

bool publicSuffix(std::string_view host) {
    return hostValid(host) && !ipAddress(host) && suffixLength(host) == host.size();
}

std::string registrableDomain(std::string_view host) {
    if (!hostValid(host) || ipAddress(host)) return std::string(host);
    auto length = suffixLength(host);
    if (length >= host.size()) return std::string(host);
    auto separator = host.rfind('.', host.size() - length - 2);
    return std::string(host.substr(separator == host.npos ? 0 : separator + 1));
}

bool sameSite(const Address &a, const Address &b) {
    return a.valid() && b.valid() && a.scheme == b.scheme &&
           registrableDomain(a.host) == registrableDomain(b.host);
}

// Every page shares the browser profile's database, and the VFS has the pages
// take turns (sdk/runtime/sqlite_vfs.c). A writer gives what it changed a new
// version ("" for cookies, else the origin), so other pages reload only that.
// Session cookies are shared too, tagged with this boot's session, which the
// first page after boot names in the profile's /tmp: every boot empties it.
struct Store::Impl {
    enum class Status { Retry, Ready, Broken };
    using Clock = std::chrono::steady_clock;
    using Versions = std::map<std::string, int64_t, std::less<>>;
    std::string path;
    sqlite3 *db = nullptr;
    Status status = Status::Retry;
    bool checked = false;
    Clock::time_point attempted, refreshed;
    std::string session;
    std::vector<Cookie> jar;
    int64_t sequence = 0;
    int64_t dataVersion = 0;
    Versions versions;

    explicit Impl(std::string file) : path(std::move(file)) { open(); }

    ~Impl() { sqlite3_close(db); }

    static int64_t lookup(const Versions &map, std::string_view scope) {
        auto at = map.find(scope);
        return at == map.end() ? 0 : at->second;
    }

    bool exec(const char *sql) {
        return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    bool commit(bool good) {
        if (good && exec("COMMIT")) return true;
        exec("ROLLBACK");
        return false;
    }

    // Another page holding the database past the busy timeout is temporary.
    bool busy() const {
        int code = sqlite3_extended_errcode(db);
        return (code & 0xff) == SQLITE_BUSY || (code & 0xff) == SQLITE_LOCKED ||
               code == SQLITE_IOERR_LOCK;
    }

    Status failed() const { return busy() ? Status::Retry : Status::Broken; }

    void open() {
        attempted = Clock::now();
        if (!db) {
            if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                nullptr) != SQLITE_OK) {
                sqlite3_close(db);
                db = nullptr;
                status = Status::Broken;
                return;
            }
            sqlite3_busy_timeout(db, 5000);
            sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 2 * originQuota + 4096);
            sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        }
        status = initialize();
        // Load everything: dataVersion 0 never matches.
        if (status == Status::Ready && !read() && status == Status::Ready) status = Status::Retry;
    }

    Status initialize() {
        if (!exec("PRAGMA trusted_schema=OFF;PRAGMA temp_store=MEMORY;")) return failed();
        if (!checked) {
            Statement check(db, "PRAGMA quick_check");
            if (check.step() != SQLITE_ROW) return failed();
            if (check.text(0) != "ok") return Status::Broken;
            Statement version(db, "PRAGMA user_version"), application(db, "PRAGMA application_id");
            if (version.step() != SQLITE_ROW || application.step() != SQLITE_ROW) return failed();
            if (version.number(0) == 0 && application.number(0) == 0) {
                Statement tables(
                    db, "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'");
                if (tables.step() != SQLITE_ROW) return failed();
                if (tables.number(0)) return Status::Broken;
            } else if (version.number(0) != 2 || application.number(0) != 0x534f5332)
                return Status::Broken;
            checked = true;
        }
        if (!exec(
                "PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;PRAGMA max_page_count=3072;") ||
            !exec("BEGIN IMMEDIATE"))
            return failed();
        if (!exec(
                "CREATE TABLE IF NOT EXISTS local_items(origin TEXT NOT NULL,key BLOB NOT NULL,value BLOB NOT NULL,ordinal INTEGER NOT NULL,PRIMARY KEY(origin,key));"
                "CREATE TABLE IF NOT EXISTS cookies(name TEXT NOT NULL,value TEXT NOT NULL,domain TEXT NOT NULL,path TEXT NOT NULL,host_only INTEGER NOT NULL,secure INTEGER NOT NULL,http_only INTEGER NOT NULL,same_site INTEGER NOT NULL,expires INTEGER NOT NULL,created INTEGER NOT NULL,session TEXT NOT NULL,PRIMARY KEY(name,domain,path));"
                "CREATE TABLE IF NOT EXISTS versions(version INTEGER PRIMARY KEY AUTOINCREMENT,scope TEXT NOT NULL UNIQUE);"
                // An origin without items needs no version: a page that saw one
                // reloads, and AUTOINCREMENT never hands that version out again.
                "DELETE FROM versions WHERE scope<>'' AND scope NOT IN (SELECT origin FROM local_items);"
                "PRAGMA application_id=1397707570;PRAGMA user_version=2;")) {
            auto result = failed();
            exec("ROLLBACK");
            return result;
        }
        if (!joinSession()) {
            exec("ROLLBACK");
            return Status::Retry;
        }
        return commit(true) ? Status::Ready : failed();
    }

    // Inside the write transaction, so exactly one page starts the session.
    bool joinSession() {
        static constexpr const char *file = "/tmp/session";
        char id[33] = {};
        int fd = ::open(file, O_RDONLY);
        if (fd >= 0) {
            bool valid = ::read(fd, id, 32) == 32 && std::strspn(id, "0123456789abcdef") == 32;
            ::close(fd);
            if (valid) {
                session = id;
                return true;
            }
            ::unlink(file); // Left by a page that stopped while starting a session.
        } else if (errno != ENOENT) return false;
        unsigned char bytes[16];
        if (getentropy(bytes, sizeof(bytes))) return false;
        for (unsigned i = 0; i < sizeof(bytes); ++i) std::snprintf(id + 2 * i, 3, "%02x", bytes[i]);
        fd = ::open(file, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) return false;
        bool written = ::write(fd, id, 32) == 32;
        if (::close(fd) || !written ||
            !exec(
                "DELETE FROM cookies WHERE expires=0;INSERT OR REPLACE INTO versions(scope) VALUES('');")) {
            ::unlink(file);
            return false;
        }
        session = id;
        return true;
    }

    bool available() {
        if (status == Status::Retry && Clock::now() - attempted >= std::chrono::seconds(1)) open();
        return status == Status::Ready;
    }

    bool loadCookies() {
        Statement cookies(
            db,
            "SELECT name,value,domain,path,host_only,secure,http_only,same_site,expires,created FROM cookies WHERE expires<>0 OR session=?");
        cookies.text(1, session);
        std::vector<Cookie> loaded;
        int64_t latest = 0;
        size_t bytes = 0;
        int result;
        while ((result = cookies.step()) == SQLITE_ROW) {
            Cookie c;
            c.name = cookies.text(0);
            c.value = cookies.text(1);
            c.domain = cookies.text(2);
            c.path = cookies.text(3);
            c.hostOnly = cookies.number(4);
            c.secure = cookies.number(5);
            c.httpOnly = cookies.number(6);
            auto mode = cookies.number(7);
            c.sameSite = SameSite(mode);
            c.expires = cookies.number(8);
            c.created = cookies.number(9);
            if (!cookieName(c.name) || !cookieValue(c.value) ||
                c.name.size() + c.value.size() > 4096 || !hostValid(c.domain) ||
                (!c.hostOnly && (publicSuffix(c.domain) || ipAddress(c.domain))) ||
                c.path.empty() || c.path.front() != '/' || c.path.size() > 1024 || mode < 0 ||
                mode > 2 || c.expires < 0 || c.created < 0 || c.created == INT64_MAX)
                return false;
            bytes += c.size();
            if (loaded.size() >= 1024 || bytes > 2 * 1024 * 1024) return false;
            latest = std::max(latest, c.created);
            loaded.push_back(std::move(c));
        }
        if (result != SQLITE_DONE) return false;
        jar = std::move(loaded);
        sequence = std::max(sequence, latest);
        return true;
    }

    // PRAGMA data_version changes when another connection, here another page,
    // commits; 0 if it cannot be read.
    int64_t currentVersion() {
        Statement version(db, "PRAGMA data_version");
        return version.step() == SQLITE_ROW ? version.number(0) : 0;
    }

    // Catches up with what other pages committed. A busy database leaves
    // everything as it was, for the next call; invalid cookies break the store.
    bool catchUp() {
        auto current = currentVersion();
        if (!current) return false;
        if (current == dataVersion) return true;
        Versions next;
        {
            Statement rows(db, "SELECT scope,version FROM versions");
            int result;
            while ((result = rows.step()) == SQLITE_ROW) next.emplace(rows.text(0), rows.number(1));
            if (result != SQLITE_DONE) return false;
        }
        if (lookup(next, "") != lookup(versions, "") && !loadCookies()) {
            if (!busy()) status = Status::Broken;
            return false;
        }
        versions = std::move(next);
        dataVersion = current;
        return true;
    }

    // One read transaction takes the lock once and sees one state.
    bool read() { return exec("BEGIN") && commit(catchUp()); }

    // Reads look at most every 250 ms, so loops over cookies or keys stay cheap.
    void refresh(bool force) {
        if (!available()) return;
        auto now = Clock::now();
        if (!force && now - refreshed < std::chrono::milliseconds(250)) return;
        refreshed = now;
        read();
    }

    // Runs change in one write transaction and gives scope a new version.
    // This page's version follows only if it was current.
    template <class Change> bool transact(std::string_view scope, Change &&change) {
        if (!available() || !exec("BEGIN IMMEDIATE")) return false;
        int64_t previous = 0, next = 0;
        bool good = change();
        if (good) {
            Statement old(db, "SELECT version FROM versions WHERE scope=?");
            old.text(1, scope);
            int result = old.step();
            previous = result == SQLITE_ROW ? old.number(0) : 0;
            Statement put(db, "INSERT OR REPLACE INTO versions(scope) VALUES(?)");
            put.text(1, scope);
            good = (result == SQLITE_ROW || result == SQLITE_DONE) && put.step() == SQLITE_DONE;
            next = sqlite3_last_insert_rowid(db);
        }
        if (!commit(good)) return false;
        if (lookup(versions, scope) == previous)
            versions.insert_or_assign(std::string(scope), next);
        return true;
    }

    bool erase(const Cookie &c) {
        Statement statement(db, "DELETE FROM cookies WHERE name=? AND domain=? AND path=?");
        statement.text(1, c.name);
        statement.text(2, c.domain);
        statement.text(3, c.path);
        return statement.step() == SQLITE_DONE;
    }

    bool put(const Cookie &c) {
        Statement statement(db, "INSERT OR REPLACE INTO cookies VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        statement.text(1, c.name);
        statement.text(2, c.value);
        statement.text(3, c.domain);
        statement.text(4, c.path);
        statement.number(5, c.hostOnly);
        statement.number(6, c.secure);
        statement.number(7, c.httpOnly);
        statement.number(8, int(c.sameSite));
        statement.number(9, c.expires);
        statement.number(10, c.created);
        statement.text(11, c.expires ? std::string_view() : session);
        return statement.step() == SQLITE_DONE;
    }

    // Writes the difference between the jar and next.
    bool saveCookies(const std::vector<Cookie> &next) {
        for (const auto &old : jar)
            if (std::none_of(next.begin(), next.end(),
                             [&](const Cookie &c) { return old.identity(c); }) &&
                !erase(old))
                return false;
        for (const auto &c : next)
            if (std::find(jar.begin(), jar.end(), c) == jar.end() && !put(c)) return false;
        return true;
    }
};

Store::Store(const std::string &path) : impl(std::make_unique<Impl>(path)) { }

Store::~Store() = default;

bool Store::available() { return impl->available(); }

int64_t Store::version(std::string_view origin, Look look) {
    if (look != Look::Cached) impl->refresh(look == Look::Now);
    return Impl::lookup(impl->versions, origin);
}

bool Store::read(const std::string &origin, Items &output) {
    output.clear();
    if (!available() || !originValid(origin)) return false;
    Statement statement(impl->db,
                        "SELECT key,value FROM local_items WHERE origin=? ORDER BY ordinal");
    statement.text(1, origin);
    size_t bytes = 0;
    int result;
    while ((result = statement.step()) == SQLITE_ROW) {
        int keyBytes = sqlite3_column_bytes(statement.value, 0),
            valueBytes = sqlite3_column_bytes(statement.value, 1);
        bytes += keyBytes + valueBytes;
        if (keyBytes % 2 || valueBytes % 2 || bytes > originQuota || output.size() >= 4096) {
            output.clear();
            return false;
        }
        output.emplace_back(statement.blob(0), statement.blob(1));
    }
    if (result != SQLITE_DONE) output.clear();
    return result == SQLITE_DONE;
}

bool Store::set(const std::string &origin, const std::u16string &key, const std::u16string &value) {
    if (!originValid(origin) || key.size() > originQuota / 2 ||
        value.size() > originQuota / 2 - key.size())
        return false;
    return impl->transact(origin, [&] {
        Statement old(
            impl->db,
            "SELECT length(key)+length(value),ordinal FROM local_items WHERE origin=? AND key=?");
        old.text(1, origin);
        old.blob(2, key);
        int result = old.step();
        bool found = result == SQLITE_ROW;
        int64_t oldBytes = found ? old.number(0) : 0, ordinal = found ? old.number(1) : 0;
        Statement own(
            impl->db,
            "SELECT coalesce(sum(length(key)+length(value)),0),count(*),coalesce(max(ordinal),0) FROM local_items WHERE origin=?");
        own.text(1, origin);
        Statement total(
            impl->db,
            "SELECT coalesce(sum(length(key)+length(value)),0),count(*),count(DISTINCT origin) FROM local_items");
        if (!(found || result == SQLITE_DONE) || own.step() != SQLITE_ROW ||
            total.step() != SQLITE_ROW)
            return false;
        int64_t size = (key.size() + value.size()) * 2;
        if (own.number(0) - oldBytes + size > int64_t(originQuota) ||
            total.number(0) - oldBytes + size > int64_t(profileQuota) ||
            (!found && (own.number(2) == INT64_MAX || own.number(1) >= 4096 ||
                        total.number(1) >= 32768 || (!own.number(1) && total.number(2) >= 256))))
            return false;
        if (!found) ordinal = own.number(2) + 1;
        Statement put(impl->db, "INSERT OR REPLACE INTO local_items VALUES(?,?,?,?)");
        put.text(1, origin);
        put.blob(2, key);
        put.blob(3, value);
        put.number(4, ordinal);
        return put.step() == SQLITE_DONE;
    });
}

bool Store::remove(const std::string &origin, const std::u16string &key) {
    if (!originValid(origin)) return false;
    return impl->transact(origin, [&] {
        Statement statement(impl->db, "DELETE FROM local_items WHERE origin=? AND key=?");
        statement.text(1, origin);
        statement.blob(2, key);
        return statement.step() == SQLITE_DONE;
    });
}

bool Store::clear(const std::string &origin) {
    if (!originValid(origin)) return false;
    return impl->transact(origin, [&] {
        Statement statement(impl->db, "DELETE FROM local_items WHERE origin=?");
        statement.text(1, origin);
        return statement.step() == SQLITE_DONE;
    });
}

bool Store::setCookie(const CookieContext &context, std::string_view header, int64_t now) {
    auto parsed = parseCookie(context, header, now);
    if (!parsed) return false;
    const Cookie &c = *parsed;
    std::vector<Cookie> next;
    // The jar is current inside the write transaction, so the limits hold
    // whichever pages write at the same time.
    bool written = impl->transact("", [&] {
        if (!impl->catchUp() || impl->sequence >= INT64_MAX - 1) return false;
        next = impl->jar;
        std::erase_if(next, [&](const Cookie &old) { return old.expires && old.expires <= now; });
        for (const auto &old : next) {
            if (context.dom && old.httpOnly && c.identity(old)) return false;
            if (!context.url.secure() && old.secure && c.name == old.name &&
                (domainMatches(c.domain, old.domain) || domainMatches(old.domain, c.domain)) &&
                pathMatches(c.path, old.path))
                return false;
        }
        auto at = std::find_if(next.begin(), next.end(),
                               [&](const Cookie &old) { return c.identity(old); });
        Cookie added = c;
        added.created = at == next.end() ? impl->sequence + 1 : at->created;
        if (at != next.end()) next.erase(at);
        if (!added.expires || added.expires > now) next.push_back(added);
        auto site = registrableDomain(c.domain);
        auto oldest = [&](bool own) {
            auto candidate = next.end();
            for (auto it = next.begin(); it != next.end(); ++it)
                if ((!own || registrableDomain(it->domain) == site) &&
                    (candidate == next.end() || it->created < candidate->created))
                    candidate = it;
            return candidate;
        };
        while (std::count_if(next.begin(), next.end(), [&](const Cookie &item) {
                   return registrableDomain(item.domain) == site;
               }) > 180)
            next.erase(oldest(true));
        size_t size = 0;
        for (const auto &item : next) size += item.size();
        while (next.size() > 1024 || size > 2 * 1024 * 1024) {
            auto victim = oldest(false);
            size -= victim->size();
            next.erase(victim);
        }
        return impl->saveCookies(next);
    });
    if (!written) return false;
    for (const auto &item : next) impl->sequence = std::max(impl->sequence, item.created);
    impl->jar = std::move(next);
    return true;
}

std::string Store::cookies(const CookieContext &context, int64_t now) {
    impl->refresh(false);
    if (!available() || !allowedContext(context)) return {};
    bool same = context.sameSite && sameSite(context.url, context.firstParty);
    std::vector<const Cookie *> matching;
    for (const auto &c : impl->jar) {
        // Without wall time (now<1) expiry is unknown, and parseCookie already
        // drops every Set-Cookie including deletions: send session cookies only.
        if ((c.expires && (now < 1 || c.expires <= now)) || (c.secure && !context.url.secure()) ||
            (context.dom && c.httpOnly))
            continue;
        if ((c.hostOnly ? context.url.host != c.domain
                        : !domainMatches(context.url.host, c.domain)) ||
            !pathMatches(context.url.path, c.path))
            continue;
        if (!context.dom && !same &&
            (c.sameSite == SameSite::Strict ||
             (c.sameSite == SameSite::Lax && !(context.topLevel && context.safeMethod))))
            continue;
        matching.push_back(&c);
    }
    std::sort(matching.begin(), matching.end(), [](const Cookie *a, const Cookie *b) {
        return a->path.size() != b->path.size() ? a->path.size() > b->path.size()
                                                : a->created < b->created;
    });
    std::string result;
    for (auto *cookie : matching) {
        if (result.size() + cookie->name.size() + cookie->value.size() + 3 > 16 * 1024) break;
        if (!result.empty()) result += "; ";
        result += cookie->name + "=" + cookie->value;
    }
    return result;
}
}
