#include "app_store.h"
#include "kernel_io.h"
#include "platform.h"
#include "process.h"
#include "sha256.h"
#include "vm.h"
#include <stddef.h>

/* The store (include/app_store.h) is read at boot and on the serialized
 * service worker only, so one sector buffer serves both. Installing writes
 * the half its position does not use: header cleared, package written,
 * flushed and read back, header written with the next sequence and read
 * back. A position keeps its app through any interruption before that last
 * write. */
enum { WORK_US = 2000 };

static uint32_t sector[128], chunk[1024];
static uint64_t worked_at;

static void work(void) {
    if (time_us() - worked_at < WORK_US) return;
    kernel_io_wait();
    worked_at = time_us();
}

/* A package's SHA-256, in pieces the desktop runs between. */
static void digest(const unsigned char *data, uint32_t size, uint8_t out[32]) {
    struct sha256 hash;
    sha256_begin(&hash);
    for (uint32_t at = 0; at < size; at += 65536) {
        sha256_add(&hash, data + at, size - at < 65536 ? size - at : 65536);
        work();
    }
    sha256_end(&hash, out);
}

static bool available(void) {
    return system_store_sectors(SYSTEM_CACHE) >= APP_STORE_START + APP_STORE_SECTORS;
}

static uint64_t half_sector(unsigned position, unsigned half) {
    return APP_STORE_START + ((uint64_t)position * 2 + half) * APP_HALF_SECTORS;
}

/* A text field of length bytes: zero-terminated and zero-padded. */
static bool field(const char *text, size_t length) {
    const char *end = memchr(text, 0, length);
    if (!end) return false;
    for (const char *p = end; p < text + length; ++p)
        if (*p) return false;
    return true;
}

bool app_package_valid(const struct app_package *p, uint32_t size) {
    if (!field(p->id, sizeof(p->id)) || !app_id_valid(p->id) || !field(p->name, sizeof(p->name)))
        return false;
    size_t name = strlen(p->name);
    for (size_t i = 0; i < name; ++i)
        if (p->name[i] < ' ' || p->name[i] > '~') return false;
    return !memcmp(p->magic, "SOS2APP1", 8) && p->version == 1 && name && name <= APP_NAME_MAX &&
           p->icon_offset == APP_PACKAGE_HEADER && p->icon_size == APP_ICON_BYTES &&
           p->program_offset == APP_PROGRAM_OFFSET && p->program_size >= 52 &&
           p->program_size <= APP_PACKAGE_MAX - APP_PROGRAM_OFFSET &&
           p->size == APP_PROGRAM_OFFSET + p->program_size && p->size == size &&
           crc32_ieee(p, offsetof(struct app_package, crc32)) == p->crc32;
}

static bool half_read(unsigned position, unsigned half, struct app_store_half *out) {
    if (!system_store_read(SYSTEM_CACHE, half_sector(position, half), sector, 1)) return false;
    memcpy(out, sector, sizeof(*out));
    return !memcmp(out->magic, "SOS2APPS", 8) && out->version == 1 && !out->reserved &&
           out->size > APP_PROGRAM_OFFSET && out->size <= APP_PACKAGE_MAX &&
           field(out->id, sizeof(out->id)) && app_id_valid(out->id) &&
           crc32_ieee(out, offsetof(struct app_store_half, crc32)) == out->crc32;
}

/* The half holding a position's app: valid, with the later sequence; -1 for
 * none. */
static int active_half(unsigned position, struct app_store_half *out) {
    struct app_store_half halves[2];
    bool valid[2] = {half_read(position, 0, &halves[0]), half_read(position, 1, &halves[1])};
    int best = valid[0] && valid[1] ? ((int32_t)(halves[1].sequence - halves[0].sequence) > 0)
               : valid[1]           ? 1
               : valid[0]           ? 0
                                    : -1;
    if (best >= 0) *out = halves[best];
    return best;
}

/* A position's record from its package header and icon, as stored. */
static void load(unsigned position, unsigned half, const struct app_store_half *h) {
    uint32_t *icon;
    struct app *app = apps_store_record(position, &icon);
    uint64_t first = half_sector(position, half) + APP_HALF_HEADER / 512;
    struct app_package package;
    if (!system_store_read(SYSTEM_CACHE, first, sector, 1)) return;
    memcpy(&package, sector, sizeof(package));
    /* An id taken by a built-in app or an earlier position stays theirs. */
    if (!app_package_valid(&package, h->size) || strcmp(package.id, h->id) ||
        apps_find(package.id) ||
        !system_store_read(SYSTEM_CACHE, first + 1, icon, APP_ICON_BYTES / 512))
        return;
    *app = (struct app){.icon = icon,
                        .present = true,
                        .stored = true,
                        .stale = package.abi != USER_ABI_VERSION,
                        .position = position,
                        .size = h->size,
                        .abi = package.abi};
    memcpy(app->id, package.id, sizeof(app->id));
    memcpy(app->name, package.name, sizeof(app->name));
    memcpy(app->sha256, h->sha256, sizeof(app->sha256));
}

void app_store_load(void) {
    if (!available()) return;
    for (unsigned position = 0; position < APP_LIMIT; ++position) {
        struct app_store_half h;
        int half = active_half(position, &h);
        if (half >= 0) load(position, (unsigned)half, &h);
    }
}

bool app_store_program(const struct app *app, unsigned char **buffer, unsigned *pages,
                       const void **program, uint32_t *size) {
    struct app_store_half h;
    worked_at = time_us();
    if (!app || !app->stored || app->stale || !available()) return false;
    int half = active_half(app->position, &h);
    if (half < 0 || h.size != app->size || memcmp(h.sha256, app->sha256, 32)) return false;
    unsigned count = (h.size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned char *data = vm_cpu_alloc(count);
    if (!data) return false;
    uint64_t first = half_sector(app->position, (unsigned)half) + APP_HALF_HEADER / 512;
    uint32_t sectors = (h.size + 511) / 512;
    bool read = true;
    for (uint32_t done = 0; read && done < sectors; done += 8) {
        read = system_store_read(SYSTEM_CACHE, first + done, data + (size_t)done * 512,
                                 sectors - done < 8 ? sectors - done : 8);
        work();
    }
    uint8_t sha256[32];
    if (read) digest(data, h.size, sha256);
    struct app_package package;
    memcpy(&package, data, sizeof(package));
    if (!read || memcmp(sha256, h.sha256, 32) || !app_package_valid(&package, h.size) ||
        package.abi != USER_ABI_VERSION) {
        vm_cpu_free(data, count);
        return false;
    }
    *buffer = data;
    *pages = count;
    *program = data + package.program_offset;
    *size = package.program_size;
    return true;
}

/* Write and check sectors from data, the last one zero-padded. */
static bool write_back(uint64_t first, const unsigned char *data, uint32_t size) {
    uint32_t sectors = (size + 511) / 512;
    for (uint32_t done = 0; done < sectors; done += 8) {
        unsigned n = sectors - done < 8 ? sectors - done : 8;
        size_t at = (size_t)done * 512, bytes = size - at < n * 512u ? size - at : n * 512u;
        memset(chunk, 0, sizeof(chunk));
        memcpy(chunk, data + at, bytes);
        if (!system_store_write(SYSTEM_CACHE, first + done, chunk, n)) return false;
        work();
    }
    if (!system_store_sync()) return false;
    for (uint32_t done = 0; done < sectors; done += 8) {
        unsigned n = sectors - done < 8 ? sectors - done : 8;
        size_t at = (size_t)done * 512, bytes = size - at < n * 512u ? size - at : n * 512u;
        if (!system_store_read(SYSTEM_CACHE, first + done, chunk, n) ||
            memcmp(chunk, data + at, bytes))
            return false;
        work();
    }
    return true;
}

static bool header_write(unsigned position, unsigned half, const struct app_store_half *h) {
    memset(sector, 0, sizeof(sector));
    if (h) memcpy(sector, h, sizeof(*h));
    return system_store_write(SYSTEM_CACHE, half_sector(position, half), sector, 1) &&
           system_store_sync();
}

static const char storage_failed[] = "Storage failed; restart to try again";

int32_t app_store_install(const unsigned char *data, uint32_t size, const char **message) {
    struct app_package package;
    worked_at = time_us();
    memcpy(&package, data, sizeof(package));
    if (!available()) {
        *message = "This SOS2 installs no apps";
        return USER_ENODEV;
    }
    if (!app_package_valid(&package, size) || package.abi != USER_ABI_VERSION) {
        *message = "Not an app for this system";
        return USER_EINVAL;
    }
    const struct app *existing = apps_find(package.id);
    if (existing && !existing->stored) {
        *message = "Built into the system already";
        return USER_EEXIST;
    }
    if (existing && process_app_running(existing)) {
        *message = "Close the app first";
        return USER_EBUSY;
    }
    unsigned position = existing ? existing->position : APP_LIMIT;
    if (!existing) {
        const struct app *present[2 * APP_LIMIT];
        uint32_t *icon;
        for (unsigned p = 0; p < APP_LIMIT && position == APP_LIMIT; ++p)
            if (!apps_store_record(p, &icon)->present) position = p;
        if (position == APP_LIMIT || apps_records(present, 2 * APP_LIMIT) >= APP_LIMIT) {
            *message = "No room for another app";
            return USER_ENOSPC;
        }
    }
    struct app_store_half active;
    int current = active_half(position, &active);
    unsigned half = current == 0 ? 1 : 0;
    struct app_store_half h = {.magic = {'S', 'O', 'S', '2', 'A', 'P', 'P', 'S'},
                               .version = 1,
                               .sequence = current >= 0 ? active.sequence + 1 : 1,
                               .size = size};
    memcpy(h.id, package.id, sizeof(h.id));
    digest(data, size, h.sha256);
    h.crc32 = crc32_ieee(&h, offsetof(struct app_store_half, crc32));
    struct app_store_half stored;
    if (!header_write(position, half, 0) ||
        !write_back(half_sector(position, half) + APP_HALF_HEADER / 512, data, size) ||
        !header_write(position, half, &h) || !half_read(position, half, &stored) ||
        memcmp(&stored, &h, sizeof(h))) {
        *message = storage_failed;
        return USER_EIO;
    }
    /* The new record at once, between two yields of this worker. */
    uint32_t *icon;
    struct app *app = apps_store_record(position, &icon);
    memcpy(icon, data + APP_PACKAGE_HEADER, APP_ICON_BYTES);
    *app = (struct app){.icon = icon,
                        .present = true,
                        .stored = true,
                        .position = position,
                        .size = size,
                        .abi = package.abi};
    memcpy(app->id, package.id, sizeof(app->id));
    memcpy(app->name, package.name, sizeof(app->name));
    memcpy(app->sha256, h.sha256, sizeof(app->sha256));
    *message = "Installed; starts at next launch";
    return 0;
}

int32_t app_store_remove(const char *id, const char **message) {
    const struct app *app = apps_find(id);
    worked_at = time_us();
    if (!app) {
        *message = "Not installed";
        return USER_ENOENT;
    }
    if (!app->stored) {
        *message = "Built into the system";
        return USER_EINVAL;
    }
    if (process_app_running(app)) {
        *message = "Close the app first";
        return USER_EBUSY;
    }
    unsigned position = app->position;
    uint32_t *icon;
    struct app *record = apps_store_record(position, &icon);
    /* Off the launcher before this worker yields, so nothing starts it. */
    record->present = false;
    struct app_store_half h;
    int current = active_half(position, &h);
    /* The unused half first: until the other goes, the app stays whole. */
    if ((current >= 0 && (!header_write(position, current ? 0 : 1, 0) ||
                          !header_write(position, (unsigned)current, 0))) ||
        (current < 0 && (!header_write(position, 0, 0) || !header_write(position, 1, 0)))) {
        record->present = true;
        *message = storage_failed;
        return USER_EIO;
    }
    *record = (struct app){0};
    *message = "Removed";
    return 0;
}
