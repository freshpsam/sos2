#define MBEDTLS_CONFIG_FILE <sos2/mbedtls_config.h>
#include "http_client.h"
#include "http2_session.h"
#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>
#include <psa/crypto_extra.h>
#include <sos2/network.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace {
uint64_t milliseconds() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return uint64_t(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
}

int entropy(void *, unsigned char *out, size_t size) {
    while (size) {
        size_t n = std::min<size_t>(size, 256);
        if (getentropy(out, n)) return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
        out += n;
        size -= n;
    }
    return 0;
}

struct TLS {
    mbedtls_ctr_drbg_context random;
    mbedtls_x509_crt roots;
    mbedtls_ssl_config config;
    bool ready = false;

    TLS() {
        mbedtls_ctr_drbg_init(&random);
        mbedtls_x509_crt_init(&roots);
        mbedtls_ssl_config_init(&config);
        const unsigned char personal[] = "SOS2 native HTTPS";
        if (mbedtls_ctr_drbg_seed(&random, entropy, nullptr, personal, sizeof(personal))) return;
        FILE *file = fopen("/certs/roots.pem", "rb");
        if (!file) return;
        std::vector<unsigned char> pem(384 * 1024 + 1);
        size_t size = fread(pem.data(), 1, pem.size(), file);
        bool bad = ferror(file) || size == pem.size();
        fclose(file);
        if (bad || !size) return;
        pem[size] = 0;
        if (mbedtls_x509_crt_parse(&roots, pem.data(), size + 1)) return;
        if (mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT))
            return;
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&config, &roots, nullptr);
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
        mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_session_tickets(&config, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
        mbedtls_ssl_conf_tls13_enable_signal_new_session_tickets(
            &config, MBEDTLS_SSL_TLS1_3_SIGNAL_NEW_SESSION_TICKETS_ENABLED);
        static const char *protocols[] = {"h2", "http/1.1", nullptr};
        if (mbedtls_ssl_conf_alpn_protocols(&config, protocols)) return;
        ready = true;
    }
};

TLS &tls() {
    static TLS instance;
    return instance;
}

int sendBytes(void *context, const unsigned char *data, size_t size) {
    int result =
        sos2_net_send(*static_cast<int *>(context), data, std::min<size_t>(size, USER_NET_IO_MAX));
    if (result < 0)
        return errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return result;
}

int receiveBytes(void *context, unsigned char *data, size_t size) {
    int result =
        sos2_net_recv(*static_cast<int *>(context), data, std::min<size_t>(size, USER_NET_IO_MAX));
    if (result < 0)
        return errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return result;
}

bool validText(std::string_view text, bool spaces = true) {
    for (unsigned char c : text)
        if (c < 32 || c == 127 || (!spaces && c == ' ')) return false;
    return true;
}

bool validToken(std::string_view text) {
    if (text.empty()) return false;
    for (unsigned char c : text)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos))
            return false;
    return true;
}
}

extern "C" mbedtls_ms_time_t mbedtls_ms_time(void) { return milliseconds(); }

extern "C" psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *,
                                                        uint8_t *output, size_t size,
                                                        size_t *written) {
    *written = 0;
    while (*written < size) {
        size_t n = std::min<size_t>(size - *written, MBEDTLS_CTR_DRBG_MAX_REQUEST);
        if (mbedtls_ctr_drbg_random(&tls().random, output + *written, n))
            return PSA_ERROR_INSUFFICIENT_ENTROPY;
        *written += n;
    }
    return PSA_SUCCESS;
}

namespace SOS2::HTTP {
struct CertificateDates {
    mbedtls_x509_time from, to;

    bool valid() const {
        return !mbedtls_x509_time_is_future(&from) && !mbedtls_x509_time_is_past(&to);
    }
};

struct Connection {
    std::string host;
    unsigned port;
    bool tls;
    int handle = -1;
    mbedtls_ssl_context ssl;
    uint64_t idle = 0;
    time_t wall = 0;
    uint64_t verifiedAt = 0;
    time_t verifiedWall = 0;
    // When a cached session was offered: the time of its original full
    // verification, so re-saved sessions keep the one-minute limit from it.
    uint64_t authenticated = 0;
    time_t authenticatedWall = 0;
    std::vector<CertificateDates> certificates;
    std::unique_ptr<HTTP2Session> h2;
    std::vector<uint8_t> h2Output;
    size_t h2Sent = 0;

    explicit Connection(const Request &request)
        : host(request.host), port(request.port), tls(request.tls) {
        mbedtls_ssl_init(&ssl);
    }

    ~Connection() {
        if (handle >= 0) sos2_net_close(handle);
        mbedtls_ssl_free(&ssl);
    }

    bool reusable() const {
        user_net_status status{};
        if (handle < 0 || sos2_net_status(handle, &status) || status.state != NET_CONNECTED ||
            (status.flags & NET_READ_EOF))
            return false;
        if (tls &&
            (certificates.empty() || !std::all_of(certificates.begin(), certificates.end(),
                                                  [](auto &dates) { return dates.valid(); })))
            return false;
        auto drift =
            int64_t(time(nullptr)) - verifiedWall - int64_t((milliseconds() - verifiedAt) / 1000);
        if (tls && (drift < -2 || drift > 2)) return false;
        return h2 ? h2->acceptsRequests()
                  : !status.readable && (!tls || !mbedtls_ssl_get_bytes_avail(&ssl));
    }

    void pollHTTP2();
};

struct Session {
    std::string host;
    unsigned port;
    uint64_t created = milliseconds();
    time_t wall = time(nullptr);
    mbedtls_ssl_session session;
    std::vector<CertificateDates> certificates;

    explicit Session(const Connection &c)
        : host(c.host), port(c.port), certificates(c.certificates) {
        mbedtls_ssl_session_init(&session);
    }

    ~Session() { mbedtls_ssl_session_free(&session); }
};

// Short-lived, single-use, in-memory tickets. They never cross page profiles,
// hosts or ports, and clock changes/certificate expiry invalidate them.
static std::vector<std::unique_ptr<Session>> sessions;

static void saveSession(const Connection &c) {
    if (c.certificates.empty() || mbedtls_ssl_get_verify_result(&c.ssl)) return;
    auto entry = std::make_unique<Session>(c);
    // A resumed connection was not freshly verified. Age its sessions from the
    // original verification, so resumption cannot extend the lifetime forever.
    if (c.authenticated) {
        entry->created = c.authenticated;
        entry->wall = c.authenticatedWall;
    }
    if (mbedtls_ssl_get_session(&c.ssl, &entry->session)) return;
    std::erase_if(sessions, [&](auto &old) { return old->host == c.host && old->port == c.port; });
    if (sessions.size() == 4) sessions.erase(sessions.begin());
    sessions.push_back(std::move(entry));
}

static void resumeSession(Connection &c) {
    auto now = milliseconds();
    auto wall = time(nullptr);
    std::erase_if(sessions, [&](auto &entry) {
        auto drift = int64_t(wall) - entry->wall - int64_t((now - entry->created) / 1000);
        return now - entry->created > 60000 || drift < -2 || drift > 2 ||
               !std::all_of(entry->certificates.begin(), entry->certificates.end(),
                            [](auto &dates) { return dates.valid(); });
    });
    for (auto at = sessions.begin(); at != sessions.end(); ++at) {
        if ((*at)->host != c.host || (*at)->port != c.port) continue;
        if (!mbedtls_ssl_set_session(&c.ssl, &(*at)->session)) {
            c.certificates = (*at)->certificates;
            c.authenticated = (*at)->created;
            c.authenticatedWall = (*at)->wall;
        }
        sessions.erase(at);
        return;
    }
}

// Main-loop-owned and private to this profile. Six active requests plus two
// idle connections fit the kernel's eight-socket limit without unbounded pools.
static std::vector<std::shared_ptr<Connection>> idleConnections;
// Weak references let active HTTP/2 loads share a connection without retaining
// extra idle sockets beyond the same two-entry limit used by HTTP/1.1.
static std::vector<std::weak_ptr<Connection>> activeHTTP2;

// Called from the renderer loop: an idle page must not keep kernel sockets
// beyond the 15 s idle limit just because it starts no new request. Age-only
// and at most once per second, so it adds no syscalls to an idle loop.
void closeIdleConnections() {
    static uint64_t checked = 0;
    uint64_t now = milliseconds();
    if (now - checked < 1000) return;
    checked = now;
    std::erase_if(idleConnections, [&](auto &c) { return now - c->idle > 15000; });
}

static std::shared_ptr<Connection> takeConnection(const Request &request) {
    uint64_t now = milliseconds();
    time_t wall = time(nullptr);
    for (auto at = idleConnections.begin(); at != idleConnections.end();) {
        auto &c = **at;
        int64_t drift = int64_t(wall) - c.wall - int64_t((now - c.idle) / 1000);
        if (now - c.idle > 15000 || !c.reusable() || (c.tls && (drift < -2 || drift > 2))) {
            at = idleConnections.erase(at);
            continue;
        }
        ++at;
    }
    std::erase_if(activeHTTP2, [](auto &weak) { return weak.expired(); });
    for (auto &weak : activeHTTP2)
        if (auto c = weak.lock(); c && c->tls == request.tls && c->port == request.port &&
                                  c->host == request.host && c->reusable())
            return c;
    // Prefer the most recently used socket. Round-robin reuse needlessly keeps
    // old sockets alive and increases exposure to server idle timeouts.
    for (size_t index = idleConnections.size(); index; --index) {
        auto at = idleConnections.begin() + index - 1;
        auto &c = **at;
        if (c.tls == request.tls && c.port == request.port && c.host == request.host) {
            auto result = std::move(*at);
            idleConnections.erase(at);
            return result;
        }
    }
    return nullptr;
}

void Connection::pollHTTP2() {
    // Keep TLS retries on the identical output buffer, and service reads even
    // when writes are blocked: peer WINDOW_UPDATE frames unblock uploads.
    for (unsigned budget = 0; budget < 4; ++budget) {
        if (h2Output.empty()) {
            h2Output = h2->output();
            h2Sent = 0;
        }
        if (h2Output.empty()) break;
        size_t size = std::min<size_t>(4096, h2Output.size() - h2Sent);
        int result = mbedtls_ssl_write(&ssl, h2Output.data() + h2Sent, size);
        if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            saveSession(*this);
            continue;
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (result <= 0) {
            h2->fail("HTTP/2 TLS send failed");
            return;
        }
        h2Sent += result;
        if (h2Sent == h2Output.size()) h2Output.clear();
    }
    for (unsigned budget = 0; budget < 4; ++budget) {
        std::array<uint8_t, 4096> bytes;
        int result = mbedtls_ssl_read(&ssl, bytes.data(), bytes.size());
        if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            saveSession(*this);
            continue;
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) return;
        if (result <= 0) {
            h2->fail("HTTP/2 TLS connection ended");
            return;
        }
        h2->receive(std::span<const uint8_t>(bytes.data(), result));
    }
}

struct Client::Impl {
    Request request;
    Parser parser;
    std::shared_ptr<Connection> connection;
    std::shared_ptr<HTTP2Stream> h2Stream;
    uint64_t h2Activity = 0;
    int code = 0;

    enum State { Connecting, Handshake, Sending, Receiving, Finished, Failed } state = Connecting;

    uint64_t started = milliseconds(), progress = started;
    size_t sent = 0;
    std::vector<uint8_t> outgoing;
    std::vector<uint8_t> decoded;
    size_t decodedBytes = 0;
    z_stream inflater{};
    bool encodingReady = false, inflateActive = false, inflateFinished = false, reused = false,
         retried = false, retrySafe = false;
    std::string failure;

    explicit Impl(Request value) : request(std::move(value)), parser(request.method == "HEAD") {
        retrySafe = (request.method == "GET" || request.method == "HEAD") && request.body.empty();
        if (request.host.empty() || request.host.size() > 253 || !validText(request.host, false) ||
            request.host.find_first_of("/:@[]\\") != std::string::npos || !request.port ||
            request.port > 65535 || request.target.empty() || request.target[0] != '/' ||
            request.target.size() > 8192 || !validText(request.target, false) ||
            !validToken(request.method) || request.method == "CONNECT" ||
            request.method == "TRACE" || request.body.size() > maximumBody) {
            fail("Unsupported or invalid HTTP request", -3);
            return;
        }
        std::string wire = request.method + " " + request.target +
                           " HTTP/1.1\r\nHost: " + request.host + ":" +
                           std::to_string(request.port) + "\r\nAccept-Encoding: gzip, deflate\r\n";
        Headers forwarded;
        for (const auto &header : request.headers) {
            if (!validToken(header.first) || !validText(header.second)) {
                fail("Invalid HTTP request header", -3);
                return;
            }
            auto name = lower(header.first);
            if (name == "host" || name == "connection" || name == "content-length" ||
                name == "transfer-encoding" || name == "accept-encoding" ||
                name == "proxy-authorization" || name == "proxy-connection" || name == "upgrade" ||
                name == "te" || name == "trailer" || name == "expect")
                continue;
            if (wire.size() + header.first.size() + header.second.size() + 4 > maximumHeaders) {
                fail("Request headers exceed limit", -3);
                return;
            }
            wire += header.first + ": " + header.second + "\r\n";
            forwarded.push_back(header);
        }
        request.headers = std::move(forwarded);
        if (!request.body.empty() || request.method == "POST" || request.method == "PUT" ||
            request.method == "PATCH")
            wire += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
        wire += "\r\n";
        outgoing.assign(wire.begin(), wire.end());
        outgoing.insert(outgoing.end(), request.body.begin(), request.body.end());
        connection = takeConnection(request);
        if (connection) {
            reused = true;
            if (connection->h2) beginHTTP2();
            else state = Sending;
            return;
        }
        connect();
    }

    Response &response() { return h2Stream ? h2Stream->response : parser.response; }

    bool headersReady() const { return h2Stream ? h2Stream->headersReady : parser.headersReady(); }

    bool complete() const { return h2Stream ? h2Stream->complete : parser.complete(); }

    void beginHTTP2() {
        h2Stream = connection->h2->submit(request);
        state = Receiving;
    }

    void connect() {
        connection = std::make_shared<Connection>(request);
        if (request.tls) {
            // An unset RTC cannot silently turn certificate date checking off.
            if (time(nullptr) < 1735689600 || !tls().ready) {
                fail("HTTPS needs a valid clock and trusted root store", -2);
                return;
            }
            if (psa_crypto_init() != PSA_SUCCESS ||
                mbedtls_ssl_setup(&connection->ssl, &tls().config) ||
                mbedtls_ssl_set_hostname(&connection->ssl, request.host.c_str())) {
                fail("Unable to initialize verified TLS", -2);
                return;
            }
            mbedtls_ssl_set_bio(&connection->ssl, &connection->handle, sendBytes, receiveBytes,
                                nullptr);
            resumeSession(*connection);
        }
        openSocket();
    }

    // Returns false after failing the request. The kernel divides a bounded
    // socket table between pages: when this page is over its share or the table
    // is full, release our idle keep-alive sockets and retry from poll(),
    // within the request's progress timeout, instead of failing the load.
    bool openSocket() {
        connection->handle = sos2_net_connect(request.host.c_str(), request.port);
        if (connection->handle >= 0) return true;
        if (errno == EAGAIN || errno == ENOMEM || errno == EMFILE) {
            idleConnections.clear();
            return true;
        }
        fail("Network connection unavailable", -1);
        return false;
    }

    ~Impl() {
        close();
        if (inflateActive) inflateEnd(&inflater);
    }

    void fail(const char *why, int error) {
        failure = why;
        code = error;
        state = Failed;
        close();
    }

    void close() {
        if (connection && h2Stream) connection->h2->cancel(h2Stream);
        connection.reset();
    }

    bool retry(bool unprocessedHTTP2 = false) {
        // A server can close an idle keep-alive socket between the status check
        // and write. Retry once, only for bodyless GET/HEAD before any response.
        if ((!reused && !unprocessedHTTP2) || retried || !retrySafe || headersReady()) return false;
        retried = true;
        reused = false;
        close();
        sent = 0;
        parser = Parser(request.method == "HEAD");
        h2Stream.reset();
        h2Activity = 0;
        state = Connecting;
        progress = milliseconds();
        connect();
        return true;
    }

    void finish() {
        if ((h2Stream || parser.response.persistent) && connection->reusable()) {
            std::erase(idleConnections, connection);
            if (idleConnections.size() == 2) idleConnections.erase(idleConnections.begin());
            connection->idle = milliseconds();
            connection->wall = time(nullptr);
            idleConnections.push_back(std::move(connection));
        } else close();
        outgoing.clear();
        state = Finished;
    }

    void decode() {
        if (!headersReady() || state == Failed) return;
        auto &input = response().body;
        if (!encodingReady) {
            encodingReady = true;
            auto encoding = lower(response().header("content-encoding"));
            bool hasBody =
                request.method != "HEAD" && response().status != 204 && response().status != 304;
            if (hasBody && !encoding.empty() && encoding != "identity") {
                if (encoding != "gzip" && encoding != "deflate") {
                    fail("Unsupported HTTP content encoding", -3);
                    return;
                }
                if (inflateInit2(&inflater, encoding == "gzip" ? 31 : 15) != Z_OK) {
                    fail("Unable to initialize decompression", -3);
                    return;
                }
                inflateActive = true;
            }
        }
        if (!inflateActive) {
            decodedBytes += input.size();
            if (h2Stream) connection->h2->consume(h2Stream, input.size());
            decoded.insert(decoded.end(), input.begin(), input.end());
            input.clear();
            if (complete()) finish();
            return;
        }
        if (inflateFinished && !input.empty()) {
            fail("Data after compressed response", -3);
            return;
        }
        // Bound both output storage and CPU work per poll, including compressed
        // bombs. Keep encoded input until its output has been drained by WebCore.
        inflater.next_in = input.data();
        inflater.avail_in = input.size();
        for (unsigned budget = 0; !inflateFinished && decoded.size() < 65536 && budget < 4;
             ++budget) {
            std::array<uint8_t, 16384> buffer;
            inflater.next_out = buffer.data();
            inflater.avail_out = std::min(buffer.size(), 65536 - decoded.size());
            size_t capacity = inflater.avail_out;
            auto before = inflater.avail_in;
            int result = inflate(&inflater, complete() ? Z_FINISH : Z_NO_FLUSH);
            size_t count = capacity - inflater.avail_out;
            if (count > maximumBody - decodedBytes) {
                fail("Decoded HTTP body exceeds resource limit", -3);
                return;
            }
            decodedBytes += count;
            decoded.insert(decoded.end(), buffer.data(), buffer.data() + count);
            if (result == Z_STREAM_END) inflateFinished = true;
            else if (result != Z_OK && result != Z_BUF_ERROR) {
                fail("Invalid compressed response", -3);
                return;
            }
            if (before == inflater.avail_in && !count) {
                if (complete()) {
                    // An empty encoded body (e.g. a redirect with Content-Length: 0)
                    // is an empty response, not a truncated stream.
                    if (!inflater.total_in && input.empty()) {
                        inflateFinished = true;
                        break;
                    }
                    fail("Truncated compressed response", -3);
                    return;
                }
                break;
            }
        }
        size_t consumed = input.size() - inflater.avail_in;
        if (h2Stream) connection->h2->consume(h2Stream, consumed);
        input.erase(input.begin(), input.begin() + consumed);
        if (inflateFinished && !input.empty()) {
            fail("Data after compressed response", -3);
            return;
        }
        if (complete() && inflateFinished) finish();
    }

    void poll() {
        if (state == Finished || state == Failed) return;
        auto now = milliseconds();
        if (now - started > 120000 || now - progress > 30000) {
            fail("Network request timed out", -4);
            return;
        }
        if (h2Stream) {
            connection->pollHTTP2();
            if (h2Activity != h2Stream->activity) {
                h2Activity = h2Stream->activity;
                progress = now;
            }
            if (!h2Stream->error.empty()) {
                if (h2Stream->retryable && retry(true)) return;
                fail(h2Stream->error.c_str(), -3);
                return;
            }
            decode();
            return;
        }
        if (state == Receiving) {
            decode();
            if (state == Finished || state == Failed || decoded.size() >= 65536 ||
                !parser.response.body.empty())
                return;
        }
        if (state == Connecting) {
            if (connection->handle < 0 && (!openSocket() || connection->handle < 0)) return;
            user_net_status status{};
            if (sos2_net_status(connection->handle, &status) || status.state == NET_FAILED) {
                fail("Connection or DNS lookup failed", -1);
                return;
            }
            if (status.state != NET_CONNECTED) return;
            progress = now;
            state = request.tls ? Handshake : Sending;
        }
        if (state == Handshake) {
            // Several TLS states only advance internal bookkeeping. Making each
            // wait for a rendered frame can exceed a server's handshake timeout
            // while a complex page is laying out. Still bound work per visit.
            for (unsigned budget = 0;
                 budget < 4 && !mbedtls_ssl_is_handshake_over(&connection->ssl); ++budget) {
                int result = mbedtls_ssl_handshake_step(&connection->ssl);
                if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
                    return;
                if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                    saveSession(*connection);
                    continue;
                }
                if (result) {
                    fail("TLS handshake or certificate verification failed", -2);
                    return;
                }
            }
            if (!mbedtls_ssl_is_handshake_over(&connection->ssl)) return;
            if (mbedtls_ssl_get_verify_result(&connection->ssl)) {
                fail("TLS certificate verification failed", -2);
                return;
            }
            if (auto *certificate = mbedtls_ssl_get_peer_cert(&connection->ssl)) {
                connection->certificates.clear();
                for (; certificate; certificate = certificate->next)
                    connection->certificates.push_back(
                        {certificate->valid_from, certificate->valid_to});
            }
            if (connection->certificates.empty() ||
                !std::all_of(connection->certificates.begin(), connection->certificates.end(),
                             [](auto &dates) { return dates.valid(); })) {
                fail("TLS certificate validity unavailable or expired", -2);
                return;
            }
            if (mbedtls_ssl_get_version_number(&connection->ssl) == MBEDTLS_SSL_VERSION_TLS1_2)
                saveSession(*connection);
            connection->verifiedAt = milliseconds();
            connection->verifiedWall = time(nullptr);
            progress = now;
            state = Sending;
            const char *protocol = mbedtls_ssl_get_alpn_protocol(&connection->ssl);
            if (protocol && !strcmp(protocol, "h2")) {
                connection->h2 = std::make_unique<HTTP2Session>();
                activeHTTP2.push_back(connection);
                beginHTTP2();
                return;
            }
        }
        // Bound each visit even on an always-readable socket.
        for (unsigned budget = 0; budget < 4; ++budget) {
            if (state == Sending) {
                request.body.clear();
                size_t count = std::min<size_t>(outgoing.size() - sent, 4096);
                int result =
                    request.tls ? mbedtls_ssl_write(&connection->ssl, outgoing.data() + sent, count)
                                : sendBytes(&connection->handle, outgoing.data() + sent, count);
                if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
                    return;
                if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                    saveSession(*connection);
                    continue;
                }
                if (result <= 0) {
                    if (!retry()) fail("HTTP send failed", -1);
                    return;
                }
                sent += result;
                progress = now;
                if (sent == outgoing.size()) state = Receiving;
            } else if (state == Receiving) {
                std::array<uint8_t, 4096> buffer;
                int result = request.tls
                                 ? mbedtls_ssl_read(&connection->ssl, buffer.data(), buffer.size())
                                 : receiveBytes(&connection->handle, buffer.data(), buffer.size());
                if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
                    return;
                if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                    saveSession(*connection);
                    continue;
                }
                if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || (!request.tls && !result)) {
                    if (!parser.headersReady() && retry()) return;
                    parser.eof();
                } else if (result <= 0) {
                    if (retry()) return;
                    fail(request.tls ? "TLS stream ended without a complete response"
                                     : "HTTP receive failed",
                         -1);
                    return;
                } else {
                    parser.feed(buffer.data(), result);
                    progress = now;
                }
                if (!parser.error().empty()) {
                    fail(parser.error().c_str(), -3);
                    return;
                }
                decode();
                if (state == Finished || state == Failed || decoded.size() >= 65536 ||
                    !parser.response.body.empty())
                    return;
            } else return;
        }
    }
};

Client::Client(Request request) : impl(std::make_unique<Impl>(std::move(request))) { }

Client::~Client() = default;

void Client::poll() { impl->poll(); }

bool Client::done() const { return impl->state == Impl::Finished || impl->state == Impl::Failed; }

bool Client::headersReady() const { return impl->headersReady(); }

std::vector<uint8_t> Client::takeData() { return std::exchange(impl->decoded, {}); }

const std::string &Client::error() const { return impl->failure; }

int Client::errorCode() const { return impl->code; }

Response &Client::response() { return impl->response(); }
}
