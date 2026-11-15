#include "apps.h"
#include "app_store.h"
#include "platform.h"

/* Built-in apps first, then one record per app store position. Records
 * never move: processes and the launcher keep pointers to them. */
static struct app records[2 * APP_LIMIT];
static uint32_t store_icons[APP_LIMIT][APP_ICON_SIZE * APP_ICON_SIZE];
/* The system's updater, which no launcher lists. */
static struct app updater;

static const struct runtime_file *app_file(const char *id, const char *leaf) {
    char path[48] = "/apps/";
    size_t used = strlen(path), id_length = strlen(id), leaf_length = strlen(leaf);
    if (used + id_length + 1 + leaf_length >= sizeof(path)) return 0;
    memcpy(path + used, id, id_length);
    used += id_length;
    path[used++] = '/';
    memcpy(path + used, leaf, leaf_length + 1);
    return runtime_file_find(path);
}

void apps_init(void) {
    memset(records, 0, sizeof(records));
    updater = (struct app){.id = "updater", .name = "Updates", .system = true};
    updater.program = runtime_file_find("/bin/sos2-updater");
    updater.present = updater.program != 0;
    unsigned count = 0;
    static const char prefix[] = "/apps/", suffix[] = "/program";
    const size_t prefix_length = sizeof(prefix) - 1, suffix_length = sizeof(suffix) - 1;
    for (unsigned i = 0; i < runtime_files_count(); ++i) {
        const struct runtime_file *program = runtime_file_at(i);
        size_t n = strlen(program->name);
        if (n <= prefix_length + suffix_length || memcmp(program->name, prefix, prefix_length) ||
            strcmp(program->name + n - suffix_length, suffix))
            continue;
        struct app app = {0};
        size_t length = n - prefix_length - suffix_length;
        if (length > APP_ID_MAX) continue;
        memcpy(app.id, program->name + prefix_length, length);
        if (!app_id_valid(app.id)) continue;
        if (count == APP_LIMIT) continue;
        const struct runtime_file *icon = app_file(app.id, "icon"),
                                  *name = app_file(app.id, "name");
        if (!icon || icon->size != APP_ICON_SIZE * APP_ICON_SIZE * 4) continue;
        if (!name || !name->size || name->size > APP_NAME_MAX) continue;
        const char *text = runtime_file_data(name);
        bool printable = true;
        for (uint32_t k = 0; k < name->size; ++k)
            if (text[k] < ' ' || text[k] > '~') printable = false;
        if (!printable) continue;
        memcpy(app.name, text, name->size);
        app.program = program;
        app.icon = runtime_file_data(icon);
        app.present = true;
        records[count++] = app;
    }
    app_store_load();
}

static bool shown(const struct app *app) { return app->present && !app->stale; }

unsigned apps_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < 2 * APP_LIMIT && n < APP_LIMIT; ++i)
        if (shown(&records[i])) ++n;
    return n;
}

const struct app *app_at(unsigned index) {
    if (index >= APP_LIMIT) return 0;
    for (unsigned i = 0, n = 0; i < 2 * APP_LIMIT; ++i)
        if (shown(&records[i]) && n++ == index) return &records[i];
    return 0;
}

unsigned apps_records(const struct app **out, unsigned capacity) {
    unsigned n = 0;
    for (unsigned i = 0; i < 2 * APP_LIMIT && n < capacity; ++i)
        if (records[i].present) out[n++] = &records[i];
    return n;
}

const struct app *apps_find(const char *id) {
    for (unsigned i = 0; i < 2 * APP_LIMIT; ++i)
        if (records[i].present && !strcmp(records[i].id, id)) return &records[i];
    return 0;
}

const struct app *apps_updater(void) { return updater.present ? &updater : 0; }

struct app *apps_store_record(unsigned position, uint32_t **icon) {
    if (position >= APP_LIMIT) return 0;
    *icon = store_icons[position];
    return &records[APP_LIMIT + position];
}
