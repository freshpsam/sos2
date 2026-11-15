#include "internal.h"
#include <stdlib.h>
#include <stdio.h>
extern void __libc_init_array(void);
extern void __libc_fini_array(void);
extern int main(int, char **);
static char *environment[] = {"HOME=/data", "TMPDIR=/tmp", "LANG=C", NULL};
char **environ = environment;
void *__dso_handle = &__dso_handle;

void _init(void) { }

void _fini(void) { }

void sos2_start(unsigned mode, unsigned pid) {
    (void)mode;
    (void)pid;
    sos2_threads_init();
    atexit(__libc_fini_array);
    __libc_init_array();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    char *argv[] = {"sos2-native", NULL};
    exit(main(1, argv));
}
