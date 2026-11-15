#include "config.h"
#include "Pasteboard.h"
#include "PasteboardContext.h"
#include "StaticPasteboard.h"
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/StringHash.h>

namespace WebCore {
// Editing has a process-local text/HTML pasteboard. It does not access the
// host clipboard or another renderer; browser clipboard permissions stay off.
static HashMap<String, String> &contents() {
    static NeverDestroyed<HashMap<String, String>> data;
    return data.get();
}

Pasteboard::Pasteboard(std::unique_ptr<PasteboardContext> &&context)
    : m_context(WTF::move(context)) { }

std::unique_ptr<Pasteboard>
Pasteboard::createForCopyAndPaste(std::unique_ptr<PasteboardContext> &&context) {
    return makeUnique<Pasteboard>(WTF::move(context));
}

bool Pasteboard::hasData() { return !contents().isEmpty(); }

Vector<String> Pasteboard::typesSafeForBindings(const String &) {
    Vector<String> result;
    for (auto &type : contents().keys())
        if (isSafeTypeForDOMToReadAndWrite(type)) result.append(type);
    return result;
}

Vector<String> Pasteboard::typesForLegacyUnsafeBindings() {
    Vector<String> result;
    for (auto &type : contents().keys()) result.append(type);
    return result;
}

String Pasteboard::readOrigin() { return {}; }

String Pasteboard::readString(const String &type) { return contents().get(type); }

String Pasteboard::readStringInCustomData(const String &) { return {}; }

void Pasteboard::writeString(const String &type, const String &text) { contents().set(type, text); }

void Pasteboard::clear() { contents().clear(); }

void Pasteboard::clear(const String &type) { contents().remove(type); }

void Pasteboard::read(PasteboardPlainText &text, PlainTextURLReadingPolicy, std::optional<size_t>) {
    text.text = readString("text/plain"_s);
}

// The native editor consumes text/HTML via readString; platform rich-content
// readers and file/image clipboard transport are not available.
void Pasteboard::read(PasteboardWebContentReader &, WebContentReadingPolicy,
                      std::optional<size_t>) { }

void Pasteboard::read(PasteboardFileReader &, std::optional<size_t>) { }

void Pasteboard::write(const Color &) { }

void Pasteboard::write(const PasteboardURL &url) {
    clear();
    writeString("text/uri-list"_s, url.url.string());
    writePlainText(url.url.string(), CannotSmartReplace);
}

void Pasteboard::writeTrustworthyWebURLsPboardType(const PasteboardURL &) { }

void Pasteboard::write(const PasteboardImage &) { }

void Pasteboard::write(const PasteboardBuffer &) { }

void Pasteboard::write(const PasteboardWebContent &content) {
    clear();
    writePlainText(content.text, CannotSmartReplace);
    writeMarkup(content.markup);
}

void Pasteboard::writeCustomData(const Vector<PasteboardCustomData> &) { }

Pasteboard::FileContentState Pasteboard::fileContentState() {
    return FileContentState::NoFileOrImageData;
}

bool Pasteboard::canSmartReplace() { return false; }

void Pasteboard::writeMarkup(const String &markup) { writeString("text/html"_s, markup); }

void Pasteboard::writePlainText(const String &text, SmartReplaceOption) {
    writeString("text/plain"_s, text);
    writeString("text/plain;charset=utf-8"_s, text);
}
#if ENABLE(DRAG_SUPPORT)
// System transfers are not exposed. Give each attempted drag an independent
// empty pasteboard, never access the renderer's copy/paste contents implicitly.
std::unique_ptr<Pasteboard>
Pasteboard::createForDragAndDrop(std::unique_ptr<PasteboardContext> &&) {
    return makeUnique<StaticPasteboard>();
}

std::unique_ptr<Pasteboard> Pasteboard::create(const DragData &) {
    return makeUnique<StaticPasteboard>();
}

void Pasteboard::setDragImage(DragImage, const IntPoint &) { }
#endif
}
