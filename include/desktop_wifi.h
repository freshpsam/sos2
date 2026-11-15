#ifndef SOS2_DESKTOP_WIFI_H
#define SOS2_DESKTOP_WIFI_H
#include "graphics.h"
#include "user_input.h"
void desktop_wifi_open(void);
void desktop_wifi_close(void);
bool desktop_wifi_visible(void);
bool desktop_wifi_editing(void);
void desktop_wifi_hide_keyboard(void);
void desktop_wifi_pointer(const struct pointer_event *, bool press, bool release);
void desktop_wifi_key(const struct user_input_event *, char text);
/* The Back key: a network's page returns to the list; the list stays (Home
 * returns to the desktop). */
void desktop_wifi_back(void);
void desktop_wifi_render(struct framebuffer *);
void desktop_wifi_network_status(const char *);
#endif
