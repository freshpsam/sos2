#include "config.h"
#include "AXObjectCache.h"
#include "AccessibilityObject.h"
#include "FilterOperation.h"
#include "PlatformKeyboardEvent.h"

namespace WebCore {
OptionSet<PlatformEvent::Modifier> PlatformKeyboardEvent::currentStateOfModifierKeys() {
    return s_currentModifiers.value_or(OptionSet<Modifier>{});
}

void PlatformKeyboardEvent::disambiguateKeyDownEvent(Type type, bool backwardsCompatibility) {
    ASSERT(m_type == Type::KeyDown);
    ASSERT(type == Type::RawKeyDown || type == Type::Char);
    m_type = type;
    if (backwardsCompatibility) return;
    if (type == Type::RawKeyDown) {
        m_text = emptyString();
        m_unmodifiedText = emptyString();
    } else m_keyIdentifier = emptyString();
}

// PublicSuffixStore uses the browser's pinned list in web_storage.cpp.
void AXObjectCache::handleScrolledToAnchor(const Node &) { }

void AXObjectCache::frameLoadingEventPlatformNotification(RenderView *, AXLoadingEvent) { }

void AXObjectCache::postPlatformNotification(AccessibilityObject &, AXNotification) { }

void AXObjectCache::attachWrapper(AccessibilityObject &) { }

void AXObjectCache::nodeTextChangePlatformNotification(AccessibilityObject *, AXTextChange,
                                                       unsigned, const String &) { }

void AXObjectCache::platformHandleFocusedUIElementChanged(Element *, Element *) { }

void AXObjectCache::platformPerformDeferredCacheUpdate() { }

void AccessibilityObject::detachPlatformWrapper(AccessibilityDetachmentType) { }

bool AccessibilityObject::accessibilityIgnoreAttachment() const { return false; }

AccessibilityObjectInclusion AccessibilityObject::accessibilityPlatformIncludesObject() const {
    return AccessibilityObjectInclusion::DefaultBehavior;
}
}
