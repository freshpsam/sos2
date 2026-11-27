#ifndef SOS2_PC_H
#define SOS2_PC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fdt.h"
#include "graphics.h"
#include "user_input.h"

/* A PC's drivers (boards/pc): PCI, USB on xHCI with its keyboards, mice and
 * the stick SOS2 started from, and the screen's pointer. Everything is
 * polled on CPU 0, as the kernel's other drivers are. Drivers find their
 * functions and ask for their windows before the kernel maps its devices
 * (board_handoff), then start once it has (process_init's drivers, or
 * board_processes_ready). */

/* PCI (pci.c): configuration space through ECAM; memory BARs as the
 * firmware assigned them. */
#define PCI_FUNCTIONS 256

struct pci_function {
    uint8_t bus, device, function;
    uint16_t vendor, id;
    uint32_t class; /* base class, sub-class and interface */
    uint64_t bar[6], bar_size[6];
};

void pci_scan(struct platform_devices *d);
/* The index-th function whose class matches under mask, or NULL. */
const struct pci_function *pci_find(uint32_t class, uint32_t mask, unsigned index);
/* A memory BAR in the kernel's device mappings. */
bool pci_map(struct platform_devices *d, const struct pci_function *f, unsigned bar);
/* Memory decoding and bus mastering on, after the kernel's mappings. */
void pci_enable(const struct pci_function *f);
/* A function's configuration space, a dword at a time. */
uint32_t pci_read32(const struct pci_function *f, unsigned offset);
void pci_write32(const struct pci_function *f, unsigned offset, uint32_t value);

/* USB on the xHCI controllers (xhci.c). */
void xhci_probe(struct platform_devices *d);
/* The controllers reset and their devices enumerated, once, after
 * vm_init; true while any runs. */
bool xhci_start(void);
/* Events: transfers completed (reports to their drivers) and ports
 * changed; in every wait. */
void xhci_poll(void);
/* xhci_poll, then devices gone released and new ones enumerated: on CPU
 * 0's loop (board_poll). */
void xhci_service(void);

/* A configured endpoint: a device ("slot": numbered across the
 * controllers) and its endpoint (DCI). */
struct usb_endpoint {
    uint8_t slot, dci;
    uint16_t max_packet;
};

/* A control transfer on endpoint 0, waited for (up to timeout_us). */
bool usb_control(unsigned slot, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                 void *data, uint16_t length, uint32_t timeout_us);
/* A bulk transfer, waited for, yielding on the service worker; the bytes
 * moved in *done. */
bool usb_bulk(const struct usb_endpoint *e, void *data, uint32_t length, uint32_t *done,
              uint32_t timeout_us);
/* An interrupt IN transfer started; its completion reaches the class
 * driver's report hook. */
bool usb_interrupt_in(const struct usb_endpoint *e, void *data, uint32_t length);
/* An endpoint's halt cleared on the device (CLEAR_FEATURE ENDPOINT_HALT)
 * and on the controller: the data toggle starts afresh on both, and the
 * ring again past what was queued. */
bool usb_clear_halt(const struct usb_endpoint *e);

/* Class drivers, which xhci.c calls for each interface it configures. */
enum { USB_CLASS_HID = 3, USB_CLASS_STORAGE = 8 };

bool usb_hid_attach(unsigned slot, unsigned interface, uint8_t protocol,
                    const struct usb_endpoint *in, uint16_t report_length);
void usb_hid_report(unsigned slot, uint8_t dci, const uint8_t *data, uint32_t length);
bool usb_storage_attach(unsigned slot, unsigned interface, const struct usb_endpoint *in,
                        const struct usb_endpoint *out);
void usb_storage_detach(unsigned slot);
/* A device gone: every class driver lets go of it. */
void usb_detach(unsigned slot);

/* The network card (e1000e.c): an Intel 82574L. */
struct net_device;
void e1000e_probe(struct platform_devices *d);
bool e1000e_init(const struct platform_devices *d, struct net_device *device);

/* Sound (hda.c): Intel High Definition Audio, the first controller whose
 * first codec has an analog output. */
struct audio_device;
void hda_probe(struct platform_devices *d);
bool hda_init(struct audio_device *device);

/* The power button (acpi_power.c): ACPI's fixed one. */
void acpi_power_start(const struct platform_devices *d);
bool acpi_power_pressed(void);

/* Input (usb_hid.c): key events with repeat, as kernel/input.c's, and the
 * pointer in monitor pixels. */
bool usb_keyboard_poll(struct user_input_event *event);

struct pc_pointer {
    int32_t x, y, wheel;
    bool down, moved;
};

bool usb_pointer_poll(struct pc_pointer *pointer);
/* The monitor, and the desktop's part of it, which holds the pointer. */
void usb_input_screen(uint32_t monitor_width, uint32_t monitor_height, uint32_t left, uint32_t top,
                      uint32_t width, uint32_t height);

/* The cursor (kernel_board.c), kept after the desktop draws a region of
 * source over it (kernel/display.c). */
void pc_cursor_presented(const struct framebuffer *visible, const struct framebuffer *source,
                         unsigned x, unsigned y, unsigned width, unsigned height);
#endif
