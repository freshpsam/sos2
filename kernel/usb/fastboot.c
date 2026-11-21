#include "fastboot.h"
#include <string.h>

/* The protocol is AOSP's fastboot README: one command per bulk OUT transfer,
 * answered by INFO lines and then OKAY or FAIL; a download or an upload is
 * answered by DATA and its size, then the bytes, then OKAY. Replies stay
 * under 64 bytes, so each is one short packet and ends its transfer at
 * either speed. Every device answers getvar (version, product and serialno,
 * then the board's own; all lists them) and reboot and reboot-bootloader,
 * when the board says it can restart; the rest are the board's commands.
 * Commands run in the kernel's main loop, or in a CPU 0 panic's
 * (kernel/platform.c). The USB IDs are fastboot's generic ones (18d1:d00d),
 * with strings 1-3 the manufacturer, the product's name and the serial
 * number. */
enum { RESPONSE = 63, TEXT = RESPONSE - 4 };

static const struct fastboot_device *device;
static bool stopped;

void fastboot_init(const struct fastboot_device *d) { device = d; }

void fastboot_panic(void) { stopped = true; }

bool fastboot_stopped(void) { return stopped; }

bool fastboot_reply(const char *kind, const char *text) {
    char packet[RESPONSE + 1];
    char *p = stpcpy(packet, kind);
    for (unsigned room = TEXT; *text && room; --room) *p++ = *text++;
    return device->send(packet, (uint32_t)(p - packet));
}

bool fastboot_starts(const char *text, const char *prefix) {
    size_t n = strlen(prefix);
    return strlen(text) >= n && !memcmp(text, prefix, n);
}

char *fastboot_hex32(char *p, uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; ++i) *p++ = digits[value >> (28 - 4 * i) & 15];
    *p = 0;
    return p;
}

static const char *variable(const char *name) {
    if (!strcmp(name, "version")) return "0.4";
    if (!strcmp(name, "product") || !strcmp(name, "serialno")) return device->product;
    return device->variable ? device->variable(name) : 0;
}

static void getvar(const char *name) {
    static const char *const common[] = {"version", "product", "serialno"};
    if (strcmp(name, "all")) {
        const char *value = variable(name);
        (void)fastboot_reply(value ? "OKAY" : "FAIL", value ? value : "unknown variable");
        return;
    }
    const char *const *board = device->variables;
    for (unsigned i = 0; i < 3 || (board && board[i - 3]); ++i) {
        const char *listed = i < 3 ? common[i] : board[i - 3], *value = variable(listed);
        if (!value) continue;
        char line[TEXT + 1];
        unsigned n = 0;
        for (const char *s = listed; *s && n < TEXT; ++s) line[n++] = *s;
        for (const char *s = ": "; *s && n < TEXT; ++s) line[n++] = *s;
        for (const char *s = value; *s && n < TEXT; ++s) line[n++] = *s;
        line[n] = 0;
        if (!fastboot_reply("INFO", line)) return;
    }
    (void)fastboot_reply("OKAY", "");
}

/* The host has the OKAY before the restart. */
static void restart(bool bootloader) {
    const char *why = device->can_restart(bootloader);
    if (why) (void)fastboot_reply("FAIL", why);
    else if (fastboot_reply("OKAY", "")) device->restart(bootloader);
}

void fastboot_command(const char *command, unsigned length) {
    if (strlen(command) != length) (void)fastboot_reply("FAIL", "not a command");
    else if (fastboot_starts(command, "getvar:")) getvar(command + 7);
    else if (!strcmp(command, "reboot")) restart(false);
    else if (!strcmp(command, "reboot-bootloader")) restart(true);
    else if (!device->command || !device->command(command))
        (void)fastboot_reply("FAIL", "unknown command");
}

/* The configuration at a speed: the fastboot interface (vendor class, 0x42,
 * 3) with bulk IN 1 and bulk OUT 1, bus-powered at 500 mA. */
static unsigned configuration(uint8_t *out, bool high, uint8_t type) {
    static const uint8_t head[18] = {9, 2, 32, 0, 1, 1,    0,    0x80, 250,
                                     9, 4, 0,  0, 2, 0xff, 0x42, 0x03, 0};
    unsigned packet = high ? 512 : 64;
    memcpy(out, head, sizeof(head));
    out[1] = type;
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t *e = out + sizeof(head) + 7 * i;
        e[0] = 7;
        e[1] = 5;
        e[2] = i ? 0x01 : 0x81;
        e[3] = 2;
        e[4] = (uint8_t)packet;
        e[5] = (uint8_t)(packet >> 8);
        e[6] = 0;
    }
    return 32;
}

unsigned fastboot_descriptor(uint16_t value, bool high_speed, uint8_t *out, unsigned room) {
    static const uint8_t device_descriptor[18] = {18,   1,    0x00, 0x02, 0,    0, 0, 64, 0xd1,
                                                  0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3,  1};
    static const uint8_t qualifier[10] = {10, 6, 0x00, 0x02, 0, 0, 0, 64, 1, 0};
    uint8_t d[64];
    unsigned type = value >> 8, index = value & 255, n = 0;
    if (type == 1) memcpy(d, device_descriptor, n = sizeof(device_descriptor));
    else if (type == 2) n = configuration(d, high_speed, 2);
    else if (type == 7) n = configuration(d, !high_speed, 7);
    else if (type == 6) memcpy(d, qualifier, n = sizeof(qualifier));
    else if (type == 3 && !index) {
        static const uint8_t languages[4] = {4, 3, 0x09, 0x04}; /* US English */
        memcpy(d, languages, n = sizeof(languages));
    } else if (type == 3 && index <= 3) {
        const char *s = index == 1 ? "SOS2" : index == 2 ? device->name : device->product;
        for (n = 2; *s && n + 2 <= sizeof(d); ++s, n += 2) {
            d[n] = (uint8_t)*s;
            d[n + 1] = 0;
        }
        d[0] = (uint8_t)n;
        d[1] = 3;
    }
    if (n > room) n = 0;
    memcpy(out, d, n);
    return n;
}
