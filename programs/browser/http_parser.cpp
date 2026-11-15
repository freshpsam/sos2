#include "http_client.h"
#include <algorithm>
#include <charconv>
#include <limits>

namespace SOS2::HTTP {
std::string lower(std::string_view text) {
    std::string value(text);
    for (char &c : value)
        if (c >= 'A' && c <= 'Z') c += 32;
    return value;
}

static std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

static bool token(std::string_view text) {
    if (text.empty()) return false;
    for (unsigned char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos))
            return false;
    return true;
}

std::string Response::header(std::string_view name) const {
    std::string value;
    for (auto &h : headers)
        if (h.first == name) {
            if (!value.empty()) value += ", ";
            value += h.second;
        }
    return value;
}

void Parser::fail(const char *why) {
    failure = why;
    state = Failed;
}

void Parser::append(const uint8_t *data, size_t length) {
    if (length > maximumBody - receivedBody) {
        fail("HTTP body exceeds resource limit");
        return;
    }
    receivedBody += length;
    response.body.insert(response.body.end(), data, data + length);
}

void Parser::headers() {
    auto end = pending.find("\r\n");
    std::string_view status(pending.data(), end);
    if (end == std::string::npos || status.size() < 12 ||
        (status.substr(0, 9) != "HTTP/1.1 " && status.substr(0, 9) != "HTTP/1.0 ") ||
        (status.size() > 12 && status[12] != ' ') || status[9] < '1' || status[9] > '5' ||
        status[10] < '0' || status[10] > '9' || status[11] < '0' || status[11] > '9') {
        fail("Invalid HTTP status line");
        return;
    }
    response.status = (status[9] - '0') * 100 + (status[10] - '0') * 10 + status[11] - '0';
    bool http11 = status.substr(0, 8) == "HTTP/1.1";
    for (unsigned char c : status)
        if ((c < 32 && c != '\t') || c == 127) {
            fail("Invalid HTTP status text");
            return;
        }
    response.reason = status.size() > 12 ? std::string(status.substr(13)) : "";
    bool bodyless = head || response.status == 204 || response.status == 304;
    bool lengthSeen = false, transferSeen = false;
    for (size_t at = end + 2; at + 2 < pending.size();) {
        end = pending.find("\r\n", at);
        if (end == std::string::npos) {
            fail("Invalid HTTP header");
            return;
        }
        auto line = std::string_view(pending).substr(at, end - at);
        at = end + 2;
        auto colon = line.find(':');
        if (colon == std::string_view::npos || !token(line.substr(0, colon))) {
            fail("Invalid HTTP header name");
            return;
        }
        auto value = trim(line.substr(colon + 1));
        for (unsigned char c : value)
            if ((c < 32 && c != '\t') || c == 127) {
                fail("Invalid HTTP header value");
                return;
            }
        auto name = lower(line.substr(0, colon));
        if (name == "content-length") {
            if (lengthSeen || value.empty()) {
                fail("Duplicate or invalid Content-Length");
                return;
            }
            uint64_t declared = 0;
            auto result = std::from_chars(value.data(), value.data() + value.size(), declared);
            if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
                (!bodyless && declared > maximumBody)) {
                fail("Invalid or excessive Content-Length");
                return;
            }
            // Representation metadata can exceed size_t on ARM32. No body
            // bytes are expected for HEAD/204/304, so never narrow that value.
            remaining = bodyless ? 0 : static_cast<size_t>(declared);
            lengthSeen = true;
        }
        if (name == "transfer-encoding") {
            if (transferSeen || lower(value) != "chunked") {
                fail("Unsupported Transfer-Encoding");
                return;
            }
            transferSeen = true;
        }
        response.headers.emplace_back(std::move(name), value);
    }
    if (transferSeen && lengthSeen) {
        fail("Ambiguous HTTP response framing");
        return;
    }
    if (response.status < 200) {
        if (response.status == 101 || ++interim > 8 || lengthSeen || transferSeen) {
            fail("Unsupported HTTP interim response");
            return;
        }
        response = {};
        remaining = 0;
        state = HeadersState;
        return;
    }
    if (bodyless) state = Done;
    else if (transferSeen) state = ChunkSize;
    else if (lengthSeen) state = remaining ? Fixed : Done;
    else state = UntilClose;
    finalHeaders = true;
    bool close = false, keepAlive = false;
    auto connection = lower(response.header("connection"));
    std::string_view tokens(connection);
    while (!tokens.empty()) {
        size_t comma = tokens.find(',');
        auto name = trim(tokens.substr(0, comma));
        close |= name == "close";
        keepAlive |= name == "keep-alive";
        if (comma == std::string_view::npos) break;
        tokens.remove_prefix(comma + 1);
    }
    response.persistent = state != UntilClose && !close && (http11 || keepAlive);
}

void Parser::feed(const uint8_t *data, size_t length) {
    while (length && state != Failed) {
        if (state == Done) {
            fail("Data after HTTP response");
            return;
        }
        if (state == Fixed || state == ChunkBody || state == UntilClose) {
            size_t take = state == UntilClose ? length : std::min(length, remaining);
            append(data, take);
            if (state == Failed) return;
            data += take;
            length -= take;
            if (state != UntilClose) {
                remaining -= take;
                if (!remaining) state = state == Fixed ? Done : ChunkEnd;
            }
            continue;
        }
        char c = *data++;
        --length;
        if (++metadata > maximumHeaders) {
            fail("HTTP metadata exceeds limit");
            return;
        }
        pending += c;
        if (state == HeadersState) {
            // Interim and final header blocks share one budget. After the final
            // headers, each chunk-size line (and the last one with its
            // trailers) gets its own, so small chunks can't exhaust it.
            if (pending.size() >= 4 && pending.ends_with("\r\n\r\n")) {
                headers();
                pending.clear();
                if (finalHeaders) metadata = 0;
            }
            continue;
        }
        if (state == ChunkEnd) {
            if (pending.size() == 1 && c != '\r') {
                fail("Invalid chunk delimiter");
                return;
            }
            if (pending.size() == 2) {
                if (c != '\n') {
                    fail("Invalid chunk delimiter");
                    return;
                }
                pending.clear();
                state = ChunkSize;
                metadata = 0;
            }
            continue;
        }
        if (!pending.ends_with("\r\n")) continue;
        std::string_view line(pending.data(), pending.size() - 2);
        if (state == Trailers) {
            if (line.empty()) state = Done;
            else {
                auto colon = line.find(':');
                if (colon == std::string_view::npos || !token(line.substr(0, colon))) {
                    fail("Invalid HTTP trailer");
                    return;
                }
                auto name = lower(line.substr(0, colon));
                if (name == "content-length" || name == "transfer-encoding" ||
                    name == "content-encoding") {
                    fail("Forbidden HTTP trailer");
                    return;
                }
                for (unsigned char byte : line)
                    if ((byte < 32 && byte != '\t') || byte == 127) {
                        fail("Invalid HTTP trailer");
                        return;
                    }
            }
        } else {
            auto semi = line.find(';');
            auto size = line.substr(0, semi);
            if (size.empty() || size.size() > 8) {
                fail("Invalid HTTP chunk size");
                return;
            }
            auto result = std::from_chars(size.data(), size.data() + size.size(), remaining, 16);
            if (result.ec != std::errc() || result.ptr != size.data() + size.size() ||
                remaining > maximumBody - receivedBody) {
                fail("Invalid or excessive HTTP chunk");
                return;
            }
            for (unsigned char byte : line)
                if (byte < 32 || byte == 127) {
                    fail("Invalid HTTP chunk extension");
                    return;
                }
            state = remaining ? ChunkBody : Trailers;
        }
        pending.clear();
    }
}

void Parser::eof() {
    if (state == UntilClose) state = Done;
    else if (state != Done && state != Failed) fail("Truncated HTTP response");
}
}
