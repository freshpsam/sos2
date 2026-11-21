#ifndef SOS2_FASTBOOT_H
#define SOS2_FASTBOOT_H
#include <stdbool.h>
#include <stdint.h>

/* fastboot on a device's USB link (kernel/usb/fastboot.c), for the computer
 * it is plugged into, with Android's fastboot tool and no driver or root:
 * the commands every device answers and the USB descriptors of its one
 * interface, around the board's controller driver, its own commands and
 * variables, and its restarts (boards/gta7lite, boards/nexus5). */
struct fastboot_device {
    /* One transfer to the host on the bulk IN endpoint: false if it did not
     * go. */
    bool (*send)(const void *data, uint32_t length);
    /* The product, also the serial number; the USB product string. */
    const char *product, *name;
    /* The board's variables, NULL for one it does not have, and those that
     * getvar:all lists after version, product and serialno (NULL-ended). */
    const char *(*variable)(const char *name);
    const char *const *variables;
    /* A command of the board's: false when it is not one, else it answered. */
    bool (*command)(const char *command);
    /* reboot and reboot-bootloader (into the loader's own fastboot or flash
     * mode): first why not now, NULL when it can; then, once the host has
     * the OKAY, the restart. */
    const char *(*can_restart)(bool bootloader);
    void (*restart)(bool bootloader);
};

/* The board's link, before the host sees it. */
void fastboot_init(const struct fastboot_device *);
/* Each command as it arrives: length bytes, NUL-terminated. */
void fastboot_command(const char *command, unsigned length);
/* A reply: kind ("INFO", "OKAY", "FAIL" or "DATA") and its text, cut to
 * fit one packet under 64 bytes. False if it did not go. */
bool fastboot_reply(const char *kind, const char *text);
bool fastboot_starts(const char *text, const char *prefix);
/* Eight lowercase hex digits and a NUL at p: the NUL's address. */
char *fastboot_hex32(char *p, uint32_t value);
/* A panic on CPU 0: the main loop and the service worker are gone (kernel/
 * platform.c), which the board's storage commands must know. */
void fastboot_panic(void);
bool fastboot_stopped(void);
/* GET_DESCRIPTOR's answer for value (type in the high byte, index in the
 * low), for a link at high or full speed, in at most room bytes of out: its
 * size, or 0 for none (a stall). */
unsigned fastboot_descriptor(uint16_t value, bool high_speed, uint8_t *out, unsigned room);
#endif
