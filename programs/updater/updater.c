/* The updater, /bin/sos2-updater: part of the system, like the browser
 * (programs/browser), and opened by the desktop's UPDATES button. It checks
 * UPDATE_URL and the app index at APPS_URL over verified HTTPS, following
 * redirects, and shows the system release and each app next to what runs.
 * Installing hands a file to the kernel's update service (SYS_UPDATE,
 * kernel/protection/system_update.c), which checks it and, on a device
 * started from its installed system, installs it: the system into the boot
 * partition, an app into the app store. Apps can be
 * removed too. RESTART starts an installed system update; an app's update
 * starts with its next launch. One thread does it all: the network is
 * nonblocking, and each step is short. */
#define MBEDTLS_CONFIG_FILE <sos2/mbedtls_config.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <psa/crypto_extra.h>
#include <sos2/network.h>
#include <sos2/services.h>
#include "graphics.h"

/* tools/publish_pages.py checks that these are where it publishes: each
 * device's system and the apps for its architecture. The Galaxy Tab A7 Lite
 * is the AArch64 device (QEMU's AArch64 machine checks it too). */
#ifdef __aarch64__
#define UPDATE_URL "https://freshpsam.github.io/sos2/gta7lite/sos2-gta7lite.update"
#define APPS_URL   "https://freshpsam.github.io/sos2/apps-arm64/"
#define BOARD      "gta7litewifi"
#define DEVICE     "tablet"
#else
#define UPDATE_URL "https://freshpsam.github.io/sos2/redmi2/sos2-redmi2.update"
#define APPS_URL   "https://freshpsam.github.io/sos2/apps/"
#define BOARD      "redmi2-wt88047"
#define DEVICE     "phone"
#endif

enum {
    WIDTH = WEB_WIDTH,
    HEIGHT = WEB_HEIGHT,
    /* The desktop's Wi-Fi screen's measures: panels at 16, text at 20 and
     * 10 inside panels, the title at scale 2 with the status right of it,
     * 44-pixel buttons and 56-pixel rows. */
    MARGIN = 16,
    TEXT_X = 20,
    INSET = 10,
    INNER = WIDTH - 2 * MARGIN,
    HALF = (INNER - 16) / 2,
    TITLE_Y = 20,
    STATUS_X = TEXT_X + 7 * 12 + 24, /* after "UPDATES" */
    STATUS_W = WIDTH - MARGIN - STATUS_X,
    STATUS_CHARS = STATUS_W / 6,
    STATUS_Y = TITLE_Y - 6,
    STATUS_H = 28,
    SYSTEM_Y = 56,
    CHECK_Y = 164,
    BUTTON_H = 44,
    SYSTEM_BUTTONS_Y = 224,
    BAR_Y = 284,
    APPS_Y = 318,
    ROWS_Y = 348,
    ROW_H = 56,
    ROW_PITCH = 64,
    ROWS = 5,
    PAGE_Y = ROWS_Y + ROWS * ROW_PITCH,
    ROW_BUTTON_W = 124,
    ROW_BUTTON_H = 40,
    STALL_MS = 30000,
    REDIRECTS = 5,
    HEADERS_MAX = 16384,
    URL_MAX = 2048,
    INDEX_MAX = 8192,
    LISTED_MAX = 10, /* the launcher's limit, APP_LIMIT */
    APPS_MAX = 20
};

enum {
    BACKGROUND = 0x17232c,
    PANEL = 0x0f181d,
    TEXT = 0xf1f5f6,
    MUTED = 0x8fa4ad,
    BUTTON = 0x31515c,
    PRESSED = 0x4a6c78,
    DISABLED = 0x222c32,
    ACCENT = 0x61e0bd,
    WARNING = 0xef7766
};

/* Targets: the system and paging buttons, then each app's button. */
enum { CHECK, INSTALL_SYSTEM, RESTART, PREVIOUS_APPS, NEXT_APPS, ROW_BUTTON = 100 };

enum job { NONE, CHECKING_SYSTEM, CHECKING_APPS, INSTALLING, FINISHING };

/* Where a response's body goes: the update's header, the app index, or the
 * kernel through BEGIN and WRITE. */
enum sink { HEADER, INDEX, STREAM };

enum stage { CONNECT, HANDSHAKE, SEND, HEADERS, BODY };

enum action { ACTION_NONE, ACTION_INSTALL, ACTION_UPDATE, ACTION_REMOVE };

static uint32_t pixels[WIDTH * HEIGHT];
static struct framebuffer screen = {pixels, WIDTH, HEIGHT, WIDTH * 4, PIXEL_ARGB8888};

static struct user_update_info info;
static struct user_update_app installed[APPS_MAX];
static unsigned installed_count;
static char available[48], status[128];
static uint32_t available_size;
static bool system_checked, apps_checked, status_warning;
static enum job job;
static int pressed = -1;
static uint64_t drawn_at;
static bool focused;
static uint64_t info_checked_at;

/* The published apps, from index.txt. */
static struct listed {
    char id[16], name[16];
    unsigned abi;
    uint32_t size;
    uint8_t sha256[32];
} listed[LISTED_MAX];

static unsigned listed_count;

/* What the list shows: published apps first, then installed ones that are
 * not published, each with the one thing it can do now. */
static struct row {
    char id[16], name[16];
    const struct listed *listed;
    const struct user_update_app *app;
    enum action action;
    char detail[64];
} rows[LISTED_MAX + APPS_MAX];

static unsigned row_count, row_offset;

/* The download: one HTTPS/1.1 request at a time, Connection: close. */
static struct {
    enum stage stage;
    enum sink sink;
    char host[USER_NET_HOST_MAX + 1], path[URL_MAX];
    unsigned port, redirects;
    int handle;
    bool tls;
    mbedtls_ssl_context ssl;
    char request[URL_MAX + 512];
    size_t request_size, sent;
    char headers[HEADERS_MAX + 1];
    size_t header_size;
    long long length; /* of the body, or -1 for chunked */
    long long chunk;  /* bytes left in the current chunk; -1 while reading its size line */
    char line[32];
    size_t line_size;
    uint64_t body, activity;
} net = {.handle = -1};

/* A file's first 4 KiB, then the rest in kernel-sized pieces; or the index. */
static unsigned char head[USER_UPDATE_HEADER], piece[USER_UPDATE_WRITE_MAX];
static char index_text[INDEX_MAX + 1];
static size_t head_used, piece_used, index_used;
static bool begun;
static uint32_t total;
static char installing[16]; /* the name of the app being installed, or empty for the system */

static uint64_t milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

/* --- Verified TLS, as the browser sets it up (programs/browser/http_client.cpp). */

static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt roots;
static mbedtls_ssl_config tls_config;
static bool tls_ready;

static int entropy(void *unused, unsigned char *out, size_t size) {
    (void)unused;
    while (size) {
        size_t n = size < 256 ? size : 256;
        if (getentropy(out, n)) return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
        out += n;
        size -= n;
    }
    return 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)milliseconds(); }

psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *unused,
                                             uint8_t *output, size_t size, size_t *written) {
    (void)unused;
    *written = 0;
    while (*written < size) {
        size_t n = size - *written < MBEDTLS_CTR_DRBG_MAX_REQUEST ? size - *written
                                                                  : MBEDTLS_CTR_DRBG_MAX_REQUEST;
        if (mbedtls_ctr_drbg_random(&drbg, output + *written, n))
            return PSA_ERROR_INSUFFICIENT_ENTROPY;
        *written += n;
    }
    return PSA_SUCCESS;
}

static bool tls_init(void) {
    if (tls_ready) return true;
    static bool seeded, loaded, configured;
    static const unsigned char personal[] = "SOS2 Update";
    if (!seeded) {
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_x509_crt_init(&roots);
        mbedtls_ssl_config_init(&tls_config);
        if (mbedtls_ctr_drbg_seed(&drbg, entropy, 0, personal, sizeof(personal))) return false;
        seeded = true;
    }
    if (!loaded) {
        FILE *file = fopen("/certs/roots.pem", "rb");
        if (!file) return false;
        static unsigned char pem[384 * 1024 + 1];
        size_t size = fread(pem, 1, sizeof(pem) - 1, file);
        bool bad = ferror(file) || !size || size == sizeof(pem) - 1;
        fclose(file);
        if (bad) return false;
        pem[size] = 0;
        if (mbedtls_x509_crt_parse(&roots, pem, size + 1)) return false;
        loaded = true;
    }
    if (!configured) {
        static const char *protocols[] = {"http/1.1", 0};
        if (mbedtls_ssl_config_defaults(&tls_config, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) ||
            mbedtls_ssl_conf_alpn_protocols(&tls_config, protocols) ||
            psa_crypto_init() != PSA_SUCCESS)
            return false;
        mbedtls_ssl_conf_authmode(&tls_config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&tls_config, &roots, 0);
        mbedtls_ssl_conf_rng(&tls_config, mbedtls_ctr_drbg_random, &drbg);
        mbedtls_ssl_conf_min_tls_version(&tls_config, MBEDTLS_SSL_VERSION_TLS1_2);
        configured = true;
    }
    tls_ready = true;
    return true;
}

static int send_bytes(void *context, const unsigned char *data, size_t size) {
    int result =
        sos2_net_send(*(int *)context, data, size < USER_NET_IO_MAX ? size : USER_NET_IO_MAX);
    if (result < 0)
        return errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return result;
}

static int receive_bytes(void *context, unsigned char *data, size_t size) {
    int result =
        sos2_net_recv(*(int *)context, data, size < USER_NET_IO_MAX ? size : USER_NET_IO_MAX);
    if (result < 0)
        return errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return result;
}

/* --- What is installed and what is published. */

static bool installs(void) { return info.flags & USER_UPDATE_INSTALLS; }

static bool refresh_info(void) {
    bool changed = false;
    struct user_update_info next = {0};
    if (sos2_update(USER_UPDATE_INFO, &next, sizeof(next)) == 0) {
        changed = memcmp(&info, &next, sizeof(info)) != 0;
        info = next;
    }
    struct user_update_app apps[APPS_MAX];
    int count = sos2_update(USER_UPDATE_APPS, apps, sizeof(apps));
    if (count >= 0 && count <= APPS_MAX) {
        changed |= installed_count != (unsigned)count ||
                   memcmp(installed, apps, (size_t)count * sizeof(apps[0])) != 0;
        memcpy(installed, apps, (size_t)count * sizeof(apps[0]));
        installed_count = (unsigned)count;
    }
    info_checked_at = milliseconds();
    return changed;
}

static int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* index.txt: "SOS2 apps 1", then "id abi size sha256 name" per app. */
static bool read_index(void) {
    index_text[index_used] = 0;
    char *line = index_text, *end = strchr(line, '\n');
    if (!end || strncmp(line, "SOS2 apps 1", 11) || end != line + 11) return false;
    listed_count = 0;
    for (line = end + 1; *line && listed_count < LISTED_MAX; line = end + 1) {
        end = strchr(line, '\n');
        if (!end) return false;
        *end = 0;
        struct listed *l = &listed[listed_count];
        char digest[65];
        int name_at = 0;
        unsigned size;
        if (sscanf(line, "%15[a-z0-9-] %u %u %64[0-9a-f] %n", l->id, &l->abi, &size, digest,
                   &name_at) != 4 ||
            strlen(digest) != 64 || !name_at || !line[name_at] || strlen(line + name_at) > 12)
            return false;
        for (unsigned i = 0; i < 32; ++i)
            l->sha256[i] = (uint8_t)(hex(digest[2 * i]) << 4 | hex(digest[2 * i + 1]));
        l->size = size;
        memset(l->name, 0, sizeof(l->name));
        strcpy(l->name, line + name_at);
        ++listed_count;
    }
    return true;
}

static void megabytes(char *out, size_t size, uint64_t bytes) {
    unsigned tenths = (unsigned)((bytes + 50000) / 100000);
    snprintf(out, size, "%u.%u MB", tenths / 10, tenths % 10);
}

/* One row per app, published or installed. */
static void build_rows(void) {
    row_count = 0;
    for (unsigned i = 0; i < listed_count; ++i) {
        struct row *r = &rows[row_count++];
        *r = (struct row){.listed = &listed[i]};
        memcpy(r->id, listed[i].id, sizeof(r->id));
        memcpy(r->name, listed[i].name, sizeof(r->name));
    }
    for (unsigned i = 0; i < installed_count; ++i) {
        const struct user_update_app *a = &installed[i];
        struct row *r = 0;
        for (unsigned k = 0; k < row_count; ++k)
            if (!strcmp(rows[k].id, a->id)) r = &rows[k];
        if (!r) {
            r = &rows[row_count++];
            *r = (struct row){0};
            memcpy(r->id, a->id, sizeof(r->id));
            memcpy(r->name, a->name, sizeof(r->name));
        }
        r->app = a;
    }
    for (unsigned k = 0; k < row_count; ++k) {
        struct row *r = &rows[k];
        const struct listed *l = r->listed;
        const struct user_update_app *a = r->app;
        /* fits: the published copy is for this system; an installed app
         * stays removable whatever its published copy is for. */
        bool stored = a && (a->flags & USER_APP_STORED), fits = l && l->abi == USER_ABI_VERSION,
             current = stored && fits && !memcmp(a->sha256, l->sha256, 32);
        char size[16] = "";
        if (l) megabytes(size, sizeof(size), l->size);
        r->action = ACTION_NONE;
        if (a && !stored) snprintf(r->detail, sizeof(r->detail), "Built into this system");
        else if (l && !fits && !stored)
            snprintf(r->detail, sizeof(r->detail), "%s",
                     l->abi > USER_ABI_VERSION ? "Needs a newer system first"
                                               : "Built for an older system");
        else if (!installs())
            snprintf(r->detail, sizeof(r->detail), "%s; this SOS2 installs no apps", size);
        else if (stored && (a->flags & USER_APP_RUNNING))
            snprintf(r->detail, sizeof(r->detail), "Running; close it to change it");
        else if (current) {
            snprintf(r->detail, sizeof(r->detail), "Installed, the published one");
            r->action = ACTION_REMOVE;
        } else if (stored && fits) {
            snprintf(r->detail, sizeof(r->detail), "%s, %s", size,
                     (a->flags & USER_APP_STALE) ? "the installed one needs it" : "an update");
            r->action = ACTION_UPDATE;
        } else if (stored) {
            snprintf(r->detail, sizeof(r->detail), "Installed; %s",
                     (a->flags & USER_APP_STALE) ? "built for another system"
                     : l                         ? "its update needs another system"
                                                 : "no longer published");
            r->action = ACTION_REMOVE;
        } else {
            snprintf(r->detail, sizeof(r->detail), "%s, not installed", size);
            r->action = ACTION_INSTALL;
        }
    }
    /* Removing the last app on a page returns to the preceding page. */
    if (row_offset >= row_count) row_offset = row_count ? (row_count - 1) / ROWS * ROWS : 0;
}

/* --- Drawing. */

static void present(int x, int y, int width, int height) {
    (void)sos2_present_damage(pixels, (unsigned)x, (unsigned)y, (unsigned)width, (unsigned)height);
}

static bool busy(void) { return job != NONE; }

static const char *action_label(enum action action) {
    switch (action) {
        case ACTION_INSTALL: return "INSTALL";
        case ACTION_UPDATE: return "UPDATE";
        case ACTION_REMOVE: return "REMOVE";
        default: return 0;
    }
}

/* A device that installs nothing yet but restarts into its bootloader's
 * download mode (the Galaxy Tab A7 Lite) has that in INSTALL SYSTEM's place. */
static bool download_mode(void) {
    return !installs() && (info.flags & USER_UPDATE_DOWNLOAD_MODE);
}

static bool enabled(int target) {
    if (busy()) return false;
    if (target == INSTALL_SYSTEM) return download_mode() || (system_checked && installs());
    if (target == PREVIOUS_APPS) return row_offset > 0;
    if (target == NEXT_APPS) return row_offset + ROWS < row_count;
    if (target >= ROW_BUTTON)
        return target - ROW_BUTTON >= (int)row_offset &&
               target - ROW_BUTTON < (int)row_count &&
               target - ROW_BUTTON < (int)row_offset + ROWS &&
               rows[target - ROW_BUTTON].action != ACTION_NONE;
    return target == CHECK || target == RESTART;
}

static void target_rect(int target, int *x, int *y, int *width, int *height) {
    if (target >= ROW_BUTTON) {
        *x = MARGIN + INNER - ROW_BUTTON_W - (ROW_H - ROW_BUTTON_H) / 2;
        *y = ROWS_Y + (target - ROW_BUTTON - (int)row_offset) * ROW_PITCH +
             (ROW_H - ROW_BUTTON_H) / 2;
        *width = ROW_BUTTON_W;
        *height = ROW_BUTTON_H;
        return;
    }
    if (target == PREVIOUS_APPS || target == NEXT_APPS) {
        *x = target == PREVIOUS_APPS ? MARGIN : WIDTH - MARGIN - ROW_BUTTON_W;
        *y = PAGE_Y;
        *width = ROW_BUTTON_W;
        *height = BUTTON_H;
        return;
    }
    *x = target == RESTART ? MARGIN + HALF + 16 : MARGIN;
    *y = target == CHECK ? CHECK_Y : SYSTEM_BUTTONS_Y;
    *width = target == CHECK ? INNER : HALF;
    *height = BUTTON_H;
}

static const char *target_label(int target) {
    if (target >= ROW_BUTTON) return action_label(rows[target - ROW_BUTTON].action);
    if (target == PREVIOUS_APPS) return "PREVIOUS";
    if (target == NEXT_APPS) return "NEXT";
    if (target == CHECK) return "CHECK FOR UPDATES";
    if (target == RESTART) return "RESTART";
    if (download_mode()) return "DOWNLOAD MODE";
    return system_checked && !strcmp(available, info.release) ? "REINSTALL" : "INSTALL SYSTEM";
}

static void render_target(int target) {
    int x, y, width, height;
    target_rect(target, &x, &y, &width, &height);
    const char *label = target_label(target);
    if (!label) return;
    bool on = enabled(target),
         highlight = target == RESTART && on && (info.flags & USER_UPDATE_RESTART_DUE);
    uint32_t fill = !on ? DISABLED : pressed == target ? PRESSED : highlight ? ACCENT : BUTTON;
    gfx_rect(&screen, x, y, width, height, fill);
    gfx_text(&screen, x + (width - gfx_text_width(label, 2)) / 2, y + (height - 14) / 2, label, 2,
             !on         ? MUTED
             : highlight ? BACKGROUND
                         : TEXT);
}

/* Right of the title and right-aligned, as on the Wi-Fi screen: one line
 * beside it, or two around it, broken at a space, when it is longer. */
static void render_status(void) {
    gfx_rect(&screen, STATUS_X, STATUS_Y, STATUS_W, STATUS_H, BACKGROUND);
    char lines[2][STATUS_CHARS + 1];
    unsigned count = 0;
    for (const char *text = status; count < 2 && *text; ++count) {
        size_t n = strlen(text);
        if (n > STATUS_CHARS) {
            n = STATUS_CHARS;
            while (n > STATUS_CHARS / 2 && text[n] != ' ') --n;
            if (text[n] != ' ') n = STATUS_CHARS;
        }
        memcpy(lines[count], text, n);
        lines[count][n] = 0;
        text += n;
        while (*text == ' ') ++text;
    }
    for (unsigned i = 0; i < count; ++i)
        gfx_text(&screen, STATUS_X + STATUS_W - gfx_text_width(lines[i], 1),
                 count == 1 ? TITLE_Y + 4 : STATUS_Y + 4 + (int)i * 12, lines[i], 1,
                 status_warning ? WARNING : TEXT);
}

/* A download, then a system's boot-image write and read-back, as a fraction
 * of the bar. */
static void render_bar(void) {
    gfx_rect(&screen, MARGIN, BAR_Y, INNER, 10, PANEL);
    uint64_t done = 0, all = 0;
    if (job == INSTALLING) {
        done = net.body;
        all = total;
    } else if (job == FINISHING &&
               (info.state == USER_UPDATE_WRITING || info.state == USER_UPDATE_CHECKING)) {
        done = info.done;
        all = info.total;
    } else if (job == FINISHING || info.state == USER_UPDATE_DONE) {
        done = all = 1;
    }
    if (all)
        gfx_rect(&screen, MARGIN, BAR_Y, (int)(INNER * (done > all ? all : done) / all), 10,
                 ACCENT);
}

static void render_system(void) {
    gfx_rect(&screen, MARGIN, SYSTEM_Y, INNER, 92, PANEL);
    gfx_text(&screen, MARGIN + INSET, SYSTEM_Y + 10, "SYSTEM", 1, MUTED);
    gfx_text(&screen, MARGIN + INSET, SYSTEM_Y + 26,
             info.release[0] ? info.release : "Unknown release", 2, TEXT);
    char line[80], size[16];
    if (system_checked) {
        megabytes(size, sizeof(size), available_size);
        snprintf(line, sizeof(line), "Available: %s", available);
        gfx_text(&screen, MARGIN + INSET, SYSTEM_Y + 54, line, 1, TEXT);
        snprintf(line, sizeof(line), "%s%s", size,
                 !strcmp(available, info.release) ? ", the running release" : "");
        gfx_text(&screen, MARGIN + INSET, SYSTEM_Y + 70, line, 1, MUTED);
    } else gfx_text(&screen, MARGIN + INSET, SYSTEM_Y + 54, "Available: not checked yet", 1, MUTED);
}

static void render_rows(void) {
    gfx_rect(&screen, 0, APPS_Y, WIDTH, HEIGHT - APPS_Y, BACKGROUND);
    gfx_text(&screen, TEXT_X, APPS_Y, "APPS", 2, TEXT);
    if (!row_count)
        gfx_text(&screen, TEXT_X, ROWS_Y + 8,
                 apps_checked ? "No apps are published." : "Check for updates to see apps.", 1,
                 MUTED);
    for (unsigned k = row_offset; k < row_count && k < row_offset + ROWS; ++k) {
        int y = ROWS_Y + (int)(k - row_offset) * ROW_PITCH;
        gfx_rect(&screen, MARGIN, y, INNER, ROW_H, PANEL);
        gfx_text(&screen, MARGIN + INSET, y + 7, rows[k].name, 2, TEXT);
        gfx_text(&screen, MARGIN + INSET, y + 35, rows[k].detail, 1, MUTED);
        render_target(ROW_BUTTON + (int)k);
    }
    if (row_count > ROWS) {
        render_target(PREVIOUS_APPS);
        render_target(NEXT_APPS);
        char page[24];
        snprintf(page, sizeof(page), "%u / %u", row_offset / ROWS + 1,
                 (row_count + ROWS - 1) / ROWS);
        gfx_text(&screen, (WIDTH - gfx_text_width(page, 2)) / 2, PAGE_Y + (BUTTON_H - 14) / 2,
                 page, 2, MUTED);
    }
}

static void render(void) {
    build_rows();
    gfx_rect(&screen, 0, 0, WIDTH, HEIGHT, BACKGROUND);
    gfx_text(&screen, TEXT_X, TITLE_Y, "UPDATES", 2, TEXT);
    render_system();
    for (int i = CHECK; i <= RESTART; ++i) render_target(i);
    render_status();
    render_bar();
    render_rows();
    sos2_present(pixels, sizeof(pixels));
    drawn_at = milliseconds();
}

/* Status and bar, at most five times a second unless forced. */
static void render_progress(bool force) {
    if (!force && milliseconds() - drawn_at < 200) return;
    render_status();
    render_bar();
    const struct user_pixel_rect damage[2] = {{STATUS_X, STATUS_Y, STATUS_W, STATUS_H},
                                              {MARGIN, BAR_Y, INNER, 10}};
    (void)sos2_present_frame(pixels, 0, damage, 2);
    drawn_at = milliseconds();
}

static void say(bool warning, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void say(bool warning, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(status, sizeof(status), format, args);
    va_end(args);
    status_warning = warning;
}

/* --- The request. */

static void close_connection(void) {
    if (net.tls) mbedtls_ssl_free(&net.ssl);
    net.tls = false;
    if (net.handle >= 0) sos2_net_close(net.handle);
    net.handle = -1;
}

/* Ends the job with a message. A download that failed after BEGIN leaves the
 * kernel nothing to finish. */
static void stop(bool warning, const char *text) {
    close_connection();
    if (begun && job == INSTALLING) (void)sos2_update(USER_UPDATE_CANCEL, 0, 0);
    begun = false;
    refresh_info();
    say(warning, "%s", text);
    job = NONE;
    render();
}

/* Ends a job the kernel refused, with its reason when it gave one. */
static void refused(const char *text) {
    refresh_info();
    char reason[sizeof(info.message)];
    snprintf(reason, sizeof(reason), "%s",
             info.state == USER_UPDATE_FAILED && info.message[0] ? info.message : text);
    stop(true, reason);
}

/* https://host[:port]/path only. */
static bool parse_url(const char *url) {
    if (strncmp(url, "https://", 8)) return false;
    const char *host = url + 8, *end = host + strcspn(host, "/:?#");
    size_t length = (size_t)(end - host);
    if (!length || length > USER_NET_HOST_MAX) return false;
    memcpy(net.host, host, length);
    net.host[length] = 0;
    net.port = 443;
    if (*end == ':') {
        char *after;
        unsigned long port = strtoul(end + 1, &after, 10);
        if (after == end + 1 || !port || port > 65535) return false;
        net.port = (unsigned)port;
        end = after;
    }
    if (*end && *end != '/') return false;
    const char *path = *end ? end : "/";
    size_t path_length = strcspn(path, "#");
    if (path_length >= sizeof(net.path)) return false;
    for (size_t i = 0; i < path_length; ++i)
        if ((unsigned char)path[i] <= ' ' || path[i] == 127) return false;
    memcpy(net.path, path, path_length);
    net.path[path_length] = 0;
    return true;
}

static bool open_request(void) {
    close_connection();
    net.request_size =
        (size_t)snprintf(net.request, sizeof(net.request),
                         "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SOS2-Update/1.0\r\n"
                         "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
                         net.path, net.host);
    if (net.request_size >= sizeof(net.request)) return false;
    net.sent = net.header_size = net.line_size = 0;
    net.body = 0;
    net.length = 0;
    net.chunk = -1;
    net.activity = milliseconds();
    net.handle = sos2_net_connect(net.host, net.port);
    if (net.handle < 0) return false;
    mbedtls_ssl_init(&net.ssl);
    net.tls = true;
    if (mbedtls_ssl_setup(&net.ssl, &tls_config) || mbedtls_ssl_set_hostname(&net.ssl, net.host))
        return false;
    mbedtls_ssl_set_bio(&net.ssl, &net.handle, send_bytes, receive_bytes, 0);
    net.stage = CONNECT;
    return true;
}

/* Fetch url into sink for the job. */
static bool request(const char *url, enum sink sink) {
    head_used = piece_used = index_used = 0;
    begun = false;
    net.sink = sink;
    net.redirects = 0;
    if (!parse_url(url) || !open_request()) {
        stop(true, "Cannot reach the server");
        return false;
    }
    return true;
}

static bool network_ready(void) {
    struct user_net_info network;
    if (sos2_net_info(&network) || !(network.flags & NET_ADDRESS)) {
        say(true, "Connect to Wi-Fi first");
        render();
        return false;
    }
    /* An unset clock cannot turn certificate date checks off. */
    if (time(0) < 1735689600) {
        say(true, "No network time yet; try again soon");
        render();
        return false;
    }
    say(false, "Securing the connection...");
    render();
    if (!tls_init()) {
        say(true, "Secure connections unavailable");
        render();
        return false;
    }
    return true;
}

static void start(enum job next, const char *url, enum sink sink, uint32_t size) {
    if (!network_ready()) return;
    job = next;
    total = size;
    if (!request(url, sink)) return;
    say(false, "Connecting to %s...", net.host);
    render();
}

/* The system update's header (struct system_update_header in
 * include/system_store.h) names it: its magic, board, release and size. The
 * kernel checks the rest when installing. */
enum { SYSTEM_FILE_SIZE = 120 /* the header's total_size */ };

static bool read_system_header(void) {
    static const char board[16] = BOARD;
    struct {
        char magic[8];
        uint32_t version, header_size;
        char board[16], release[48];
    } fields;

    memcpy(&fields, head, sizeof(fields));
    uint32_t file_size;
    memcpy(&file_size, head + SYSTEM_FILE_SIZE, 4);
    if (memcmp(fields.magic, "SOS2UPDT", 8) || fields.version != 2 ||
        fields.header_size != USER_UPDATE_HEADER ||
        memcmp(fields.board, board, sizeof(board)) ||
        !memchr(fields.release, 0, sizeof(fields.release)) || !fields.release[0])
        return false;
    for (const char *c = fields.release; *c; ++c)
        if (*c < ' ' || *c > '~') return false;
    memcpy(available, fields.release, sizeof(available));
    available_size = file_size;
    return true;
}

static bool write_piece(void) {
    if (!piece_used) return true;
    int result = sos2_update(USER_UPDATE_WRITE, piece, (uint32_t)piece_used);
    piece_used = 0;
    return result == 0;
}

/* The checks: the system's header, then the app index. */
static void header_done(void) {
    if (!read_system_header()) {
        stop(true, "Not an SOS2 update for this device");
        return;
    }
    system_checked = true;
    close_connection();
    job = CHECKING_APPS;
    if (request(APPS_URL "index.txt", INDEX)) render_progress(true);
}

static void index_done(void) {
    close_connection();
    if (!read_index()) {
        stop(true, "App list damaged");
        return;
    }
    apps_checked = true;
    refresh_info();
    stop(false, !strcmp(available, info.release) ? "System up to date" : "System update available");
}

/* The body's bytes in order; false ends the request (done or failed). */
static bool deliver(const unsigned char *p, size_t n) {
    net.body += n;
    net.activity = milliseconds();
    if (net.sink == INDEX) {
        if (index_used + n > INDEX_MAX) {
            stop(true, "App list too long");
            return false;
        }
        memcpy(index_text + index_used, p, n);
        index_used += n;
        return true;
    }
    if (head_used < sizeof(head)) {
        size_t take = sizeof(head) - head_used < n ? sizeof(head) - head_used : n;
        memcpy(head + head_used, p, take);
        head_used += take;
        p += take;
        n -= take;
        if (head_used < sizeof(head)) return true;
        if (net.sink == HEADER) {
            header_done();
            return false;
        }
        if (sos2_update(USER_UPDATE_BEGIN, head, sizeof(head))) {
            refused("The system refused the file");
            return false;
        }
        /* The size the file's own header gives, should it differ from the
         * one checked. */
        memcpy(&total, head + (memcmp(head, "SOS2APP1", 8) ? SYSTEM_FILE_SIZE : 64), sizeof(total));
        begun = true;
    }
    while (n) {
        size_t take = sizeof(piece) - piece_used < n ? sizeof(piece) - piece_used : n;
        memcpy(piece + piece_used, p, take);
        piece_used += take;
        p += take;
        n -= take;
        if (piece_used == sizeof(piece) && !write_piece()) {
            refused("Update failed");
            return false;
        }
    }
    char done[16], all[16];
    megabytes(done, sizeof(done), net.body);
    megabytes(all, sizeof(all), total);
    say(false, "Downloading %s of %s...", done, all);
    render_progress(false);
    return true;
}

/* The whole body arrived. */
static void body_done(void) {
    close_connection();
    if (net.sink == INDEX) {
        index_done();
        return;
    }
    if (net.sink != STREAM || !begun || net.body != total) {
        stop(true, "Download ended early; try again");
        return;
    }
    if (!write_piece()) {
        refused("Update failed");
        return;
    }
    job = FINISHING;
    say(false, "Checking the download...");
    render_progress(true);
}

/* Chunked bodies: hex size line, data, CRLF; a zero size ends them. */
static bool deliver_chunked(const unsigned char *p, size_t n) {
    while (n) {
        if (net.chunk < 0) {
            char c = (char)*p++;
            --n;
            if (c == '\n') {
                net.line[net.line_size] = 0;
                net.line_size = 0;
                char *end;
                unsigned long long size = strtoull(net.line, &end, 16);
                if (end == net.line || size > 0x7fffffff) {
                    stop(true, "Server reply damaged");
                    return false;
                }
                if (!size) {
                    body_done();
                    return false;
                }
                net.chunk = (long long)size + 2; /* and its CRLF */
            } else if (c != '\r' && net.line_size < sizeof(net.line) - 1)
                net.line[net.line_size++] = c;
            continue;
        }
        size_t take = (size_t)net.chunk < n ? (size_t)net.chunk : n;
        size_t data = net.chunk > 2 ? (size_t)(net.chunk - 2) : 0;
        if (data > take) data = take;
        if (data && !deliver(p, data)) return false;
        net.chunk -= (long long)take;
        p += take;
        n -= take;
        if (!net.chunk) net.chunk = -1;
    }
    return true;
}

static bool header_value(const char *name, char *out, size_t size) {
    size_t length = strlen(name);
    for (const char *line = strstr(net.headers, "\r\n"); line && line[2];
         line = strstr(line + 2, "\r\n")) {
        const char *field = line + 2;
        if (strncasecmp(field, name, length) || field[length] != ':') continue;
        const char *value = field + length + 1;
        while (*value == ' ' || *value == '\t') ++value;
        size_t n = strcspn(value, "\r");
        while (n && (value[n - 1] == ' ' || value[n - 1] == '\t')) --n;
        if (n >= size) return false;
        memcpy(out, value, n);
        out[n] = 0;
        return true;
    }
    return false;
}

/* The status line and headers are complete: redirect, fail, or read the body. */
static bool begin_body(const unsigned char *rest, size_t n) {
    int code = 0;
    if (sscanf(net.headers, "HTTP/1.%*1[01] %3d", &code) != 1) {
        stop(true, "Server reply is not HTTP");
        return false;
    }
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
        static char location[URL_MAX + 1];
        if (++net.redirects > REDIRECTS || !header_value("location", location, sizeof(location))) {
            stop(true, "Too many redirects");
            return false;
        }
        /* A path keeps the host; anything else must be HTTPS. */
        if (location[0] == '/' && location[1] != '/') {
            if (strlen(location) >= sizeof(net.path)) {
                stop(true, "Redirect too long");
                return false;
            }
            strcpy(net.path, location);
        } else if (!parse_url(location)) {
            stop(true, "Redirect leaves HTTPS");
            return false;
        }
        if (!open_request()) stop(true, "Cannot follow the redirect");
        return false;
    }
    if (code != 200) {
        char text[64];
        snprintf(text, sizeof(text),
                 code == 404 ? "Nothing is published there yet (HTTP 404)."
                             : "The update server answered HTTP %d.",
                 code);
        stop(true, text);
        return false;
    }
    char value[64];
    if (header_value("content-encoding", value, sizeof(value)) && strcasecmp(value, "identity")) {
        stop(true, "Server compressed the file");
        return false;
    }
    if (header_value("transfer-encoding", value, sizeof(value))) {
        if (strcasecmp(value, "chunked")) {
            stop(true, "Unsupported server reply");
            return false;
        }
        net.length = -1;
    } else if (header_value("content-length", value, sizeof(value))) {
        char *end;
        net.length = strtoll(value, &end, 10);
        if (end == value || *end || net.length < 0 ||
            (net.sink != INDEX && net.length < (long long)sizeof(head))) {
            stop(true, "Server file too short");
            return false;
        }
    } else {
        stop(true, "Server sent no length");
        return false;
    }
    net.stage = BODY;
    if (!net.length) {
        body_done();
        return false;
    }
    if (!n) return true;
    return net.length < 0 ? deliver_chunked(rest, n) : deliver(rest, n);
}

static void fail_tls(int result, const char *what) {
    char text[96];
    uint32_t flags = net.tls ? mbedtls_ssl_get_verify_result(&net.ssl) : 0;
    if (flags && flags != (uint32_t)-1)
        snprintf(text, sizeof(text), "The update server's certificate failed (%#lx).",
                 (unsigned long)flags);
    else snprintf(text, sizeof(text), "%s (%d).", what, result);
    stop(true, text);
}

/* One bounded step of the request; true when it may do more at once. */
static bool step_request(void) {
    if (milliseconds() - net.activity > STALL_MS) {
        stop(true, "Connection stalled; try again");
        return false;
    }
    if (net.stage == CONNECT) {
        struct user_net_status state;
        if (sos2_net_status(net.handle, &state) || state.state == NET_FAILED) {
            stop(true, "Cannot reach the server");
            return false;
        }
        if (state.state != NET_CONNECTED) return false;
        net.stage = HANDSHAKE;
        net.activity = milliseconds();
    }
    if (net.stage == HANDSHAKE) {
        int result = mbedtls_ssl_handshake(&net.ssl);
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
            return false;
        if (result) {
            fail_tls(result, "The secure connection failed");
            return false;
        }
        net.stage = SEND;
        net.activity = milliseconds();
    }
    if (net.stage == SEND) {
        while (net.sent < net.request_size) {
            int result = mbedtls_ssl_write(&net.ssl, (const unsigned char *)net.request + net.sent,
                                           net.request_size - net.sent);
            if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
                return false;
            if (result <= 0) {
                fail_tls(result, "Sending the request failed");
                return false;
            }
            net.sent += (size_t)result;
        }
        net.stage = HEADERS;
        net.activity = milliseconds();
    }
    static unsigned char buffer[16384];
    int result = mbedtls_ssl_read(&net.ssl, buffer, sizeof(buffer));
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) return false;
    if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return true;
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || result == 0) {
        if (net.stage == BODY && net.length >= 0 && net.body == (uint64_t)net.length) body_done();
        else stop(true, "Connection closed early");
        return false;
    }
    if (result < 0) {
        fail_tls(result, "The connection failed");
        return false;
    }
    net.activity = milliseconds();
    const unsigned char *p = buffer;
    size_t n = (size_t)result;
    if (net.stage == HEADERS) {
        size_t take = HEADERS_MAX - net.header_size < n ? HEADERS_MAX - net.header_size : n;
        memcpy(net.headers + net.header_size, p, take);
        size_t before = net.header_size;
        net.header_size += take;
        net.headers[net.header_size] = 0;
        char *end = strstr(net.headers, "\r\n\r\n");
        if (!end) {
            if (net.header_size == HEADERS_MAX)
                stop(true, "Server headers too long");
            return net.header_size < HEADERS_MAX;
        }
        size_t used = (size_t)(end + 4 - net.headers) - before;
        end[2] = 0; /* keep the last header's CRLF for header_value */
        return begin_body(p + used, n - used);
    }
    if (net.length < 0) return deliver_chunked(p, n);
    if (net.body + n > (uint64_t)net.length) {
        stop(true, "Server sent too much");
        return false;
    }
    if (!deliver(p, n)) return false;
    if (net.body == (uint64_t)net.length) {
        body_done();
        return false;
    }
    return true;
}

/* The kernel checks, then installs; each call does a bounded part. */
static bool step_finish(void) {
    int result = sos2_update(USER_UPDATE_FINISH, 0, 0);
    refresh_info();
    if (result == USER_EAGAIN) {
        if (info.state == USER_UPDATE_WRITING || info.state == USER_UPDATE_CHECKING) {
            char done[16], all[16];
            megabytes(done, sizeof(done), info.done);
            megabytes(all, sizeof(all), info.total);
            say(false, "%s %s of %s; keep the " DEVICE " on",
                info.state == USER_UPDATE_WRITING ? "Writing" : "Verifying", done, all);
        }
        render_progress(true);
        return true;
    }
    if (result) {
        refused("Update failed");
        return false;
    }
    if (installing[0]) {
        char text[96];
        snprintf(text, sizeof(text), "%s: %s", installing, info.message);
        stop(false, text);
    } else stop(false, "Installed; restart to use it");
    return false;
}

static bool step(void) {
    if (job == CHECKING_SYSTEM || job == CHECKING_APPS || job == INSTALLING) return step_request();
    if (job == FINISHING) return step_finish();
    return false;
}

/* --- Input. */

static int target_at(int x, int y) {
    for (int t = CHECK; t <= (row_count > ROWS ? NEXT_APPS : RESTART); ++t) {
        int tx, ty, w, h;
        target_rect(t, &tx, &ty, &w, &h);
        if (x >= tx && x < tx + w && y >= ty && y < ty + h) return t;
    }
    for (int k = (int)row_offset; k < (int)row_count && k < (int)row_offset + ROWS; ++k) {
        int tx, ty, w, h;
        target_rect(ROW_BUTTON + k, &tx, &ty, &w, &h);
        if (x >= tx && x < tx + w && y >= ty && y < ty + h) return ROW_BUTTON + k;
    }
    return -1;
}

static void show_pressed(int target) {
    if (target == pressed) return;
    int previous = pressed;
    pressed = target;
    int both[2] = {previous, target};
    for (int i = 0; i < 2; ++i) {
        if (both[i] < 0) continue;
        int x, y, w, h;
        render_target(both[i]);
        target_rect(both[i], &x, &y, &w, &h);
        present(x, y, w, h);
    }
}

static void act_row(const struct row *r) {
    if (r->action == ACTION_REMOVE) {
        char id[16] = {0};
        memcpy(id, r->id, sizeof(id) - 1);
        int result = sos2_update(USER_UPDATE_REMOVE, id, sizeof(id));
        refresh_info();
        say(result != 0, "%s: %s", r->name, info.message[0] ? info.message : "not removed");
        render();
        return;
    }
    if (!r->listed) return;
    memcpy(installing, r->name, sizeof(installing));
    char url[sizeof(APPS_URL) + 32];
    snprintf(url, sizeof(url), APPS_URL "%s.sos2app", r->id);
    start(INSTALLING, url, STREAM, r->listed->size);
}

static void act(int target) {
    if (!enabled(target)) return;
    if (target == CHECK) {
        installing[0] = 0;
        start(CHECKING_SYSTEM, UPDATE_URL, HEADER, 0);
    } else if (target == INSTALL_SYSTEM && download_mode()) {
        say(false, "Restarting into download mode...");
        render();
        if (sos2_update(USER_UPDATE_DOWNLOAD, 0, 0)) {
            refresh_info();
            say(true, "%s", info.message[0] ? info.message : "Download mode failed");
            render();
        }
    } else if (target == INSTALL_SYSTEM) {
        installing[0] = 0;
        start(INSTALLING, UPDATE_URL, STREAM, available_size);
    } else if (target == PREVIOUS_APPS || target == NEXT_APPS) {
        if (target == PREVIOUS_APPS) row_offset -= ROWS;
        else row_offset += ROWS;
        render();
    } else if (target >= ROW_BUTTON) act_row(&rows[target - ROW_BUTTON]);
    else {
        say(false, "Restarting...");
        render();
        if (sos2_update(USER_UPDATE_RESTART, 0, 0)) {
            refresh_info();
            say(true, "%s", info.message[0] ? info.message : "Restart failed");
            render();
        }
    }
}

static int armed = -1;

static void handle(const struct user_input_event *event) {
    if (event->type == INPUT_RESET || event->type == INPUT_FOCUS ||
        (event->type == INPUT_POINTER && (event->flags & INPUT_CANCEL))) {
        armed = -1;
        show_pressed(-1);
        if (event->type == INPUT_RESET || event->type == INPUT_FOCUS) {
            focused = (event->flags & INPUT_FOCUSED) != 0;
            if (focused) {
                refresh_info();
                render();
            }
        }
        return;
    }
    if (event->type != INPUT_POINTER) return;
    int hit = target_at(event->x, event->y);
    if (hit >= 0 && !enabled(hit)) hit = -1;
    if (event->code == 1) {
        armed = hit;
        show_pressed(hit);
        if (hit >= 0) (void)sos2_haptic_tap();
    } else if (event->code == 2) {
        int target = armed;
        armed = -1;
        show_pressed(-1);
        if (target >= 0 && target == hit) act(target);
    } else if (event->flags & INPUT_DOWN) show_pressed(hit == armed ? armed : -1);
}

int main(void) {
    refresh_info();
    /* What ended the last update, such as a boot image left unwritten. */
    if (info.state == USER_UPDATE_FAILED && info.message[0]) say(true, "%s", info.message);
    else if (info.flags & USER_UPDATE_RESTART_DUE)
        say(false, "Installed; restart to use it");
    else if (download_mode())
        say(false, "Updates cannot install here yet");
    else if (!installs())
        say(false, "Running from RAM: updates cannot install");
    else say(false, "Ready");
    render();
    for (;;) {
        struct user_input_event event;
        while (sos2_next_event(&event) == 1) handle(&event);
        /* Closing an app can still be releasing its resources when focus
         * returns. Keep its actions current without another network check. */
        if (focused && !busy() && milliseconds() - info_checked_at >= 500 && refresh_info()) {
            armed = -1;
            show_pressed(-1);
            render();
        }
        bool more = false;
        for (unsigned budget = 0; budget < 8 && step(); ++budget) more = true;
        struct timespec pause = {0, (more ? 1 : busy() ? 4 : 16) * 1000 * 1000};
        nanosleep(&pause, 0);
    }
}
