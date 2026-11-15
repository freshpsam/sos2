#ifndef SOS2_USER_INPUT_H
#define SOS2_USER_INPUT_H
#include <stdint.h>

/* Fixed-width native ABI. Key codes are the virtio-input physical key numbers;
 * layout/text interpretation belongs to the renderer, not the kernel. */
#define INPUT_POINTER   1u
#define INPUT_KEY       2u
#define INPUT_WHEEL     3u
#define INPUT_FOCUS     4u
#define INPUT_RESET     5u
#define INPUT_VIEWPORT  6u
#define INPUT_DOWN      1u
#define INPUT_REPEAT    2u
#define INPUT_CANCEL    4u
#define INPUT_FOCUSED   8u
#define INPUT_SHIFT     1u
#define INPUT_CTRL      2u
#define INPUT_ALT       4u
#define INPUT_META      8u
#define INPUT_CAPS_LOCK 16u

struct user_input_event {
    uint32_t type, flags, modifiers, code;
    int32_t x, y, delta_x, delta_y;
};
#endif
