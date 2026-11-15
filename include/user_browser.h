#ifndef SOS2_USER_BROWSER_H
#define SOS2_USER_BROWSER_H
#include <stdint.h>
#define BROWSER_URL_MAX     2047u
#define BROWSER_TITLE_MAX   127u
#define BROWSER_NAVIGATE    1u
#define BROWSER_BACK        2u
#define BROWSER_CAN_GO_BACK 4u

/* Only the trusted shell issues commands; a renderer can read its own slot.
 * Metadata is presentation data, never permission or certificate authority. */
struct user_browser_command {
    uint32_t action;
    char url[BROWSER_URL_MAX + 1];
};

struct user_browser_state {
    uint32_t flags;
    char url[BROWSER_URL_MAX + 1], title[BROWSER_TITLE_MAX + 1];
};
#endif
