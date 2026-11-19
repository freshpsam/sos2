#ifndef SOS2_APPS_H
#define SOS2_APPS_H
#include "runtime_files.h"

/* Native apps. Built in: from the resource bundle, one directory each,
 * /apps/<id>/:
 *   program  static ELF built with the SDK (tools/build_native_apps.py)
 *   icon     APP_ICON_SIZE x APP_ICON_SIZE little-endian ARGB8888 pixels;
 *            alpha below 128 is transparent
 *   name     its launcher name, 1..APP_NAME_MAX printable ASCII characters
 * The phone's images build in none: apps are packages an installed phone
 * keeps in its app store (include/app_store.h). An app runs in a page slot
 * like a web page, but with its own profile, /profiles/apps/<id>, instead of
 * the browser's.
 *
 * The system's updater, /bin/sos2-updater (programs/updater), is no app: it
 * is part of the system like the browser, the desktop's UPDATES button opens
 * it, and it alone may call SYS_UPDATE. It runs in a page slot from a record
 * of its own, which no launcher lists, with the profile /profiles/updater. */
#define APP_LIMIT     10u
#define APP_ID_MAX    15u
#define APP_NAME_MAX  12u
#define APP_ICON_SIZE 48u

/* An app id, which also names its profile: 1..APP_ID_MAX lowercase letters,
 * digits and hyphens. */
static inline bool app_id_valid(const char *id) {
    size_t n = 0;
    for (; id[n]; ++n) {
        char c = id[n];
        if (n == APP_ID_MAX || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return n > 0;
}

/* A record stays where it is while the app is installed: processes and the
 * launcher hold pointers to it. */
struct app {
    char id[APP_ID_MAX + 1], name[APP_NAME_MAX + 1];
    const struct runtime_file *program; /* built in; NULL when installed */
    const uint32_t *icon;
    bool present, stored;
    bool system; /* the system's updater, not an app */
    /* Installed: built for another USER_ABI_VERSION, so it does not start. */
    bool stale;
    unsigned position;  /* installed: its app store position */
    uint32_t size, abi; /* installed: its package's size, ABI and SHA-256 */
    uint8_t sha256[32];
};

/* Scan the bundle once it is mapped, then the app store; invalid entries are
 * skipped. */
void apps_init(void);
/* The apps the launcher shows, built in first: at most APP_LIMIT, none stale. */
unsigned apps_count(void);
const struct app *app_at(unsigned index);
/* Every record present, stale ones too, and a lookup by id. */
unsigned apps_records(const struct app **out, unsigned capacity);
const struct app *apps_find(const char *id);
/* The record for an app store position, for app_store.c to fill or clear,
 * with the icon pixels it owns. */
struct app *apps_store_record(unsigned position, uint32_t **icon);
/* The system's updater, or NULL when the bundle has none. */
const struct app *apps_updater(void);
#endif
