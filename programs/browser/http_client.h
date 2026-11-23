#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SOS2::HTTP {
using Headers = std::vector<std::pair<std::string, std::string>>;
// A response body, decoded: modern sites ship scripts past 8 MiB.
constexpr size_t maximumBody = 32 * 1024 * 1024;
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

// Called only from the network thread (Fetch). poll never waits for a socket.
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

struct FetchState;

// A request run on the browser's network thread, which owns every Client,
// socket, TLS session and HTTP/2 connection: it reads, decrypts and decodes
// while the main thread runs WebCore, which would otherwise leave the
// sockets unread through every long script or layout, until the kernel's
// TCP window closed. The main thread takes the progress with poll(); the
// rest reads what that took. Destroying a Fetch cancels its request.
class Fetch {
public:
    explicit Fetch(Request);
    ~Fetch();
    Fetch(const Fetch &) = delete;
    Fetch &operator=(const Fetch &) = delete;
    void poll();
    // Finished, with every decoded byte taken (an error reports itself first).
    bool done() const { return finished && data.empty(); }
    bool headersReady() const { return headers; }
    std::vector<uint8_t> takeData() { return std::exchange(data, {}); }
    const std::string &error() const { return failure; }
    int errorCode() const { return code; }
    Response &response() { return result; }

private:
    std::shared_ptr<FetchState> state;
    bool headers = false, finished = false;
    Response result;
    std::vector<uint8_t> data;
    std::string failure;
    int code = 0;
};

std::string lower(std::string_view);
}
