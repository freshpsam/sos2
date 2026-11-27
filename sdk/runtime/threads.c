#include "internal.h"
#include <errno.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/lock.h>

static struct sos2_thread threads[USER_THREAD_LIMIT];
static struct sos2_lock registry_lock;

static struct {
    unsigned used, generation;
    void (*destroy)(void *);
} keys[SOS2_KEYS];

extern char __tls_start[], __tls_data_end[], __tls_end[], __tls_align[];

/* Variant 1 TLS, as both ARM ABIs specify: the thread pointer addresses a
 * TCB_SIZE-byte TCB, whose first word points to the thread's struct
 * sos2_thread, and the TLS block follows it at the block's alignment.
 * x64's is variant 2: the TLS block ends at the thread pointer (FS's base),
 * aligned, and the TCB's first word there points to itself, its second to
 * the thread's struct sos2_thread. */
#ifdef __x86_64__
#define TCB_SIZE 16

static inline void *thread_pointer(void) {
    void *pointer;
    __asm__("movq %%fs:0, %0" : "=r"(pointer));
    return pointer;
}

/* __tls_align is an absolute symbol, out of RIP-relative reach of the image
 * window: an absolute immediate. */
static inline unsigned tls_align(void) {
    uintptr_t value;
    __asm__("movabsq $__tls_align, %0" : "=r"(value));
    return (unsigned)value;
}

/* The TLS block's size, rounded to the segment's alignment as ld rounds it:
 * the thread pointer's offset from the block's start. */
static inline uintptr_t tls_block(void) {
    uintptr_t alignment = tls_align();
    return ((uintptr_t)(__tls_end - __tls_start) + alignment - 1) & ~(alignment - 1);
}

/* The thread pointer's alignment: the segment's, and the TCB's 16. */
static inline uintptr_t tls_pointer_align(void) {
    uintptr_t alignment = tls_align();
    return alignment > 16 ? alignment : 16;
}

/* The thread pointer in a thread's TLS pages: the first aligned address
 * with room for the block below it. */
static inline void *tls_pointer(const struct sos2_thread *t) {
    uintptr_t alignment = tls_pointer_align();
    return (void *)(((uintptr_t)t->tls + tls_block() + alignment - 1) & ~(alignment - 1));
}
#elif defined(__aarch64__)
#define TCB_SIZE 16

static inline void *thread_pointer(void) {
    void *pointer;
    __asm__("mrs %0, tpidr_el0" : "=r"(pointer));
    return pointer;
}

/* __tls_align is an absolute symbol, out of PC-relative reach of the image
 * window: load it from a literal. */
static inline unsigned tls_align(void) {
    uintptr_t value;
    __asm__("ldr %0, =__tls_align" : "=r"(value));
    return (unsigned)value;
}
#else
#define TCB_SIZE 8
static inline void *thread_pointer(void) {
    void *pointer;
    __asm__("mrc p15, 0, %0, c13, c0, 3" : "=r"(pointer)); /* TPIDRURO */
    return pointer;
}

static inline unsigned tls_align(void) { return (uintptr_t)__tls_align; }
#endif

/* ARM ELF general-dynamic TLS uses a module/offset pair even in static PIC
 * libraries. There is exactly one TLS module in a native SOS2 executable. */
#ifdef __x86_64__
void *__tls_get_addr(const sos2_word index[2]) {
    char *pointer = thread_pointer();
    if (index[0] != 1 || index[1] > (uintptr_t)(__tls_end - __tls_start)) abort();
    return pointer - tls_block() + index[1];
}

static inline struct sos2_thread *self_thread(void) {
    struct sos2_thread *thread;
    __asm__("movq %%fs:8, %0" : "=r"(thread));
    return thread;
}

void sos2_threads_bootstrap(void) {
    static void *tcb[2] __attribute__((aligned(16))) = {tcb, NULL};
    if (call(SYS_THREAD_TLS, sos2_ptr(tcb), 0)) finish(121);
}
#else
void *__tls_get_addr(const sos2_word index[2]) {
    unsigned alignment = tls_align();
    unsigned header = (TCB_SIZE + alignment - 1) & ~(alignment - 1);
    void *pointer = thread_pointer();
    if (!pointer || index[0] != 1 || index[1] > (uintptr_t)(__tls_end - __tls_start)) abort();
    return (char *)pointer + header + index[1];
}

static inline struct sos2_thread *self_thread(void) {
    void *pointer = thread_pointer();
    return pointer ? *(struct sos2_thread **)pointer : NULL;
}
#endif

/* Inline below: every lock and key lookup asks, 1.5% of YouTube's main
 * thread on the Nexus 5 as calls. */
static inline unsigned self_id(void) {
    struct sos2_thread *t = self_thread();
    unsigned id = t ? __atomic_load_n(&t->tid, __ATOMIC_RELAXED) : 0;
    return id ? id : (unsigned)pthread_self();
}

struct sos2_thread *sos2_self(void) { return self_thread(); }
static struct _reent bootstrap = _REENT_INIT(bootstrap);

struct _reent *__getreent(void) {
    struct sos2_thread *t = sos2_self();
    return t ? &t->reent : &bootstrap;
}

int sos2_trylock(struct sos2_lock *lock) {
    unsigned self = self_id();
    if (sos2_load(&lock->owner) == self) {
        ++lock->depth;
        return 1;
    }
    if (!sos2_cas(&lock->owner, 0, self)) return 0;
    lock->depth = 1;
    return 1;
}

void sos2_lock(struct sos2_lock *lock) {
    if (sos2_trylock(lock)) return;
    __atomic_add_fetch(&lock->waiters, 1, __ATOMIC_ACQ_REL);
    while (!sos2_trylock(lock)) {
        unsigned owner = sos2_load(&lock->owner);
        if (owner) sos2_wait(&lock->owner, owner, USER_WAIT_FOREVER);
    }
    __atomic_sub_fetch(&lock->waiters, 1, __ATOMIC_ACQ_REL);
}

void sos2_unlock(struct sos2_lock *lock) {
    if (sos2_load(&lock->owner) != self_id() || !lock->depth) abort();
    if (!--lock->depth) {
        sos2_store(&lock->owner, 0);
        if (sos2_load(&lock->waiters)) sos2_wake(&lock->owner, 1);
    }
}

/* Newlib's static locks and a bounded dynamic pool avoid malloc-lock recursion. */
struct __lock {
    struct sos2_lock lock;
    uint32_t used;
};
struct __lock __lock___sfp_recursive_mutex, __lock___atexit_recursive_mutex,
    __lock___at_quick_exit_mutex, __lock___malloc_recursive_mutex, __lock___env_recursive_mutex,
    __lock___tz_mutex, __lock___dd_hash_mutex, __lock___arc4random_mutex;
static struct __lock libc_locks[128];

void __retarget_lock_init(_LOCK_T *out) {
    for (unsigned i = 0; i < 128; ++i)
        if (sos2_cas(&libc_locks[i].used, 0, 1)) {
            *out = &libc_locks[i];
            return;
        }
    /* Newlib provides no error return for lock allocation; never run unlocked. */
    abort();
}

void __retarget_lock_init_recursive(_LOCK_T *out) { __retarget_lock_init(out); }

void __retarget_lock_close(_LOCK_T lock) {
    if (lock->lock.owner) abort();
    sos2_store(&lock->used, 0);
}

void __retarget_lock_close_recursive(_LOCK_T lock) { __retarget_lock_close(lock); }

void __retarget_lock_acquire(_LOCK_T lock) { sos2_lock(&lock->lock); }

void __retarget_lock_acquire_recursive(_LOCK_T lock) { sos2_lock(&lock->lock); }

int __retarget_lock_try_acquire(_LOCK_T lock) { return sos2_trylock(&lock->lock); }

int __retarget_lock_try_acquire_recursive(_LOCK_T lock) { return sos2_trylock(&lock->lock); }

void __retarget_lock_release(_LOCK_T lock) { sos2_unlock(&lock->lock); }

void __retarget_lock_release_recursive(_LOCK_T lock) { sos2_unlock(&lock->lock); }

#ifdef __aarch64__
/* The Arm GNU Toolchain's GCC for aarch64-none-elf is built without native
 * TLS (--disable-tls) and for a single thread, so every __thread and
 * thread_local, in the SDK's libraries too, calls __emutls_get_address, and
 * libgcc's version shares one copy among all threads. This one gives each
 * thread its own, allocated on first use; libsos2 links ahead of libgcc.
 * Never from malloc: the engine's (mimalloc) has thread-local variables
 * itself. They are carved from the rest of the thread's TLS pages, then from
 * pages mapped for the thread, and unmapped when its record is released. */
struct __emutls_object {
    uintptr_t size, align;

    union {
        uintptr_t index; /* 1-based, assigned on first use by any thread */
        void *pointer;
    } location;

    void *initializer; /* NULL: zero-initialized */
};

struct emutls_chunk {
    struct emutls_chunk *next;
    unsigned pages;
};

static struct sos2_lock emutls_lock;
static uintptr_t emutls_indices;
/* The main thread's variables until sos2_threads_init gives it a record. */
static struct sos2_thread emutls_early;

/* Zeroed: carved space is never reused while the thread lives. */
static void *emutls_alloc(struct sos2_thread *t, uintptr_t size, uintptr_t align) {
    uintptr_t at = (t->emutls_free + align - 1) & ~(align - 1);
    if (!t->emutls_free || at > t->emutls_end || size > t->emutls_end - at) {
        unsigned pages = (sizeof(struct emutls_chunk) + align + size + 4095) / 4096;
        sos2_status mapped = sos2_map(pages, 3);
        if (mapped < 0) abort();
        struct emutls_chunk *chunk = (void *)(uintptr_t)mapped;
        chunk->next = t->emutls_chunks;
        chunk->pages = pages;
        t->emutls_chunks = chunk;
        t->emutls_end = (uintptr_t)chunk + pages * 4096ul;
        at = ((uintptr_t)(chunk + 1) + align - 1) & ~(align - 1);
    }
    t->emutls_free = at + size;
    return (void *)at;
}

void __emutls_register_common(void *registered, unsigned long size, unsigned long align,
                              void *initializer) {
    struct __emutls_object *object = registered;
    if (object->size < size) {
        object->size = size;
        object->initializer = NULL;
    }
    if (object->align < align) object->align = align;
    if (initializer && size == object->size) object->initializer = initializer;
}

/* A variable's first use on this thread, and an index's by any thread. */
static __attribute__((noinline)) void *emutls_first_use(struct __emutls_object *object) {
    uintptr_t index = __atomic_load_n(&object->location.index, __ATOMIC_ACQUIRE);
    if (!index) {
        sos2_lock(&emutls_lock);
        index = object->location.index;
        if (!index)
            __atomic_store_n(&object->location.index, index = ++emutls_indices, __ATOMIC_RELEASE);
        sos2_unlock(&emutls_lock);
    }
    struct sos2_thread *t = sos2_self();
    if (!t) t = &emutls_early;
    if (index > t->emutls_count) {
        uintptr_t count = index < 2 * t->emutls_count ? 2 * t->emutls_count : index + 31;
        void **grown = emutls_alloc(t, count * sizeof(*grown), sizeof(*grown));
        if (t->emutls_count) memcpy(grown, t->emutls, t->emutls_count * sizeof(*grown));
        t->emutls = grown;
        t->emutls_count = count;
    }
    void *variable = t->emutls[index - 1];
    if (!variable) {
        uintptr_t align = object->align > sizeof(void *) ? object->align : sizeof(void *);
        variable = emutls_alloc(t, object->size, align);
        if (object->initializer) memcpy(variable, object->initializer, object->size);
        t->emutls[index - 1] = variable;
    }
    return variable;
}

/* Every access to a thread-local variable, such as mimalloc's heap on each
 * allocation and Mesa's dispatch table on each GL call: frameless, with any
 * first use on the slow path. */
void *__emutls_get_address(void *requested) {
    struct __emutls_object *object = requested;
    uintptr_t index = __atomic_load_n(&object->location.index, __ATOMIC_ACQUIRE);
    struct sos2_thread *t = sos2_self();
    if (index && t && index <= t->emutls_count) {
        void *variable = t->emutls[index - 1];
        if (variable) return variable;
    }
    return emutls_first_use(object);
}

static void emutls_release(struct sos2_thread *t) {
    for (struct emutls_chunk *chunk = t->emutls_chunks, *next; chunk; chunk = next) {
        next = chunk->next;
        call3(SYS_VM_UNMAP, sos2_ptr(chunk), chunk->pages, 0);
    }
}
#endif

static int init_tls(struct sos2_thread *thread) {
    unsigned alignment = tls_align();
    if (alignment > 4096) return ENOTSUP;
#ifdef __x86_64__
    uintptr_t block = tls_block();
    thread->tls_pages = (block + tls_pointer_align() + TCB_SIZE + 4095) / 4096;
    sos2_status mapped = sos2_map(thread->tls_pages, 3);
    if (mapped < 0) return sos2_errno(mapped);
    thread->tls = (void *)(uintptr_t)mapped;
    void **tcb = tls_pointer(thread);
    tcb[0] = tcb;
    tcb[1] = thread;
    memcpy((char *)tcb - block, __tls_start, __tls_data_end - __tls_start);
    _REENT_INIT_PTR(&thread->reent);
    return 0;
#endif
    unsigned header = (TCB_SIZE + alignment - 1) & ~(alignment - 1), size = __tls_end - __tls_start;
    thread->tls_pages = (header + size + 4095) / 4096;
    sos2_status result = sos2_map(thread->tls_pages, 3);
    if (result < 0) return sos2_errno(result);
    thread->tls = (void *)(uintptr_t)result;
    *(struct sos2_thread **)thread->tls = thread;
    memcpy((char *)thread->tls + header, __tls_start, __tls_data_end - __tls_start);
#ifdef __aarch64__
    /* Emulated TLS variables start in the rest of these pages. */
    thread->emutls_free = (uintptr_t)thread->tls + header + size;
    thread->emutls_end = (uintptr_t)thread->tls + thread->tls_pages * 4096ul;
#endif
    _REENT_INIT_PTR(&thread->reent);
    return 0;
}

void sos2_threads_init(void) {
    struct sos2_thread *main = &threads[0];
    main->used = 1;
    main->tid = call(SYS_THREAD_ID, 0, 0);
    if (init_tls(main)) finish(120);
#ifdef __aarch64__
    /* Keep the variables the main thread used before it had this record. */
    main->emutls = emutls_early.emutls;
    main->emutls_count = emutls_early.emutls_count;
    main->emutls_chunks = emutls_early.emutls_chunks;
#endif
#ifdef __x86_64__
    if (call(SYS_THREAD_TLS, sos2_ptr(tls_pointer(main)), 0)) finish(121);
#else
    if (call(SYS_THREAD_TLS, sos2_ptr(main->tls), 0)) finish(121);
#endif
}

/* A thread's own ID never changes while it runs, so a plain load: every lock
 * and unlock asks. */
pthread_t pthread_self(void) {
    struct sos2_thread *t = sos2_self();
    unsigned id = t ? __atomic_load_n(&t->tid, __ATOMIC_RELAXED) : 0;
    return id ? id : (unsigned)call(SYS_THREAD_ID, 0, 0);
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

static struct sos2_thread *lookup(pthread_t id) {
    for (unsigned i = 0; i < USER_THREAD_LIMIT; ++i)
        if (threads[i].used && threads[i].tid == id) return &threads[i];
    return NULL;
}

static void release(struct sos2_thread *t) {
#ifdef __aarch64__
    emutls_release(t);
#endif
    if (t->mapping) call3(SYS_VM_UNMAP, sos2_ptr(t->mapping), t->pages, 0);
    if (t->tls) call3(SYS_VM_UNMAP, sos2_ptr(t->tls), t->tls_pages, 0);
    memset(t, 0, sizeof(*t));
}

static void reap_detached(void) {
    for (unsigned i = 1; i < USER_THREAD_LIMIT; ++i) {
        struct sos2_thread *t = &threads[i];
        if (t->used && t->detached && !sos2_load(&t->completion) && t->tid &&
            t->tid != pthread_self())
            if (!call(SYS_THREAD_JOIN, t->tid, 0)) release(t);
    }
}

static void trampoline(struct sos2_thread *t) { pthread_exit(t->entry(t->argument)); }

int pthread_create(pthread_t *id, const pthread_attr_t *attr, void *(*entry)(void *),
                   void *argument) {
    if (!id || !entry) return EINVAL;
    if (attr &&
        (!attr->is_initialized || attr->stacksize < 4096 || attr->stacksize > 4 * 1024 * 1024))
        return EINVAL;
    sos2_lock(&registry_lock);
    reap_detached();
    struct sos2_thread *t = NULL;
    for (unsigned i = 1; i < USER_THREAD_LIMIT; ++i)
        if (!threads[i].used) {
            t = &threads[i];
            break;
        }
    if (!t) {
        sos2_unlock(&registry_lock);
        return EAGAIN;
    }
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->entry = entry;
    t->argument = argument;
    t->detached = attr && attr->detachstate == PTHREAD_CREATE_DETACHED;
    unsigned size = attr ? (unsigned)attr->stacksize : 512 * 1024;
    void *stack = attr ? attr->stackaddr : NULL;
    int result = 0;
    if (!stack) {
        size = (size + 4095) & ~4095u;
        t->pages = size / 4096 + 2;
        sos2_status mapping = sos2_map(t->pages, 3);
        if (mapping < 0) result = sos2_errno(mapping);
        else {
            t->mapping = (void *)(uintptr_t)mapping;
            stack = (char *)t->mapping + 4096;
            int a = call3(SYS_VM_PROTECT, mapping, 1, 0),
                b = call3(SYS_VM_PROTECT, mapping + 4096 + size, 1, 0);
            if (a < 0 || b < 0) result = sos2_errno(a < 0 ? a : b);
        }
    }
    if (!result) result = init_tls(t);
    if (!result) {
#ifdef __x86_64__
        struct user_thread_start start = {sos2_ptr(trampoline),     sos2_ptr(t),
                                          sos2_ptr(stack),          size,
                                          sos2_ptr(tls_pointer(t)), sos2_ptr(&t->completion)};
#else
        struct user_thread_start start = {sos2_ptr(trampoline), sos2_ptr(t),
                                          sos2_ptr(stack),      size,
                                          sos2_ptr(t->tls),     sos2_ptr(&t->completion)};
#endif
        int created = call(SYS_THREAD_CREATE, sos2_ptr(&start), 0);
        if (created < 0) result = sos2_errno(created);
        else {
            *id = created;
            sos2_store(&t->tid, created);
        }
    }
    if (result) release(t);
    sos2_unlock(&registry_lock);
    return result;
}

void pthread_exit(void *value) {
    struct sos2_thread *t = sos2_self();
    t->result = value;
    for (unsigned pass = 0; pass < 4; ++pass) {
        int again = 0;
        for (unsigned i = 0; i < SOS2_KEYS; ++i) {
            sos2_lock(&registry_lock);
            void *value = (void *)t->keys[i];
            void (*destroy)(void *) = keys[i].used && keys[i].generation == t->key_generations[i]
                                          ? keys[i].destroy
                                          : NULL;
            t->keys[i] = NULL;
            sos2_unlock(&registry_lock);
            if (value && destroy) {
                destroy(value);
                again = 1;
            }
        }
        if (!again) break;
    }
    _reclaim_reent(&t->reent);
    call(SYS_THREAD_EXIT, 0, 0);
    for (;;) { }
}

int pthread_join(pthread_t id, void **value) {
    if (id == pthread_self()) return EDEADLK;
    sos2_lock(&registry_lock);
    struct sos2_thread *t = lookup(id);
    int result = !t ? ESRCH : t->detached || t->joining ? EINVAL : 0;
    if (!result) t->joining = 1;
    sos2_unlock(&registry_lock);
    if (result) return result;
    for (;;) {
        unsigned live = sos2_load(&t->completion);
        if (!live) break;
        sos2_wait(&t->completion, live, USER_WAIT_FOREVER);
    }
    result = sos2_errno(call(SYS_THREAD_JOIN, id, 0));
    sos2_lock(&registry_lock);
    if (!result) {
        if (value) *value = t->result;
        release(t);
    } else t->joining = 0;
    sos2_unlock(&registry_lock);
    return result;
}

int pthread_detach(pthread_t id) {
    sos2_lock(&registry_lock);
    struct sos2_thread *t = lookup(id);
    int result = !t ? ESRCH : t->joining || t->detached ? EINVAL : 0;
    if (!result) {
        t->detached = 1;
        reap_detached();
    }
    sos2_unlock(&registry_lock);
    return result;
}

int pthread_getattr_np(pthread_t id, pthread_attr_t *attr) {
    struct user_thread_info info;
    int r = call(SYS_THREAD_INFO, id, sos2_ptr(&info));
    if (r < 0) return sos2_errno(r);
    pthread_attr_init(attr);
    attr->stackaddr = (void *)(uintptr_t)info.stack;
    attr->stacksize = info.size;
    sos2_lock(&registry_lock);
    struct sos2_thread *t = lookup(id);
    if (t && t->detached) attr->detachstate = PTHREAD_CREATE_DETACHED;
    sos2_unlock(&registry_lock);
    return 0;
}

int pthread_attr_init(pthread_attr_t *a) {
    memset(a, 0, sizeof(*a));
    a->is_initialized = 1;
    a->stacksize = 512 * 1024;
    a->detachstate = PTHREAD_CREATE_JOINABLE;
    a->contentionscope = PTHREAD_SCOPE_SYSTEM;
    a->inheritsched = PTHREAD_INHERIT_SCHED;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *a) {
    a->is_initialized = 0;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *a, size_t n) {
    if (n < 4096 || n > 4 * 1024 * 1024) return EINVAL;
    a->stacksize = n;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n) {
    *n = a->stacksize;
    return 0;
}

int pthread_attr_setstack(pthread_attr_t *a, void *p, size_t n) {
    if (!p || (sos2_ptr(p) & (USER_STACK_ALIGN - 1)) || (n & (USER_STACK_ALIGN - 1))) return EINVAL;
    int r = pthread_attr_setstacksize(a, n);
    if (!r) a->stackaddr = p;
    return r;
}

int pthread_attr_getstack(const pthread_attr_t *a, void **p, size_t *n) {
    *p = a->stackaddr;
    *n = a->stacksize;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *a, int state) {
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED) return EINVAL;
    a->detachstate = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state) {
    *state = a->detachstate;
    return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size) {
    *size = a->stackaddr ? 0 : 4096;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *a, size_t size) {
    (void)a;
    return size == 4096 ? 0 : ENOTSUP;
}

int pthread_attr_setscope(pthread_attr_t *a, int scope) {
    if (scope != PTHREAD_SCOPE_SYSTEM) return ENOTSUP;
    a->contentionscope = scope;
    return 0;
}

int pthread_setname_np(pthread_t id, const char *name) {
    (void)id;
    (void)name;
    return ENOTSUP;
}

int sos2_thread_suspend(pthread_t id) { return sos2_errno(call(SYS_THREAD_SUSPEND, id, 0)); }

int sos2_thread_resume(pthread_t id) { return sos2_errno(call(SYS_THREAD_RESUME, id, 0)); }

int sos2_thread_context(pthread_t id, struct user_thread_context *out) {
    return sos2_errno(call(SYS_THREAD_CONTEXT, id, sos2_ptr(out)));
}

int pthread_once(pthread_once_t *once, void (*initialize)(void)) {
    uint32_t *state = (uint32_t *)&once->init_executed;
    if (sos2_load(state) == 2) return 0;
    if (sos2_cas(state, 0, 1)) {
        initialize();
        sos2_store(state, 2);
        sos2_wake(state, UINT32_MAX);
    } else
        while (sos2_load(state) != 2) sos2_wait(state, 1, USER_WAIT_FOREVER);
    return 0;
}

int pthread_key_create(pthread_key_t *key, void (*destroy)(void *)) {
    sos2_lock(&registry_lock);
    int r = EAGAIN;
    for (unsigned i = 0; i < SOS2_KEYS; ++i)
        if (!keys[i].used) {
            keys[i].used = 1;
            keys[i].destroy = destroy;
            ++keys[i].generation;
            *key = i;
            r = 0;
            break;
        }
    sos2_unlock(&registry_lock);
    return r;
}

int pthread_key_delete(pthread_key_t key) {
    sos2_lock(&registry_lock);
    int r = key >= SOS2_KEYS || !keys[key].used ? EINVAL : 0;
    if (!r) keys[key].used = 0;
    sos2_unlock(&registry_lock);
    return r;
}

/* Without the registry's lock: WebKit asks on every Thread::current(). A
 * thread's values are its own, and a key's generation tells a value set under
 * it from one set before it was deleted and created again; using a key while
 * another thread deletes it is undefined, as in POSIX. */
void *pthread_getspecific(pthread_key_t key) {
    struct sos2_thread *t = self_thread();
    if (key >= SOS2_KEYS || !t) return NULL;
    unsigned generation = __atomic_load_n(&keys[key].generation, __ATOMIC_RELAXED);
    return __atomic_load_n(&keys[key].used, __ATOMIC_RELAXED) &&
                   t->key_generations[key] == generation
               ? (void *)t->keys[key]
               : NULL;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    sos2_lock(&registry_lock);
    int r = key >= SOS2_KEYS || !keys[key].used ? EINVAL : 0;
    if (!r) {
        struct sos2_thread *t = sos2_self();
        t->keys[key] = value;
        t->key_generations[key] = keys[key].generation;
    }
    sos2_unlock(&registry_lock);
    return r;
}

#define OBJECTS 512

static struct mutex {
    uint32_t used;
    int type;
    struct sos2_lock lock;
} mutexes[OBJECTS];

static struct condition {
    uint32_t used, sequence, waiters;
    clockid_t clock;
} conditions[OBJECTS];

static int allocate_mutex(int type) {
    for (unsigned i = 0; i < OBJECTS; ++i)
        if (sos2_cas(&mutexes[i].used, 0, 1)) {
            mutexes[i].type = type;
            return i + 1;
        }
    return 0;
}

static struct mutex *mutex(pthread_mutex_t *handle) {
    unsigned index = sos2_load(handle);
    if (index == UINT32_MAX) {
        unsigned fresh = allocate_mutex(PTHREAD_MUTEX_NORMAL);
        if (!fresh) return NULL;
        if (sos2_cas(handle, UINT32_MAX, fresh)) index = fresh;
        else {
            sos2_store(&mutexes[fresh - 1].used, 0);
            index = sos2_load(handle);
        }
    }
    return index && index <= OBJECTS && sos2_load(&mutexes[index - 1].used) ? &mutexes[index - 1]
                                                                            : NULL;
}

int pthread_mutex_init(pthread_mutex_t *handle, const pthread_mutexattr_t *attr) {
    unsigned index = allocate_mutex(attr ? attr->type : PTHREAD_MUTEX_NORMAL);
    if (!index) return EAGAIN;
    sos2_store(handle, index);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *handle) {
    if (sos2_load(handle) == UINT32_MAX) {
        sos2_store(handle, 0);
        return 0;
    }
    struct mutex *m = mutex(handle);
    if (!m) return EINVAL;
    if (sos2_load(&m->lock.owner)) return EBUSY;
    sos2_store(handle, 0);
    sos2_store(&m->used, 0);
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *handle) {
    struct mutex *m = mutex(handle);
    if (!m) return EINVAL;
    if (m->type != PTHREAD_MUTEX_RECURSIVE && sos2_load(&m->lock.owner) == self_id())
        return EDEADLK;
    sos2_lock(&m->lock);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *handle) {
    struct mutex *m = mutex(handle);
    if (!m) return EINVAL;
    if (m->type != PTHREAD_MUTEX_RECURSIVE && sos2_load(&m->lock.owner) == self_id())
        return EBUSY;
    return sos2_trylock(&m->lock) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *handle) {
    struct mutex *m = mutex(handle);
    if (!m) return EINVAL;
    if (sos2_load(&m->lock.owner) != self_id()) return EPERM;
    sos2_unlock(&m->lock);
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *handle, const struct timespec *deadline) {
    struct mutex *m = mutex(handle);
    if (!m) return EINVAL;
    if (m->type != PTHREAD_MUTEX_RECURSIVE && sos2_load(&m->lock.owner) == self_id())
        return EDEADLK;
    if (sos2_trylock(&m->lock)) return 0;
    if (!deadline || deadline->tv_sec < 0 || deadline->tv_nsec < 0 ||
        deadline->tv_nsec >= 1000000000)
        return EINVAL;
    uint64_t end = (uint64_t)deadline->tv_sec * 1000000 + (deadline->tv_nsec + 999) / 1000;
    int r = 0;
    __atomic_add_fetch(&m->lock.waiters, 1, __ATOMIC_ACQ_REL);
    while (!sos2_trylock(&m->lock)) {
        uint64_t now;
        int status = sos2_clock_read(1, &now);
        if (status < 0) {
            r = sos2_errno(status);
            break;
        }
        if (now >= end) {
            r = ETIMEDOUT;
            break;
        }
        uint64_t left = end - now;
        unsigned owner = sos2_load(&m->lock.owner);
        if (owner)
            sos2_wait(&m->lock.owner, owner, left >= UINT32_MAX ? UINT32_MAX - 1 : (uint32_t)left);
    }
    __atomic_sub_fetch(&m->lock.waiters, 1, __ATOMIC_ACQ_REL);
    return r;
}

int pthread_mutexattr_init(pthread_mutexattr_t *a) {
    memset(a, 0, sizeof(*a));
    a->is_initialized = 1;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *a) {
    a->is_initialized = 0;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) {
    if (type < 0 || type > PTHREAD_MUTEX_DEFAULT) return EINVAL;
    a->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type) {
    *type = a->type;
    return 0;
}

static int allocate_condition(clockid_t clock) {
    for (unsigned i = 0; i < OBJECTS; ++i)
        if (sos2_cas(&conditions[i].used, 0, 1)) {
            conditions[i].clock = clock;
            conditions[i].sequence = 0;
            conditions[i].waiters = 0;
            return i + 1;
        }
    return 0;
}

static struct condition *condition(pthread_cond_t *handle) {
    unsigned index = sos2_load(handle);
    if (index == UINT32_MAX) {
        unsigned fresh = allocate_condition(CLOCK_REALTIME);
        if (!fresh) return NULL;
        if (sos2_cas(handle, UINT32_MAX, fresh)) index = fresh;
        else {
            sos2_store(&conditions[fresh - 1].used, 0);
            index = sos2_load(handle);
        }
    }
    return index && index <= OBJECTS && sos2_load(&conditions[index - 1].used)
               ? &conditions[index - 1]
               : NULL;
}

int pthread_cond_init(pthread_cond_t *handle, const pthread_condattr_t *attr) {
    unsigned index = allocate_condition(attr ? attr->clock : CLOCK_REALTIME);
    if (!index) return EAGAIN;
    sos2_store(handle, index);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *handle) {
    if (sos2_load(handle) == UINT32_MAX) {
        sos2_store(handle, 0);
        return 0;
    }
    struct condition *c = condition(handle);
    if (!c) return EINVAL;
    if (sos2_load(&c->waiters)) return EBUSY;
    sos2_store(handle, 0);
    sos2_store(&c->used, 0);
    return 0;
}

static int notify(pthread_cond_t *handle, unsigned count) {
    struct condition *c = condition(handle);
    if (!c) return EINVAL;
    __atomic_add_fetch(&c->sequence, 1, __ATOMIC_RELEASE);
    sos2_wake(&c->sequence, count);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) { return notify(c, 1); }

int pthread_cond_broadcast(pthread_cond_t *c) { return notify(c, UINT32_MAX); }

static int wait_condition(pthread_cond_t *handle, pthread_mutex_t *m, clockid_t clock,
                          const struct timespec *deadline) {
    struct condition *c = condition(handle);
    if (!c) return EINVAL;
    uint64_t end = 0;
    if (deadline) {
        if ((clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) || deadline->tv_sec < 0 ||
            deadline->tv_nsec < 0 || deadline->tv_nsec >= 1000000000)
            return EINVAL;
        end = (uint64_t)deadline->tv_sec * 1000000 + (deadline->tv_nsec + 999) / 1000;
    }
    __atomic_add_fetch(&c->waiters, 1, __ATOMIC_RELAXED);
    unsigned sequence = sos2_load(&c->sequence);
    int r = pthread_mutex_unlock(m);
    if (r) {
        __atomic_sub_fetch(&c->waiters, 1, __ATOMIC_RELAXED);
        return r;
    }
    while (sos2_load(&c->sequence) == sequence) {
        unsigned us = USER_WAIT_FOREVER;
        if (deadline) {
            uint64_t now;
            int clock_status = sos2_clock_read(clock == CLOCK_REALTIME, &now);
            if (clock_status < 0) {
                r = sos2_errno(clock_status);
                break;
            }
            if (now >= end) {
                r = ETIMEDOUT;
                break;
            }
            uint64_t left = end - now;
            us = left >= UINT32_MAX ? UINT32_MAX - 1 : left;
        }
        int status = sos2_wait(&c->sequence, sequence, us);
        if (status == USER_ETIMEDOUT && deadline) continue;
        r = status == USER_EAGAIN ? 0 : sos2_errno(status);
        break;
    }
    __atomic_sub_fetch(&c->waiters, 1, __ATOMIC_RELAXED);
    int locked = pthread_mutex_lock(m);
    return locked ? locked : r;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return wait_condition(c, m, CLOCK_REALTIME, NULL);
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline) {
    struct condition *object = condition(c);
    return object ? wait_condition(c, m, object->clock, deadline) : EINVAL;
}

int pthread_cond_clockwait(pthread_cond_t *c, pthread_mutex_t *m, clockid_t clock,
                           const struct timespec *deadline) {
    return wait_condition(c, m, clock, deadline);
}

int pthread_condattr_init(pthread_condattr_t *a) {
    memset(a, 0, sizeof(*a));
    a->is_initialized = 1;
    a->clock = CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *a) {
    a->is_initialized = 0;
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clock) {
    if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) return EINVAL;
    a->clock = clock;
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clock) {
    *clock = a->clock;
    return 0;
}

/* Reader-writer locks: one state word, the writer bit or a reader count.
 * Readers share the lock whenever no writer holds it, so a thread may take
 * read locks recursively. Waiters sleep on the state word. */
#define RWLOCK_WRITER 0x80000000u

static struct rwlock {
    uint32_t used, state, waiters;
} rwlocks[OBJECTS];

static int allocate_rwlock(void) {
    for (unsigned i = 0; i < OBJECTS; ++i)
        if (sos2_cas(&rwlocks[i].used, 0, 1)) {
            sos2_store(&rwlocks[i].state, 0);
            sos2_store(&rwlocks[i].waiters, 0);
            return i + 1;
        }
    return 0;
}

static struct rwlock *rwlock(pthread_rwlock_t *handle) {
    unsigned index = sos2_load(handle);
    if (index == UINT32_MAX) {
        unsigned fresh = allocate_rwlock();
        if (!fresh) return NULL;
        if (sos2_cas(handle, UINT32_MAX, fresh)) index = fresh;
        else {
            sos2_store(&rwlocks[fresh - 1].used, 0);
            index = sos2_load(handle);
        }
    }
    return index && index <= OBJECTS && sos2_load(&rwlocks[index - 1].used) ? &rwlocks[index - 1]
                                                                            : NULL;
}

int pthread_rwlock_init(pthread_rwlock_t *handle, const pthread_rwlockattr_t *attr) {
    (void)attr;
    unsigned index = allocate_rwlock();
    if (!index) return EAGAIN;
    sos2_store(handle, index);
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *handle) {
    if (sos2_load(handle) == UINT32_MAX) {
        sos2_store(handle, 0);
        return 0;
    }
    struct rwlock *l = rwlock(handle);
    if (!l) return EINVAL;
    if (sos2_load(&l->state)) return EBUSY;
    sos2_store(handle, 0);
    sos2_store(&l->used, 0);
    return 0;
}

static int rwlock_acquire(pthread_rwlock_t *handle, bool write, bool wait) {
    struct rwlock *l = rwlock(handle);
    if (!l) return EINVAL;
    for (;;) {
        uint32_t state = sos2_load(&l->state);
        if (write ? !state : !(state & RWLOCK_WRITER)) {
            if (sos2_cas(&l->state, state, write ? RWLOCK_WRITER : state + 1)) return 0;
            continue;
        }
        if (!wait) return EBUSY;
        __atomic_add_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
        sos2_wait(&l->state, state, USER_WAIT_FOREVER);
        __atomic_sub_fetch(&l->waiters, 1, __ATOMIC_ACQ_REL);
    }
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l) { return rwlock_acquire(l, false, true); }

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l) { return rwlock_acquire(l, false, false); }

int pthread_rwlock_wrlock(pthread_rwlock_t *l) { return rwlock_acquire(l, true, true); }

int pthread_rwlock_trywrlock(pthread_rwlock_t *l) { return rwlock_acquire(l, true, false); }

int pthread_rwlock_unlock(pthread_rwlock_t *handle) {
    struct rwlock *l = rwlock(handle);
    if (!l) return EINVAL;
    uint32_t state = sos2_load(&l->state);
    if (!state) return EPERM;
    if (state == RWLOCK_WRITER) __atomic_store_n(&l->state, 0, __ATOMIC_SEQ_CST);
    else __atomic_sub_fetch(&l->state, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&l->waiters, __ATOMIC_SEQ_CST)) sos2_wake(&l->state, UINT32_MAX);
    return 0;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *a) {
    memset(a, 0, sizeof(*a));
    a->is_initialized = 1;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) {
    a->is_initialized = 0;
    return 0;
}

/* Barriers: the last thread to arrive resets the count, then advances the
 * generation that the others sleep on. */
#define BARRIERS 64

static struct barrier {
    uint32_t used, count, arrived, generation;
} barriers[BARRIERS];

static struct barrier *barrier(pthread_barrier_t *handle) {
    unsigned index = sos2_load(handle);
    return index && index <= BARRIERS && sos2_load(&barriers[index - 1].used) ? &barriers[index - 1]
                                                                              : NULL;
}

int pthread_barrier_init(pthread_barrier_t *handle, const pthread_barrierattr_t *attr,
                         unsigned count) {
    (void)attr;
    if (!count) return EINVAL;
    for (unsigned i = 0; i < BARRIERS; ++i)
        if (sos2_cas(&barriers[i].used, 0, 1)) {
            barriers[i].count = count;
            sos2_store(&barriers[i].arrived, 0);
            sos2_store(&barriers[i].generation, 0);
            sos2_store(handle, i + 1);
            return 0;
        }
    return EAGAIN;
}

int pthread_barrier_destroy(pthread_barrier_t *handle) {
    struct barrier *b = barrier(handle);
    if (!b) return EINVAL;
    if (sos2_load(&b->arrived)) return EBUSY;
    sos2_store(handle, 0);
    sos2_store(&b->used, 0);
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *handle) {
    struct barrier *b = barrier(handle);
    if (!b) return EINVAL;
    uint32_t generation = sos2_load(&b->generation);
    if (__atomic_add_fetch(&b->arrived, 1, __ATOMIC_ACQ_REL) == b->count) {
        sos2_store(&b->arrived, 0);
        __atomic_add_fetch(&b->generation, 1, __ATOMIC_RELEASE);
        sos2_wake(&b->generation, UINT32_MAX);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (sos2_load(&b->generation) == generation)
        sos2_wait(&b->generation, generation, USER_WAIT_FOREVER);
    return 0;
}

int pthread_barrierattr_init(pthread_barrierattr_t *a) {
    memset(a, 0, sizeof(*a));
    a->is_initialized = 1;
    return 0;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t *a) {
    a->is_initialized = 0;
    return 0;
}
