#ifndef SOS2_PAGE_HOST_H
#define SOS2_PAGE_HOST_H
#include "graphics.h"
#include "vm.h"
#include "user_input.h"
#include "user_browser.h"
void page_host_init(struct framebuffer *screen);
bool page_host_input(const struct pointer_event *event);
void page_host_keyboard(const struct user_input_event *event);
/* The phone's Back key, which never returns to the desktop: a page or app
 * that asked for it gets key 158; otherwise a page goes back in its history,
 * if it has any, and an app is left as it is. On the Wi-Fi screen a
 * network's page returns to the list (desktop_wifi.h); nothing else happens
 * on the desktop. */
void page_host_back(void);
/* The phone's Home key: the desktop, from a page, an app or the Wi-Fi
 * screen; pages keep running. */
void page_host_home(void);
void page_host_wheel(const struct pointer_event *point, int32_t dx, int32_t dy);
void page_host_render(void);
int32_t page_host_present(unsigned slot, const struct vm_space *space, user_ptr address,
                          uint32_t bytes);
int32_t page_host_present_damage(unsigned slot, const struct vm_space *,
                                 const struct user_present_damage *);
int32_t page_host_present_frame(unsigned slot, const struct vm_space *,
                                const struct user_present_frame *);
/* USER_GPU_PRESENT: check the damage, then, once the GPU copied it, record the
 * frame (from the service worker). */
int32_t page_host_gpu_frame_check(unsigned slot, const struct user_pixel_rect *, unsigned count);
void page_host_gpu_frame(unsigned slot, const struct user_pixel_rect *, unsigned count);
unsigned page_host_surface_state(unsigned slot);
void page_host_screen_active(bool active);
bool page_host_next(unsigned slot, struct user_input_event *event);
int32_t page_host_input_method(unsigned slot, unsigned enabled);
/* SYS_BACK_KEY: 1 sends the slot the Back key as key 158; 0 (the default)
 * leaves Back to the desktop. */
int32_t page_host_back_key(unsigned slot, unsigned enabled);
/* SYS_HAPTIC_TAP: a buzz for each press the slot was sent (four at most wait),
 * until 250 ms after the latest; USER_EAGAIN without one, or with the screen
 * off. */
int32_t page_host_haptic_tap(unsigned slot);
void page_host_closed(unsigned slot);
/* A slot's process starts: a web page, or the app it runs (include/apps.h). */
struct app;
bool page_host_opened(unsigned slot, const struct app *app);
void page_host_start_failed(unsigned slot);
void page_host_redraw(void);
/* Queue a bounded widget repaint in logical desktop coordinates. The painter
 * must overwrite its rectangle and stay inside it; repeated requests coalesce. */
void page_host_redraw_region(unsigned x, unsigned y, unsigned width, unsigned height,
                             void (*paint)(struct framebuffer *, unsigned, unsigned));
void page_host_network_status(const char *);
void page_host_device_status(bool random_ready, bool storage_ready, bool session_only);
/* The wall time was set or lost: the header's clock follows. */
void page_host_clock_status(void);
void page_host_entropy_status(bool ready, const char *error);
/* A touch screen or USB link that failed to start (the tablet's) says why
 * in the status lines; NULL when it works. */
void page_host_touch_status(const char *error);
void page_host_usb_status(const char *error);
bool page_host_browser_command(unsigned slot, struct user_browser_command *);
int32_t page_host_browser_state(unsigned slot, const struct user_browser_state *);
#endif
