#include "internal.h"
#include <stdlib.h>
extern void __libc_init_array(void);
extern void __libc_fini_array(void);
extern int main(int, char **);
static char *environment[] = {"HOME=/data", "TMPDIR=/tmp", "LANG=C", NULL};
char **environ = environment;
void *__dso_handle = &__dso_handle;

void _init(void) { }

void _fini(void) { }

#ifndef __arm__
/* C++ unwinding without ARM32's EHABI finds .eh_frame through libgcc's
 * registry, which crtbegin.o would fill; programs link without it (the 64-bit
 * linker scripts, such as sdk/runtime/linker-arm64.ld, end the section with a
 * zero terminator). The object is libgcc's struct object. */
extern char __eh_frame_start[];
extern void __register_frame_info(const void *, void *) __attribute__((weak));
static void *eh_object[16];

/* The bare-metal libgcc unwinder has no thread locks. Its first frame lookup
 * removes the registered image while sorting its FDEs, and an allocation
 * failure makes later lookups retry. Serialize every lookup, including those
 * retries, so another thread never sees a missing or half-sorted image.
 * Keep the real reference strong: --wrap redirects libgcc's callers here,
 * so a weak reference would leave its archive member unlinked. */
extern const void *__real__Unwind_Find_FDE(const void *, void *);
static struct sos2_lock unwind_lock;

const void *__wrap__Unwind_Find_FDE(const void *pc, void *bases) {
    sos2_lock(&unwind_lock);
    const void *frame = __real__Unwind_Find_FDE(pc, bases);
    sos2_unlock(&unwind_lock);
    return frame;
}
#endif

static const char *engine_options;

const char *sos2_engine_options(void) { return engine_options; }

/* x2 (r2): a web page's sos2.jsc= boot argument (include/user_abi.h). */
void sos2_start(unsigned mode, unsigned pid, const char *options) {
#ifdef __x86_64__
    sos2_threads_bootstrap();
#endif
    engine_options = options;
#ifndef __arm__
    if (__register_frame_info) __register_frame_info(__eh_frame_start, eh_object);
#endif
    (void)mode;
    (void)pid;
    sos2_threads_init();
    atexit(__libc_fini_array);
    __libc_init_array();
    char *argv[] = {"sos2-native", NULL};
    exit(main(1, argv));
}
