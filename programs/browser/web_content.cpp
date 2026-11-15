#include "config.h"
#include "web_content.h"
#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/MIMETypeRegistry.h>
#include <wtf/text/CString.h>
#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace SOS2 {
using namespace WebCore;
using namespace WTF;
static constexpr unsigned maximumResourceBytes = 8 * 1024 * 1024;

// The browser profile's local website, /data/www, which every page shares.
URL entryURL() { return URL("https://www.sos2.local/index.html"_s); }

bool isLocalURL(const URL &url) {
    auto expected = entryURL();
    return url.isValid() && url.protocolIs("https"_s) && url.host() == expected.host() &&
           (!url.port() || url.port() == 443) && url.user().isEmpty() && url.password().isEmpty();
}

static bool diskPath(const URL &url, std::string &result) {
    if (!isLocalURL(url)) return false;
    auto encoded = url.path().utf8();
    std::string decoded;
    auto hex = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (unsigned i = 0; i < encoded.length(); ++i) {
        unsigned char c = encoded.data()[i];
        if (c == '%') {
            if (i + 2 >= encoded.length()) return false;
            int a = hex(encoded.data()[i + 1]), b = hex(encoded.data()[i + 2]);
            if (a < 0 || b < 0) return false;
            c = (a << 4) | b;
            i += 2;
            if (c == '/' || c == '\\') return false;
        }
        if (c < 32 || c == 127 || c == '\\') return false;
        decoded.push_back(c);
    }
    if (decoded.empty() || decoded.front() != '/') return false;
    result = "/data/www";
    for (size_t at = 1; at < decoded.size();) {
        size_t end = decoded.find('/', at);
        if (end == std::string::npos) end = decoded.size();
        auto component = decoded.substr(at, end - at);
        if (component == "." || component == "..") return false;
        if (!component.empty()) {
            result += '/';
            result += component;
        }
        at = end + 1;
    }
    if (decoded.back() == '/') result += "/index.html";
    return result.size() <= 127;
}

ResourceError readLocalResource(const ResourceRequest &request, ResourceResponse &response,
                                Vector<uint8_t> &bytes) {
    const auto &url = request.url();
    std::string path;
    if (!diskPath(url, path) || request.httpMethod() != "GET"_s)
        return {"SOS2"_s, 1, url, "Request is outside the browser's local website"_s,
                ResourceError::Type::AccessControl};
    auto respond = [&](int status, const String &type, size_t size) {
        String encoding = type.startsWith("text/"_s) ? "UTF-8"_s : emptyString();
        response = ResourceResponse(URL(url), String(type), size, WTF::move(encoding));
        response.setHTTPStatusCode(status);
        response.setHTTPStatusText(status == 200   ? "OK"_s
                                   : status == 404 ? "Not Found"_s
                                                   : "Resource Unavailable"_s);
        response.setHTTPHeaderField(HTTPHeaderName::ContentType, type);
        response.setHTTPHeaderField(HTTPHeaderName::CacheControl, "no-store"_s);
        response.setHTTPHeaderField(HTTPHeaderName::XContentTypeOptions, "nosniff"_s);
    };
    auto unavailable = [&](int status, const char *reason) {
        // Navigation and fetch share the same HTTP-like response. A missing
        // entry document must remain readable instead of becoming a blank page.
        std::string html =
            "<!doctype html><meta charset=utf-8><title>Page unavailable</title>"
            "<body style='font:20px sans-serif;margin:24px'><h1>Page unavailable</h1><p>";
        html += reason;
        html += "</p>";
        bytes = Vector<uint8_t>(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(html.data()), html.size()));
        respond(status, "text/html"_s, bytes.size());
    };
    bytes.clear();
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) {
        unavailable(404, "This page was not found in this profile's writable storage.");
        return {};
    }
    struct stat info;
    if (fstat(fileno(file), &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<unsigned>(info.st_size) > maximumResourceBytes) {
        fclose(file);
        unavailable(413, "This resource cannot be opened or exceeds the size limit.");
        return {};
    }
    bytes.resize(static_cast<size_t>(info.st_size));
    size_t count = bytes.isEmpty() ? 0 : fread(bytes.mutableSpan().data(), 1, bytes.size(), file);
    bool failed = ferror(file) || count != bytes.size();
    if (fclose(file)) failed = true;
    if (failed) {
        bytes.clear();
        return {"SOS2"_s, 2, url, "Unable to read local resource"_s};
    }
    auto dot = path.find_last_of('.');
    auto slash = path.find_last_of('/');
    String mime;
    if (dot != std::string::npos && dot > slash)
        mime = MIMETypeRegistry::mimeTypeForExtension(String::fromUTF8(path.substr(dot + 1)));
    if (mime.isEmpty()) mime = "application/octet-stream"_s;
    respond(200, mime, bytes.size());
    return {};
}
}
