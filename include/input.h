#ifndef SOS2_INPUT_H
#define SOS2_INPUT_H
#include "graphics.h"
#include "user_input.h"
bool input_init(const uintptr_t *bases, size_t count, uint32_t width, uint32_t height);
/* Runtime failure produces one cancelled event, then no events until reinit. */
bool input_poll_extended(struct pointer_event *event, int32_t *wheel_x, int32_t *wheel_y);
/* Optional, independent virtio keyboard. A failure emits one INPUT_RESET. */
bool keyboard_init(const uintptr_t *bases, size_t count);
bool keyboard_poll(struct user_input_event *event);
/* QEMU virt's power button on a PL061 pin. True once per debounced press. */
bool power_key_init(uintptr_t gpio, unsigned pin);
bool power_key_pressed(void);
#endif
