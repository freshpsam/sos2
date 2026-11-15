#ifndef SOS2_PIXEL_FRAME_H
#define SOS2_PIXEL_FRAME_H
#include <stdbool.h>
#include "user_abi.h"

static inline bool pixel_rect_valid(struct user_pixel_rect r) {
    return r.width && r.height && r.x < WEB_WIDTH && r.y < WEB_HEIGHT &&
           r.width <= WEB_WIDTH - r.x && r.height <= WEB_HEIGHT - r.y;
}

static inline bool pixel_scroll_valid(const struct user_pixel_scroll *s) {
    return pixel_rect_valid(s->rect) && (s->dx || s->dy) &&
           (int64_t)s->dx > -(int64_t)s->rect.width &&
           (int64_t)s->dx<s->rect.width &&(int64_t)s->dy> - (int64_t)s->rect.height &&
           (int64_t)s->dy < s->rect.height;
}

/* Call after validation. Both boxes lie in the clip; the destination is the
 * retained part, leaving exposed pixels for the frame's following patches. */
static inline void pixel_scroll_boxes(const struct user_pixel_scroll *s,
                                      struct user_pixel_rect *src, struct user_pixel_rect *dst) {
    unsigned ax = s->dx < 0 ? (unsigned)-s->dx : (unsigned)s->dx;
    unsigned ay = s->dy < 0 ? (unsigned)-s->dy : (unsigned)s->dy;
    *src =
        (struct user_pixel_rect){s->rect.x + (s->dx < 0 ? ax : 0), s->rect.y + (s->dy < 0 ? ay : 0),
                                 s->rect.width - ax, s->rect.height - ay};
    *dst = (struct user_pixel_rect){s->rect.x + (s->dx > 0 ? ax : 0),
                                    s->rect.y + (s->dy > 0 ? ay : 0), src->width, src->height};
}
#endif
