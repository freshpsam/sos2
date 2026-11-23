#pragma once
#include "http_client.h"
#include <span>

namespace SOS2::HTTP {
// What a stream and a connection's streams together may send before they are
// consumed, so also the most a stream holds undecoded. The protocol's own
// 64 KiB a stream allowed one such burst a round trip: YouTube's 3.4 MB of
// scripts took 2 s of the Nexus 5's load.
constexpr int32_t streamWindow = 1024 * 1024, connectionWindow = 3 * 1024 * 1024;

// One bounded stream. The TLS connection and framing session are shared, while
// each ResourceHandle keeps its own response, decoder, deadline and policy.
struct HTTP2Stream {
    Response response;
    std::string error;
    bool headersReady = false, complete = false, retryable = false, head = false, ended = false;
    uint64_t activity = 0;
    int32_t id = 0;
    size_t received = 0, metadata = 0, sent = 0;
    unsigned interim = 0, status = 0;
    Headers block;
    std::vector<uint8_t> upload;
};

class HTTP2Session {
public:
    HTTP2Session();
    ~HTTP2Session();
    bool acceptsRequests() const;
    std::shared_ptr<HTTP2Stream> submit(Request &);
    void cancel(const std::shared_ptr<HTTP2Stream> &);
    void consume(const std::shared_ptr<HTTP2Stream> &, size_t);
    void receive(std::span<const uint8_t>);
    std::vector<uint8_t> output();
    void fail(const char *);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
