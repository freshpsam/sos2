#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SOS2::State {
constexpr size_t originQuota = 5 * 1024 * 1024;
constexpr size_t profileQuota = 8 * 1024 * 1024;
using Items = std::vector<std::pair<std::u16string, std::u16string>>;

// Hosts supplied by WebKit are canonical ASCII/IDNA. Cookie sites are schemeful
// registrable domains; localStorage uses the complete scheme/host/port origin.
struct Address {
    std::string scheme, host, path = "/";
    unsigned port = 0; // Effective URL port: callers supply 80/443 when omitted.
    bool valid() const;

    bool secure() const { return scheme == "https"; }

    std::string origin() const;
};

std::string registrableDomain(std::string_view);
bool publicSuffix(std::string_view);
bool sameSite(const Address &, const Address &);

struct CookieContext {
    Address url, firstParty;
    bool sameSite = true, topLevel = false, safeMethod = true, dom = false, credentials = true;
};

class Store {
public:
    explicit Store(const std::string &path);
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;
    bool available();
    // The version of an origin's local storage, which changes when a page,
    // possibly another one, changes its items. Unless Cached, first catch up
    // with the other pages: at most every 250 ms, or Now.
    enum class Look { Cached, Recent, Now };
    int64_t version(std::string_view origin, Look = Look::Recent);
    bool read(const std::string &origin, Items &);
    bool set(const std::string &origin, const std::u16string &key, const std::u16string &value);
    bool remove(const std::string &origin, const std::u16string &key);
    bool clear(const std::string &origin);
    bool setCookie(const CookieContext &, std::string_view header, int64_t now);
    std::string cookies(const CookieContext &, int64_t now);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
