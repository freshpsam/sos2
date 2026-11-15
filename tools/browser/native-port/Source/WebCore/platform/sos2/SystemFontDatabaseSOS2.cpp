#include "config.h"
#include "SystemFontDatabase.h"
#include <wtf/NeverDestroyed.h>

namespace WebCore {
SystemFontDatabase &SystemFontDatabase::singleton() {
    static NeverDestroyed<SystemFontDatabase> database = SystemFontDatabase();
    return database.get();
}

auto SystemFontDatabase::platformSystemFontShorthandInfo(FontShorthand) -> SystemFontShorthandInfo {
    return {"DejaVu Sans"_s, 16, normalWeightValue()};
}

void SystemFontDatabase::platformInvalidate() { }
}
