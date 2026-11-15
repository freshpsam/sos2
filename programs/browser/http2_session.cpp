#include "http2_session.h"
#include <nghttp2/nghttp2.h>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unistd.h>

namespace SOS2::HTTP {
struct HTTP2Session::Impl {
    nghttp2_session *session = nullptr;
    bool failed = false, goaway = false;
    std::unordered_map<int32_t, std::weak_ptr<HTTP2Stream>> streams;

    std::shared_ptr<HTTP2Stream> stream(int32_t id) {
        auto at = streams.find(id);
        return at == streams.end() ? nullptr : at->second.lock();
    }

    void fail(const char *message) {
        failed = true;
        for (auto &[id, weak] : streams)
            if (auto value = weak.lock(); value && !value->complete) {
                if (value->error.empty()) value->error = message;
                value->complete = true;
            }
    }

    static Impl &self(void *context) { return *static_cast<Impl *>(context); }

    static int reject(HTTP2Stream &value, const char *message) {
        value.error = message;
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    static int beginHeaders(nghttp2_session *, const nghttp2_frame *frame, void *context) {
        if (auto value = self(context).stream(frame->hd.stream_id)) {
            value->block.clear();
            value->status = 0;
        }
        return 0;
    }

    static int header(nghttp2_session *, const nghttp2_frame *frame, const uint8_t *name,
                      size_t nameSize, const uint8_t *data, size_t size, uint8_t, void *context) {
        auto value = self(context).stream(frame->hd.stream_id);
        if (!value) return 0;
        if (value->metadata > maximumHeaders - 32 ||
            nameSize > maximumHeaders - value->metadata - 32 ||
            size > maximumHeaders - value->metadata - nameSize - 32)
            return reject(*value, "HTTP/2 headers exceed resource limit");
        value->metadata += nameSize + size + 32;
        std::string key(reinterpret_cast<const char *>(name), nameSize),
            text(reinterpret_cast<const char *>(data), size);
        if (key == ":status") {
            auto parsed = std::from_chars(text.data(), text.data() + text.size(), value->status);
            if (value->headersReady || text.size() != 3 || parsed.ec != std::errc() ||
                parsed.ptr != text.data() + text.size() || value->status < 100 ||
                value->status > 599 || value->status == 101)
                return reject(*value, "Invalid HTTP/2 status");
        } else if (value->headersReady) {
            // Trailers cannot replace framing, encoding or the response policy
            // already delivered to WebCore. Other trailer fields are ignored.
            if (key == "content-length" || key == "content-encoding" || key == "transfer-encoding")
                return reject(*value, "Forbidden HTTP/2 trailer");
        } else value->block.emplace_back(std::move(key), std::move(text));
        return 0;
    }

    static int invalidHeader(nghttp2_session *, const nghttp2_frame *frame, const uint8_t *, size_t,
                             const uint8_t *, size_t, uint8_t, void *context) {
        if (auto value = self(context).stream(frame->hd.stream_id))
            value->error = "Invalid HTTP/2 header";
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    static int frameReceived(nghttp2_session *, const nghttp2_frame *frame, void *context) {
        auto &state = self(context);
        if (frame->hd.type == NGHTTP2_GOAWAY) {
            state.goaway = true;
            for (auto &[id, weak] : state.streams)
                if (id > frame->goaway.last_stream_id)
                    if (auto value = weak.lock();
                        value && !value->complete && !value->headersReady) {
                        value->retryable = true;
                        value->complete = true;
                        value->error = "HTTP/2 GOAWAY before processing request";
                    }
        }
        auto value = state.stream(frame->hd.stream_id);
        if (!value) return 0;
        ++value->activity;
        // nghttp2 reports this frame before closing the stream; closed() uses
        // it to tell a complete response from a reset with NO_ERROR.
        if ((frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) &&
            (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
            value->ended = true;
        if (frame->hd.type == NGHTTP2_HEADERS && !value->headersReady) {
            if (value->status >= 200) {
                value->response.status = value->status;
                value->response.http2 = true;
                value->response.headers = std::move(value->block);
                value->headersReady = true;
                auto length = value->response.header("content-length");
                if (!value->head && value->status != 204 && value->status != 304 &&
                    !length.empty()) {
                    uint64_t count = 0;
                    auto parsed =
                        std::from_chars(length.data(), length.data() + length.size(), count);
                    if (parsed.ec != std::errc() || parsed.ptr != length.data() + length.size() ||
                        count > maximumBody) {
                        value->error = "Invalid or excessive HTTP/2 Content-Length";
                        nghttp2_submit_rst_stream(state.session, NGHTTP2_FLAG_NONE, value->id,
                                                  NGHTTP2_CANCEL);
                    }
                }
            } else if (!value->status || ++value->interim > 8) {
                value->error = "Invalid or excessive HTTP/2 interim responses";
                nghttp2_submit_rst_stream(state.session, NGHTTP2_FLAG_NONE, value->id,
                                          NGHTTP2_PROTOCOL_ERROR);
            }
        }
        return 0;
    }

    static int dataReceived(nghttp2_session *session, uint8_t, int32_t id, const uint8_t *data,
                            size_t size, void *context) {
        auto value = self(context).stream(id);
        // Bytes that will never be decoded must still return connection credit:
        // automatic WINDOW_UPDATE is disabled for this session.
        if (!value || !value->error.empty()) {
            nghttp2_session_consume_connection(session, size);
            return 0;
        }
        if (!value->headersReady || size > maximumBody - value->received ||
            size > 65535 - value->response.body.size()) {
            // nghttp2 ignores non-fatal errors from this callback, so reset the
            // stream here rather than returning one.
            value->error = "HTTP/2 body exceeds resource limit or precedes headers";
            nghttp2_session_consume_connection(session, size);
            nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, id, NGHTTP2_CANCEL);
            return 0;
        }
        value->received += size;
        ++value->activity;
        value->response.body.insert(value->response.body.end(), data, data + size);
        return 0;
    }

    static int closed(nghttp2_session *, int32_t id, uint32_t error, void *context) {
        auto &state = self(context);
        if (auto value = state.stream(id)) {
            value->complete = true;
            ++value->activity;
            if (error) {
                value->retryable = error == NGHTTP2_REFUSED_STREAM && !value->headersReady;
                if (value->error.empty())
                    value->error = "HTTP/2 stream reset (" + std::to_string(error) + ")";
            } else if (!value->headersReady && value->error.empty())
                value->error = "HTTP/2 stream ended without response headers";
            // RST_STREAM(NO_ERROR) is only a clean end after END_STREAM (RFC 9113
            // section 8.1); earlier, the body is truncated.
            else if (!value->ended && value->error.empty())
                value->error = "HTTP/2 stream reset before the response ended";
        }
        state.streams.erase(id);
        return 0;
    }

    static nghttp2_ssize readUpload(nghttp2_session *, int32_t id, uint8_t *out, size_t size,
                                    uint32_t *flags, nghttp2_data_source *, void *context) {
        auto value = self(context).stream(id);
        if (!value) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        size = std::min(size, value->upload.size() - value->sent);
        memcpy(out, value->upload.data() + value->sent, size);
        value->sent += size;
        ++value->activity;
        if (value->sent == value->upload.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
        return size;
    }

    Impl() {
        nghttp2_session_callbacks *callbacks = nullptr;
        nghttp2_option *options = nullptr;
        if (nghttp2_session_callbacks_new(&callbacks) || nghttp2_option_new(&options)) {
            nghttp2_session_callbacks_del(callbacks);
            failed = true;
            return;
        }
        nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, beginHeaders);
        nghttp2_session_callbacks_set_on_header_callback(callbacks, header);
        nghttp2_session_callbacks_set_on_invalid_header_callback(callbacks, invalidHeader);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, frameReceived);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, dataReceived);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, closed);
        nghttp2_session_callbacks_set_rand_callback(callbacks, [](uint8_t *bytes, size_t count) {
            while (count) {
                size_t part = std::min<size_t>(count, 256);
                if (getentropy(bytes, part)) abort();
                bytes += part;
                count -= part;
            }
        });
        nghttp2_option_set_no_auto_window_update(options, 1);
        nghttp2_option_set_max_continuations(options, 4);
        nghttp2_option_set_max_send_header_block_length(options, maximumHeaders);
        nghttp2_option_set_max_outbound_ack(options, 8);
        nghttp2_option_set_max_outbound_queue_size(options, 128);
        failed = nghttp2_session_client_new2(&session, callbacks, this, options) != 0;
        nghttp2_option_del(options);
        nghttp2_session_callbacks_del(callbacks);
        if (failed) return;
        nghttp2_settings_entry settings[] = {
            {NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
            {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, 4096},
            {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 6},
            {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, maximumHeaders}};
        if (nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings, std::size(settings)) ||
            nghttp2_session_set_local_window_size(session, NGHTTP2_FLAG_NONE, 0, 6 * 65535))
            failed = true;
    }

    ~Impl() { nghttp2_session_del(session); }
};

HTTP2Session::HTTP2Session() : impl(std::make_unique<Impl>()) { }

HTTP2Session::~HTTP2Session() = default;

bool HTTP2Session::acceptsRequests() const {
    return !impl->failed && !impl->goaway && impl->streams.size() < 6 &&
           nghttp2_session_check_request_allowed(impl->session);
}

std::shared_ptr<HTTP2Stream> HTTP2Session::submit(Request &request) {
    auto value = std::make_shared<HTTP2Stream>();
    value->head = request.method == "HEAD";
    if (!acceptsRequests()) {
        value->error = "HTTP/2 connection unavailable";
        value->complete = true;
        return value;
    }
    Headers headers = {{":method", request.method},
                       {":scheme", "https"},
                       {":authority", request.host + ":" + std::to_string(request.port)},
                       {":path", request.target},
                       {"accept-encoding", "gzip, deflate"}};
    for (auto &header : request.headers) headers.emplace_back(lower(header.first), header.second);
    if (!request.body.empty() || request.method == "POST" || request.method == "PUT" ||
        request.method == "PATCH")
        headers.emplace_back("content-length", std::to_string(request.body.size()));
    std::vector<nghttp2_nv> fields;
    for (auto &[name, text] : headers)
        fields.push_back(
            {reinterpret_cast<uint8_t *>(name.data()), reinterpret_cast<uint8_t *>(text.data()),
             name.size(), text.size(),
             uint8_t(name == "cookie" || name == "authorization" ? NGHTTP2_NV_FLAG_NO_INDEX
                                                                 : NGHTTP2_NV_FLAG_NONE)});
    value->upload = std::move(request.body);
    nghttp2_data_provider2 provider{{}, Impl::readUpload};
    value->id = nghttp2_submit_request2(impl->session, nullptr, fields.data(), fields.size(),
                                        value->upload.empty() ? nullptr : &provider, nullptr);
    if (value->id < 0) {
        value->error = "Unable to submit HTTP/2 request";
        value->complete = true;
    } else impl->streams.emplace(value->id, value);
    return value;
}

void HTTP2Session::cancel(const std::shared_ptr<HTTP2Stream> &value) {
    if (!value) return;
    // A peer may have already closed/reset the stream while WebCore was
    // deciding response policy. Return its unread bytes to connection credit
    // even when the stream itself no longer exists.
    consume(value, value->response.body.size());
    value->response.body.clear();
    if (value->complete) return;
    nghttp2_submit_rst_stream(impl->session, NGHTTP2_FLAG_NONE, value->id, NGHTTP2_CANCEL);
    impl->streams.erase(value->id);
    value->complete = true;
}

void HTTP2Session::consume(const std::shared_ptr<HTTP2Stream> &value, size_t size) {
    if (size && !impl->failed && nghttp2_session_consume(impl->session, value->id, size))
        impl->fail("HTTP/2 flow-control accounting failed");
}

void HTTP2Session::receive(std::span<const uint8_t> bytes) {
    if (impl->failed) return;
    auto used = nghttp2_session_mem_recv2(impl->session, bytes.data(), bytes.size());
    if (used < 0 || size_t(used) != bytes.size()) impl->fail("Invalid HTTP/2 connection framing");
    else if (!nghttp2_session_want_read(impl->session)) impl->fail("HTTP/2 connection ended");
}

std::vector<uint8_t> HTTP2Session::output() {
    if (impl->failed) return {};
    const uint8_t *bytes = nullptr;
    auto size = nghttp2_session_mem_send2(impl->session, &bytes);
    if (size < 0) {
        impl->fail("HTTP/2 send framing failed");
        return {};
    }
    return size ? std::vector<uint8_t>(bytes, bytes + size) : std::vector<uint8_t>();
}

void HTTP2Session::fail(const char *message) { impl->fail(message); }
}
