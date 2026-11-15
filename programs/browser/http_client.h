#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SOS2::HTTP {
using Headers = std::vector<std::pair<std::string, std::string>>;
constexpr size_t maximumBody = 8 * 1024 * 1024;
constexpr size_t maximumHeaders = 32 * 1024;

struct Request {
    bool tls = true;
    std::string host, target = "/", method = "GET";
    unsigned port = 443;
    Headers headers;
    std::vector<uint8_t> body;
};

struct Response {
    unsigned status = 0;
    std::string reason;
    Headers headers;
    std::vector<uint8_t> body;
    bool persistent = false;
    bool http2 = false;
    std::string header(std::string_view) const;
};

// Incremental, bounded HTTP/1 response framing. No sockets or browser policy.
class Parser {
public:
    explicit Parser(bool head = false) : head(head) { }

    void feed(const uint8_t *, size_t);
    void eof();

    bool complete() const { return state == Done; }

    bool headersReady() const { return finalHeaders && failure.empty(); }

    size_t bodyBytes() const { return receivedBody; }

    std::vector<uint8_t> takeBody() { return std::exchange(response.body, {}); }

    const std::string &error() const { return failure; }

    Response response;

private:
    enum State {
        HeadersState,
        Fixed,
        UntilClose,
        ChunkSize,
        ChunkBody,
        ChunkEnd,
        Trailers,
        Done,
        Failed
    } state = HeadersState;

    bool head;
    bool finalHeaders = false;
    size_t remaining = 0, metadata = 0, receivedBody = 0;
    unsigned interim = 0;
    std::string pending, failure;
    void fail(const char *);
    void headers();
    void append(const uint8_t *, size_t);
};

// Called only from the browser's main event loop. poll never waits for a socket.
class Client {
public:
    explicit Client(Request);
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    void poll();
    bool done() const;
    bool headersReady() const;
    std::vector<uint8_t> takeData();
    const std::string &error() const;
    int errorCode() const;
    Response &response();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

std::string lower(std::string_view);
// Close keep-alive connections idle past their limit; call regularly from the main loop.
void closeIdleConnections();
}
