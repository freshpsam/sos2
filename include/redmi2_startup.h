#ifndef SOS2_REDMI2_STARTUP_H
#define SOS2_REDMI2_STARTUP_H
#include "graphics.h"
/* Startup controls stay separate from the desktop while resources and Wi-Fi
 * initialize. The progress callback only paints and polls power/touch. */
void redmi2_startup_begin(struct framebuffer *);
void redmi2_startup_end(void);
void redmi2_startup_wait_wifi(void);
#endif
