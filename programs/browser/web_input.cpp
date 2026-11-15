#include "config.h"
#include "web_input.h"
#include <WebCore/Document.h>
#include <WebCore/DocumentView.h>
#include <WebCore/DOMPasteAccess.h>
#include <WebCore/Editor.h>
#include <WebCore/EditorClient.h>
#include <WebCore/EventHandler.h>
#include <WebCore/HandleUserInputEventResult.h>
#include <WebCore/FrameDestructionObserverInlines.h>
#include <WebCore/FocusController.h>
#include <WebCore/HTMLFormControlElement.h>
#include <WebCore/HitTestResult.h>
#include <WebCore/KeyboardEvent.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/Node.h>
#include <WebCore/NodeDocument.h>
#include <WebCore/Page.h>
#include <WebCore/PageConfiguration.h>
#include <WebCore/PlatformKeyboardEvent.h>
#include <WebCore/PlatformMouseEvent.h>
#include <WebCore/PlatformWheelEvent.h>
#include <WebCore/PointerCaptureController.h>
#include <WebCore/PointerEvent.h>
#include <WebCore/ScrollingCoordinatorTypes.h>
#include <WebCore/UndoStep.h>
#include <wtf/text/MakeString.h>
#include <cstdio>
#include <sos2/services.h>

namespace SOS2 {
using namespace WebCore;
using namespace WTF;

static bool needsKeyboard(Element *element) {
    if (!element || !element->shouldUseInputMethod()) return false;
    if (auto *control = dynamicDowncast<HTMLFormControlElement>(element))
        return control->isMutable();
    return true;
}

static OptionSet<PlatformEvent::Modifier> modifiers(unsigned bits) {
    OptionSet<PlatformEvent::Modifier> result;
    if (bits & INPUT_SHIFT) result.add(PlatformEvent::Modifier::ShiftKey);
    if (bits & INPUT_CTRL) result.add(PlatformEvent::Modifier::ControlKey);
    if (bits & INPUT_ALT) result.add(PlatformEvent::Modifier::AltKey);
    if (bits & INPUT_META) result.add(PlatformEvent::Modifier::MetaKey);
    if (bits & INPUT_CAPS_LOCK) result.add(PlatformEvent::Modifier::CapsLockKey);
    return result;
}

// US physical layout. Named keys retain their DOM key/code and native location;
// text is produced only for printable keys and handled by WebKit's editor.
static PlatformKeyboardEvent keyboardEvent(const user_input_event &event) {
    struct Named {
        unsigned scan;
        const char *key, *code, *identifier;
        unsigned vk;
        char text;
    };

    static constexpr Named named[] = {
        {1, "Escape", "Escape", "U+001B", 27, 0},
        {14, "Backspace", "Backspace", "U+0008", 8, 0},
        {15, "Tab", "Tab", "U+0009", 9, '\t'},
        {28, "Enter", "Enter", "Enter", 13, '\r'},
        {96, "Enter", "NumpadEnter", "Enter", 13, '\r'},
        {29, "Control", "ControlLeft", "Control", 17, 0},
        {97, "Control", "ControlRight", "Control", 17, 0},
        {42, "Shift", "ShiftLeft", "Shift", 16, 0},
        {54, "Shift", "ShiftRight", "Shift", 16, 0},
        {56, "Alt", "AltLeft", "Alt", 18, 0},
        {100, "Alt", "AltRight", "Alt", 18, 0},
        {125, "Meta", "MetaLeft", "Meta", 91, 0},
        {126, "Meta", "MetaRight", "Meta", 92, 0},
        {58, "CapsLock", "CapsLock", "CapsLock", 20, 0},
        {102, "Home", "Home", "Home", 36, 0},
        {103, "ArrowUp", "ArrowUp", "Up", 38, 0},
        {104, "PageUp", "PageUp", "PageUp", 33, 0},
        {105, "ArrowLeft", "ArrowLeft", "Left", 37, 0},
        {106, "ArrowRight", "ArrowRight", "Right", 39, 0},
        {107, "End", "End", "End", 35, 0},
        {108, "ArrowDown", "ArrowDown", "Down", 40, 0},
        {109, "PageDown", "PageDown", "PageDown", 34, 0},
        {110, "Insert", "Insert", "Insert", 45, 0},
        {111, "Delete", "Delete", "U+007F", 46, 0},
    };

    static constexpr struct {
        unsigned scan;
        char plain, shift;
        const char *code;
        unsigned vk;
    } punctuation[] = {
        {12, '-', '_', "Minus", 189},       {13, '=', '+', "Equal", 187},
        {26, '[', '{', "BracketLeft", 219}, {27, ']', '}', "BracketRight", 221},
        {39, ';', ':', "Semicolon", 186},   {40, '\'', '"', "Quote", 222},
        {41, '`', '~', "Backquote", 192},   {43, '\\', '|', "Backslash", 220},
        {51, ',', '<', "Comma", 188},       {52, '.', '>', "Period", 190},
        {53, '/', '?', "Slash", 191},       {57, ' ', ' ', "Space", 32},
    };

    String key = "Unidentified"_s, code = "Unidentified"_s, identifier;
    unsigned vk = 0;
    char text = 0, plain = 0;
    bool shift = event.modifiers & INPUT_SHIFT, caps = event.modifiers & INPUT_CAPS_LOCK;
    unsigned scan = event.code;
    if (scan >= 16 && scan <= 25) plain = "qwertyuiop"[scan - 16];
    else if (scan >= 30 && scan <= 38) plain = "asdfghjkl"[scan - 30];
    else if (scan >= 44 && scan <= 50) plain = "zxcvbnm"[scan - 44];
    if (plain) {
        vk = plain - 'a' + 'A';
        text = (shift != caps) ? static_cast<char>(vk) : plain;
        char physical[] = {'K', 'e', 'y', static_cast<char>(vk), 0};
        code = String::fromUTF8(physical);
    } else if (scan >= 2 && scan <= 11) {
        plain = "1234567890"[scan - 2];
        text = shift ? "!@#$%^&*()"[scan - 2] : plain;
        vk = plain;
        char physical[] = {'D', 'i', 'g', 'i', 't', plain, 0};
        code = String::fromUTF8(physical);
    } else {
        for (auto &item : punctuation)
            if (item.scan == scan) {
                plain = item.plain;
                text = shift ? item.shift : plain;
                code = String::fromUTF8(item.code);
                vk = item.vk;
                break;
            }
    }
    if (plain) {
        char character[] = {text, 0}, id[8];
        key = String::fromUTF8(character);
        std::snprintf(id, sizeof(id), "U+%04X",
                      plain >= 'a' && plain <= 'z' ? plain - 'a' + 'A'
                                                   : static_cast<unsigned char>(plain));
        identifier = String::fromUTF8(id);
    } else {
        for (auto &item : named)
            if (item.scan == scan) {
                key = String::fromUTF8(item.key);
                code = String::fromUTF8(item.code);
                identifier = String::fromUTF8(item.identifier);
                vk = item.vk;
                text = item.text;
                break;
            }
        unsigned function = scan >= 59 && scan <= 68 ? scan - 58
                            : scan == 87             ? 11
                            : scan == 88             ? 12
                                                     : 0;
        if (function) {
            char name[4];
            std::snprintf(name, sizeof(name), "F%u", function);
            key = code = identifier = String::fromUTF8(name);
            vk = 111 + function;
        }
    }
    char characters[] = {text, 0};
    String value = text ? String::fromUTF8(characters) : emptyString();
    bool pressed = event.flags & INPUT_DOWN;
    return {pressed ? PlatformEvent::Type::KeyDown : PlatformEvent::Type::KeyUp,
            pressed ? value : emptyString(),
            pressed ? value : emptyString(),
            key,
            code,
            identifier,
            static_cast<int>(vk),
            !!(event.flags & INPUT_REPEAT),
            scan == 96,
            false,
            modifiers(event.modifiers),
            MonotonicTime::now()};
}

class NativeEditorClient final : public EditorClient {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NativeEditorClient);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(NativeEditorClient);
    UniqueRef<EditorClient> fallback;
    Vector<Ref<UndoStep>> undoSteps, redoSteps;
    bool redoing = false;

    static void clear(Vector<Ref<UndoStep>> &steps) {
        while (!steps.isEmpty()) {
            auto step = steps.takeLast();
            step->didRemoveFromUndoManager();
        }
    }

    static void append(Vector<Ref<UndoStep>> &steps, UndoStep &step) {
        if (steps.size() == 100) {
            auto old = steps[0].copyRef();
            steps.removeAt(0);
            old->didRemoveFromUndoManager();
        }
        steps.append(step);
    }

public:
    explicit NativeEditorClient(UniqueRef<EditorClient> &&fallback)
        : fallback(WTF::move(fallback)) { }

    ~NativeEditorClient() { clearUndoRedoOperations(); }

    bool shouldDeleteRange(const std::optional<SimpleRange> &) final { return true; }

    bool smartInsertDeleteEnabled() final { return false; }

    bool isSelectTrailingWhitespaceEnabled() const final { return false; }

    bool isContinuousSpellCheckingEnabled() final { return false; }

    void toggleContinuousSpellChecking() final { }

    bool isGrammarCheckingEnabled() final { return false; }

    void toggleGrammarChecking() final { }

    int spellCheckerDocumentTag() final { return -1; }

    bool shouldBeginEditing(const SimpleRange &) final { return true; }

    bool shouldEndEditing(const SimpleRange &) final { return true; }

    bool shouldInsertNode(Node &, const std::optional<SimpleRange> &, EditorInsertAction) final {
        return true;
    }

    bool shouldInsertText(const String &, const std::optional<SimpleRange> &,
                          EditorInsertAction) final {
        return true;
    }

    bool shouldChangeSelectedRange(const std::optional<SimpleRange> &,
                                   const std::optional<SimpleRange> &, Affinity, bool) final {
        return true;
    }

    bool shouldApplyStyle(const StyleProperties &, const std::optional<SimpleRange> &) final {
        return true;
    }

    void didApplyStyle() final { }

    bool shouldMoveRangeAfterDelete(const SimpleRange &, const SimpleRange &) final { return true; }

    void didBeginEditing() final { }

    void respondToChangedContents() final { }

    void respondToChangedSelection(LocalFrame *) final { }

    void didEndUserTriggeredSelectionChanges() final { }

    void updateEditorStateAfterLayoutIfEditabilityChanged() final { }

    void didEndEditing() final { }

    void willWriteSelectionToPasteboard(const std::optional<SimpleRange> &) final { }

    void didWriteSelectionToPasteboard() final { }

    void getClientPasteboardData(const std::optional<SimpleRange> &,
                                 Vector<std::pair<String, RefPtr<SharedBuffer>>> &) final { }

    DOMPasteAccessResponse requestDOMPasteAccess(DOMPasteAccessCategory, FrameIdentifier,
                                                 const String &) final {
        return DOMPasteAccessResponse::DeniedForGesture;
    }

    void discardedComposition(const Document &) final { }

    void canceledComposition() final { }

    void didUpdateComposition() final { }

    void registerUndoStep(UndoStep &step) final {
        if (!redoing) clear(redoSteps);
        append(undoSteps, step);
    }

    void registerRedoStep(UndoStep &step) final { append(redoSteps, step); }

    void clearUndoRedoOperations() final {
        clear(undoSteps);
        clear(redoSteps);
    }

    bool canCopyCut(LocalFrame *, bool allowed) const final { return allowed; }

    bool canPaste(LocalFrame *, bool allowed) const final { return allowed; }

    bool canUndo() const final { return !undoSteps.isEmpty(); }

    bool canRedo() const final { return !redoSteps.isEmpty(); }

    void undo() final {
        if (canUndo()) {
            auto step = undoSteps.takeLast();
            step->unapply();
        }
    }

    void redo() final {
        if (canRedo()) {
            auto step = redoSteps.takeLast();
            redoing = true;
            step->reapply();
            redoing = false;
        }
    }

    void handleKeyboardEvent(KeyboardEvent &event) final {
        if (!event.isTrusted() || event.defaultPrevented()) return;
        auto *native = event.underlyingPlatformEvent();
        auto *node = dynamicDowncast<Node>(event.target());
        RefPtr frame = node ? node->document().frame() : nullptr;
        if (!native || !frame) return;
        auto &editor = frame->editor();
        String command;
        bool raw = native->type() == PlatformEvent::Type::RawKeyDown;
        bool control = event.ctrlKey() || event.metaKey(), shift = event.shiftKey();
        if (raw) {
            auto key = event.key();
            if (control && !event.altKey()) {
                switch (native->windowsVirtualKeyCode()) {
                    case 'A': command = "SelectAll"_s; break;
                    case 'C': command = "Copy"_s; break;
                    case 'X': command = "Cut"_s; break;
                    case 'V': command = "Paste"_s; break;
                    case 'Z': command = shift ? "Redo"_s : "Undo"_s; break;
                    case 'Y': command = "Redo"_s; break;
                }
            }
            if (command.isEmpty() && editor.canEdit() && !event.altKey()) {
                if (key == "Backspace"_s)
                    command = control ? "DeleteWordBackward"_s : "DeleteBackward"_s;
                else if (key == "Delete"_s)
                    command = control ? "DeleteWordForward"_s : "DeleteForward"_s;
                else {
                    if (key == "ArrowLeft"_s) command = control ? "MoveWordLeft"_s : "MoveLeft"_s;
                    else if (key == "ArrowRight"_s)
                        command = control ? "MoveWordRight"_s : "MoveRight"_s;
                    else if (key == "ArrowUp"_s) command = "MoveUp"_s;
                    else if (key == "ArrowDown"_s) command = "MoveDown"_s;
                    else if (key == "Home"_s)
                        command =
                            control ? "MoveToBeginningOfDocument"_s : "MoveToBeginningOfLine"_s;
                    else if (key == "End"_s)
                        command = control ? "MoveToEndOfDocument"_s : "MoveToEndOfLine"_s;
                    if (shift && !command.isEmpty())
                        command = makeString(command, "AndModifySelection"_s);
                }
            }
        } else if (native->type() == PlatformEvent::Type::Char && editor.canEdit() && !control &&
                   !event.altKey()) {
            if (event.key() == "Enter"_s) command = shift ? "InsertLineBreak"_s : "InsertNewline"_s;
            else if (!native->text().isEmpty() && native->text().codeUnitAt(0) >= 32 &&
                     editor.insertText(native->text(), &event))
                event.setDefaultHandled();
        }
        if (!command.isEmpty() && editor.command(command).execute(&event))
            event.setDefaultHandled();
    }

    void handleInputMethodKeydown(KeyboardEvent &) final { }

    void textFieldDidBeginEditing(Element &) final { }

    void textFieldDidEndEditing(Element &) final { }

    void textDidChangeInTextField(Element &) final { }

    bool doTextFieldCommandFromEvent(Element &, KeyboardEvent *) final { return false; }

    void textWillBeDeletedInTextField(Element &) final { }

    void textDidChangeInTextArea(Element &) final { }

    void overflowScrollPositionChanged() final { }

    void subFrameScrollPositionChanged() final { }

    TextCheckerClient *textChecker() final { return fallback->textChecker(); }

    void updateSpellingUIWithGrammarString(const String &, const GrammarDetail &) final { }

    void updateSpellingUIWithMisspelledWord(const String &) final { }

    void showSpellingUI(bool) final { }

    bool spellingUIIsShowing() final { return false; }

    void setInputMethodState(Element *element) final { sos2_input_method(needsKeyboard(element)); }

    bool performTwoStepDrop(DocumentFragment &, const SimpleRange &, bool) final { return false; }
};

void configureInput(PageConfiguration &configuration) {
    configuration.editorClient =
        makeUniqueRef<NativeEditorClient>(WTF::move(configuration.editorClient));
}

class PointerEvent final : public PlatformMouseEvent {
public:
    PointerEvent(DoublePoint point, Type type, bool pressed, unsigned clicks,
                 OptionSet<Modifier> modifiers)
        : PlatformMouseEvent(point, point,
                             pressed || type == Type::MouseReleased ? MouseButton::Left
                                                                    : MouseButton::None,
                             type, clicks, modifiers, MonotonicTime::now(), 0,
                             SyntheticClickType::NoTap, MouseEventInputSource::UserDriven) {
        m_buttons = pressed ? 1 : 0;
    }
};

void Input::cancelPointer() {
    if (down) {
        auto &handler = page.localMainFrame()->eventHandler();
        handler.invalidateClick();
        page.pointerCaptureController().cancelPointer(mousePointerID, pointerPoint);
        PointerEvent cancel({-100, -100}, PlatformEvent::Type::MouseReleased, false, 0, {});
        handler.handleMouseReleaseEvent(cancel);
        // SOS2 currently exposes one pointer. Drop its cancelled capture state
        // so the next physical press starts a new gesture, even with no hit
        // target for the cleanup mouse release outside the viewport.
        page.pointerCaptureController().reset();
    }
    down = tracking = scrolling = nativeDrag = false;
    clicks = 0;
    lastClick = 0;
}

void Input::resizeViewport(int width, int height) {
    if (width != 480 || height < 1 || height > 752 || height == viewportHeight) return;
    viewportHeight = height;
    RefPtr frame = page.localMainFrame();
    frame->view()->resize(width, height);
    frame->document()->updateLayoutIgnorePendingStylesheets();
    if (auto *element = frame->document()->focusedElement(); needsKeyboard(element))
        element->scrollIntoViewIfNeeded(false);
}

void Input::handle(const user_input_event &event) {
    if (event.type == INPUT_VIEWPORT) {
        resizeViewport(event.x, event.y);
        return;
    }
    auto mods = modifiers(event.modifiers);
    PlatformKeyboardEvent::setCurrentModifierState(mods);
    if (event.type == INPUT_FOCUS || event.type == INPUT_RESET) {
        // Blur first, then cancel the gesture outside the page: never release a
        // held Space or pointer over a newly focused button after input loss.
        page.focusController().setFocused(false);
        page.focusController().setActive(false);
        cancelPointer();
        focused = event.flags & INPUT_FOCUSED;
        if (focused) {
            page.focusController().setActive(true);
            page.focusController().setFocused(true);
        }
        resizeViewport(event.x, event.y);
        return;
    }
    if (event.type == INPUT_POINTER) {
        if (event.flags & INPUT_CANCEL) {
            cancelPointer();
            return;
        }
        bool pressed = event.flags & INPUT_DOWN;
        IntPoint point(event.x, event.y);
        auto frame = page.localMainFrame();
        auto &handler = frame->eventHandler();
        if (pressed && event.code == 1) {
            tracking = true;
            scrolling = false;
            pressPoint = dragPoint = point;
            nativeDrag = frame->view()->scrollbarAtPoint(point) ||
                         handler
                             .hitTestResultAtPoint(
                                 frame->view()->windowToContents(point),
                                 {HitTestRequest::Type::ReadOnly, HitTestRequest::Type::Active})
                             .scrollbar();
            if (!nativeDrag) return;
        }
        if (tracking && !nativeDrag) {
            // Wait until release to deliver a tap. Crossing the drag threshold
            // pans content without clicking a link, selecting text, or opening
            // the keyboard for a field underneath the initial press.
            if (abs(point.x() - pressPoint.x()) > 6 || abs(point.y() - pressPoint.y()) > 6)
                scrolling = true;
            if (scrolling) {
                float dx = point.x() - dragPoint.x(), dy = point.y() - dragPoint.y();
                if (dx || dy) {
                    PlatformWheelEvent wheel(pressPoint, pressPoint, dx, dy, dx / 48, dy / 48,
                                             PlatformWheelEventGranularity::ScrollByPixelWheelEvent,
                                             false, false, false, false);
                    handler.handleWheelEvent(wheel,
                                             {WheelEventProcessingSteps::SynchronousScrolling,
                                              WheelEventProcessingSteps::BlockingDOMEventDispatch});
                    dragPoint = point;
                }
                if (!pressed) {
                    tracking = scrolling = false;
                    clicks = 0;
                    lastClick = 0;
                }
                return;
            }
            if (pressed) return;
            tracking = false;
            double now = MonotonicTime::now().secondsSinceEpoch().value();
            bool nearby =
                abs(point.x() - lastPoint.x()) <= 4 && abs(point.y() - lastPoint.y()) <= 4;
            clicks = nearby && now - lastClick < 0.5 ? clicks % 3 + 1 : 1;
            lastPoint = point;
            lastClick = now;
            PointerEvent tap(DoublePoint(point), PlatformEvent::Type::MousePressed, true, clicks,
                             mods);
            handler.handleMousePressEvent(tap);
            PointerEvent release(DoublePoint(point), PlatformEvent::Type::MouseReleased, false,
                                 clicks, mods);
            handler.handleMouseReleaseEvent(release);
            sos2_input_method(needsKeyboard(frame->document()->focusedElement()));
            return;
        }
        // Only an explicit press can begin a gesture after a focus/reset or
        // boundary cancellation; held motion cannot resurrect the old drag.
        if (pressed && !down && event.code != 1) return;
        auto type = pressed == down ? PlatformEvent::Type::MouseMoved
                    : pressed       ? PlatformEvent::Type::MousePressed
                                    : PlatformEvent::Type::MouseReleased;
        pointerPoint = point;
        if (type == PlatformEvent::Type::MousePressed) {
            double now = MonotonicTime::now().secondsSinceEpoch().value();
            bool nearby =
                abs(point.x() - lastPoint.x()) <= 4 && abs(point.y() - lastPoint.y()) <= 4;
            clicks = nearby && now - lastClick < 0.5 ? clicks % 3 + 1 : 1;
            lastPoint = point;
            lastClick = now;
        }
        PointerEvent mouse(DoublePoint(point), type, pressed,
                           type == PlatformEvent::Type::MouseMoved ? 0 : clicks, mods);
        if (type == PlatformEvent::Type::MousePressed) {
            handler.handleMousePressEvent(mouse);
            // Tapping an already-focused field also reopens a dismissed keyboard.
            auto *element = page.localMainFrame()->document()->focusedElement();
            sos2_input_method(needsKeyboard(element));
        } else if (type == PlatformEvent::Type::MouseReleased)
            handler.handleMouseReleaseEvent(mouse);
        else handler.mouseMoved(mouse);
        down = pressed;
        if (!pressed) tracking = nativeDrag = false;
    } else if (event.type == INPUT_KEY && focused) {
        auto key = keyboardEvent(event);
        if (RefPtr frame = page.focusController().focusedOrMainFrame())
            frame->eventHandler().keyEvent(key);
    } else if (event.type == INPUT_WHEEL) {
        // Virtio vertical ticks are positive upward; WebKit platform deltas use
        // that sign, then expose the opposite sign in DOM WheelEvent.deltaY.
        float dx = -event.delta_x * 48.0f, dy = event.delta_y * 48.0f;
        if (event.modifiers & INPUT_SHIFT) std::swap(dx, dy);
        PlatformWheelEvent wheel({event.x, event.y}, {event.x, event.y}, dx, dy, -event.delta_x,
                                 event.delta_y,
                                 PlatformWheelEventGranularity::ScrollByPixelWheelEvent,
                                 event.modifiers & INPUT_SHIFT, event.modifiers & INPUT_CTRL,
                                 event.modifiers & INPUT_ALT, event.modifiers & INPUT_META);
        page.localMainFrame()->eventHandler().handleWheelEvent(
            wheel, {WheelEventProcessingSteps::SynchronousScrolling,
                    WheelEventProcessingSteps::BlockingDOMEventDispatch});
    }
}
}
