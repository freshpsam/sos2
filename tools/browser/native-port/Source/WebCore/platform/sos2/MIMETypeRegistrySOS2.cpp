#include "config.h"
#include "MIMETypeRegistry.h"

namespace WebCore {
static constexpr TypeExtensionPair types[] = {
    {"text/html"_s, "html"_s},
    {"text/html"_s, "htm"_s},
    {"text/css"_s, "css"_s},
    {"text/javascript"_s, "js"_s},
    {"text/javascript"_s, "mjs"_s},
    {"application/json"_s, "json"_s},
    {"image/png"_s, "png"_s},
    {"image/jpeg"_s, "jpg"_s},
    {"image/jpeg"_s, "jpeg"_s},
    {"image/gif"_s, "gif"_s},
    {"image/webp"_s, "webp"_s},
    {"image/bmp"_s, "bmp"_s},
    {"image/x-icon"_s, "ico"_s},
    {"image/svg+xml"_s, "svg"_s},
    {"text/plain"_s, "txt"_s},
    {"text/xml"_s, "xml"_s},
    {"application/xhtml+xml"_s, "xhtml"_s},
    {"font/ttf"_s, "ttf"_s},
    {"font/otf"_s, "otf"_s},
};

String MIMETypeRegistry::mimeTypeForExtension(StringView extension) {
    for (auto &entry : types)
        if (equalIgnoringASCIICase(extension, entry.extension)) return entry.type;
    return emptyString();
}

String MIMETypeRegistry::preferredExtensionForMIMEType(const String &type) {
    for (auto &entry : types)
        if (equalIgnoringASCIICase(type, entry.type)) return entry.extension;
    return emptyString();
}

Vector<String> MIMETypeRegistry::extensionsForMIMEType(const String &type) {
    Vector<String> result;
    for (auto &entry : types)
        if (equalIgnoringASCIICase(type, entry.type)) result.append(entry.extension);
    return result;
}

bool MIMETypeRegistry::isApplicationPluginMIMEType(const String &) { return false; }
}
