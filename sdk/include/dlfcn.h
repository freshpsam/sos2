#ifndef SOS2_DLFCN_H
#define SOS2_DLFCN_H
/* SOS2 programs are static: there is no dynamic loader. dlopen always fails
 * and dlsym finds nothing, so code that probes for optional libraries falls
 * back to what it was linked with. */
#ifdef __cplusplus
extern "C" {
#endif
#define RTLD_LAZY     1
#define RTLD_NOW      2
#define RTLD_NOLOAD   4
#define RTLD_LOCAL    0
#define RTLD_GLOBAL   0x100
#define RTLD_NODELETE 0x1000
#define RTLD_DEFAULT  ((void *)0)

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;

void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);
int dladdr(const void *address, Dl_info *info);
#ifdef __cplusplus
}
#endif
#endif
