#include "process.h"
#include "gpu_client.h"
#include "elf_load.h"
#include "platform.h"
#include "graphics.h"
#include "page_host.h"
#include "runtime_files.h"
#include "apps.h"
#include "app_store.h"
#include "system_clock.h"
#include "battery.h"
#include "fs.h"
#include "entropy.h"
#include "network.h"
#include "arm_machine.h"
#include "arm_smp.h"
#include "kernel_io.h"
#include "boot_progress.h"
#include "system_update.h"
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_startup.h"
#include "redmi2_adreno.h"
#endif
#include <stddef.h>

enum state { RUNNABLE, FAULTED, EXITED };

enum thread_state { T_UNUSED, T_READY, T_SLEEP, T_WAIT, T_SERVICE, T_QUIESCE, T_DONE };

struct thread {
    struct arm_context context;
    struct arm_runtime_context runtime;
    enum thread_state state;
    uint32_t id;
    user_ptr stack;
    uint32_t size;
    user_ptr completion;
    uint32_t result;
    user_ptr wait_address;
    uint32_t suspended;
    unsigned running_cpu;
    bool resume;
    uint64_t deadline, cpu_us, slice_deadline;
};

/* What first-fit searches in the heap or the JIT window found: no free run of
 * count pages or more starts between the range's start and address. Mapping
 * keeps that true and unmapping lowers it (lower_hints), so a search for
 * count pages starts at address instead of walking every mapped page's table
 * entries (uncached) from the start, as JavaScriptCore's per-block mappings
 * would otherwise make every search do. */
struct range_hint {
    user_ptr address;
    unsigned count; /* 0: unused */
};

#define RANGE_HINTS 4

struct process {
    struct process *next;
    struct vm_space space;
    struct range_hint hints[2][RANGE_HINTS]; /* The heap's, then the JIT window's */
    unsigned next_hint[2];
    struct gpu_client *gpu; /* SYS_GPU context and resources, if any */
    struct thread threads[USER_THREAD_LIMIT];
    unsigned next_thread, next_service_thread;
    uint32_t next_tid, peak_pages;
    struct fs_space files;
    enum state state;
    uint32_t pid;
    uint64_t cpu_us;
    bool reap_pending, restart;
    const struct app *app; /* Its program and storage; NULL: a web page */
    unsigned running_mask;
};

#define PROCESS_PAGES ((sizeof(struct process) + PAGE_SIZE - 1) / PAGE_SIZE)
static struct process *processes;
static struct process *next_process, *next_io;

static struct process *process_for(unsigned slot) {
    for (struct process *p = processes; p; p = p->next)
        if (p->pid == slot + 1) return p;
    return 0;
}

static void remove_process(struct process *p) {
    struct process **link = &processes;
    while (*link && *link != p) link = &(*link)->next;
    if (!*link) panic("process list ownership");
    *link = p->next;
    if (next_process == p) next_process = p->next ? p->next : processes;
    if (next_io == p) next_io = p->next ? p->next : processes;
    vm_cpu_free(p, PROCESS_PAGES);
}
static struct process *current;
static struct thread *current_thread;
static bool random_ready;
static unsigned baseline_pages;
static unsigned next_cpu = 1;

static struct {
    struct process *process;
    struct thread *thread;
} dispatched[ARM_CPU_MAX];

static bool continue_slice;
static struct process *io_owner;
static bool frozen;
static struct thread *io_thread;
static bool background_turn;
extern char __image_start[], __image_end[];

/* sos2.jsc=OPTIONS: JavaScriptCore options for every web page process,
 * comma-separated (programs/browser/web_main.cpp), to compare the engine's
 * tiers without rebuilding it. */
static char engine_options[USER_ENGINE_OPTIONS_MAX];

static void engine_options_init(const char *args) {
    static const char prefix[] = "sos2.jsc=";
    const size_t skip = sizeof(prefix) - 1;
    while (*args) {
        args += strspn(args, " \t");
        size_t n = strcspn(args, " \t");
        const char *token = args;
        args += n;
        if (n < skip || memcmp(token, prefix, skip)) continue;
        if (engine_options[0] || n - skip >= sizeof(engine_options))
            panic("invalid sos2.jsc boot argument");
        memcpy(engine_options, token + skip, n - skip);
    }
}

static bool spawn(struct process *p, uint32_t pid, const struct app *app) {
    *p = (struct process){.next = p->next, .pid = pid, .state = RUNNABLE, .app = app};
    const struct runtime_file *program = app ? app->program : runtime_file_find("/bin/sos2-web");
    if (!program && !(app && app->stored)) return false;
    /* A failed start is removed without another reap: release the per-process
     * file tables here, or the pool baseline check would report a leak. */
    char profile[FS_PROFILE_MAX] = "browser";
    if (app && app->system) strcpy(profile, "updater");
    else if (app) {
        memcpy(profile, "apps/", 5);
        strcpy(profile + 5, app->id);
    }
    if (!fs_space_init(&p->files, profile)) {
        fs_space_close(&p->files);
        return false;
    }
    if (!vm_create(&p->space)) {
        fs_space_close(&p->files);
        return false;
    }
    /* An installed app's program comes from the app store, checked against
     * its SHA-256, into pages released once it is loaded. */
    user_ptr entry;
    uint32_t size = program ? program->size : 0;
    const void *image = program ? runtime_file_data(program) : 0;
    unsigned char *read = 0;
    unsigned read_pages = 0;
    if (!program && !app_store_program(app, &read, &read_pages, &image, &size)) goto failed;
    bool loaded = elf_load(&p->space, image, size, !app, &entry);
    if (read) vm_cpu_free(read, read_pages);
    if (!loaded) goto failed;
    user_ptr bottom = WEB_STACK_BOTTOM, top = WEB_STACK_TOP;
    for (user_ptr page = bottom; page < top; page += PAGE_SIZE) {
        if (!vm_map(&p->space, page, VM_READ | VM_WRITE)) goto failed;
        if ((page & 0xffffu) == 0xf000u) kernel_io_wait();
    }
    user_ptr options = 0;
    if (!app && engine_options[0]) {
        size_t n = strlen(engine_options) + 1;
        top = (top - n) & ~(user_ptr)(USER_STACK_ALIGN - 1);
        options = top;
        if (!vm_copy_to_user(&p->space, options, engine_options, n)) goto failed;
    }
    /* A desktop Close/Open can arrive while the worker yields. Keep the
     * partial image owned until reaping, and never run a cancelled load. */
    if (p->state != RUNNABLE) return true;
    p->threads[0] = (struct thread){.state = T_READY,
                                    .id = 1,
                                    .stack = bottom,
                                    .size = (uint32_t)(top - bottom),
                                    .context = {USER_CONTEXT_FIELDS(entry, top)}};
    p->next_tid = 2;
    p->peak_pages = p->space.mapped_pages;
    p->threads[0].context.r[0] = 0;
    p->threads[0].context.r[1] = pid;
    p->threads[0].context.r[2] = options;

    return true;
failed:
    vm_destroy(&p->space);
    fs_space_close(&p->files);
    return false;
}

bool process_init(struct platform_devices *devices, uintptr_t dtb) {
    if (!runtime_files_init(devices, dtb, (uintptr_t)__image_start, (uintptr_t)__image_end)) {
        if (devices->has_initrd_start || devices->has_initrd_end) return false;
    }
    engine_options_init(devices->bootargs);
    system_update_init(devices);
    apps_init();
    page_host_redraw();
    boot_progress_report(BOOT_MEMORY, 0, 0);
    if (!arm_machine_init(devices) || !vm_init(devices, dtb)) return false;
    if (!arm_timer_init(&arm_machine_get()->timer)) panic(arm_timer_error());
    arm_runtime_init();
    arm_smp_init(devices);
    boot_progress_report(BOOT_STORAGE, 0, 0);
    fs_init(devices);
    boot_progress_report(BOOT_SECURITY, 0, 0);
    random_ready = entropy_init(devices);
    boot_progress_report(BOOT_NETWORK, 0, 0);
    network_init(devices);
    boot_progress_report(BOOT_CLOCK, 0, 0);
    const struct system_clock_ops clock = {arm_rtc_read, arm_rtc_kind()};
    system_clock_init(&clock);
    battery_init();
    battery_service(0); /* First bus setup/sample belongs to the loading screen. */
    page_host_device_status(random_ready, fs_persistent_ready(), fs_session_only());
    page_host_entropy_status(random_ready, entropy_error());
#ifdef SOS2_BOARD_REDMI2
    /* Long-lived driver buffers belong to the system, not a browser process.
     * Finish loading-screen services before recording the reclamation baseline,
     * including an Adreno arena retained after a failed hardware self-test. */
    redmi2_startup_wait_wifi();
    redmi2_adreno_init(&devices->framebuffer);
#endif
    baseline_pages = pages_available();
    return true;
}

static struct thread *thread_by_id(struct process *p, uint32_t id) {
    for (unsigned i = 0; i < USER_THREAD_LIMIT; ++i)
        if (p->threads[i].state != T_UNUSED && p->threads[i].id == id) return &p->threads[i];
    return 0;
}

static bool store_word(struct process *p, user_ptr address, uint32_t value) {
    uint32_t *word = (uint32_t *)vm_resolve(&p->space, address, VM_WRITE);
    if ((address & 3) || !word) return false;
    arch_memory_barrier();
    __atomic_store_n(word, value, __ATOMIC_RELAXED);
    return true;
}

static bool load_word(struct process *p, user_ptr address, uint32_t *value) {
    const uint32_t *word = (const uint32_t *)vm_resolve(&p->space, address, VM_READ);
    if ((address & 3) || !word) return false;
    *value = __atomic_load_n(word, __ATOMIC_RELAXED);
    arch_memory_barrier();
    return true;
}

static unsigned wake_word(struct process *p, user_ptr address, unsigned count) {
    unsigned woken = 0;
    for (unsigned i = 0; i < USER_THREAD_LIMIT && woken < count; ++i) {
        struct thread *t = &p->threads[i];
        if (t->state == T_WAIT && t->wait_address == address) {
            t->state = T_READY;
            t->context.r[0] = 0;
            ++woken;
        }
    }
    return woken;
}

/* SYS_THREAD_CONTEXT's struct user_thread_context: the saved registers and,
 * on arm64, the SIMD and floating-point state, which garbage collectors also
 * scan for pointers. */
static bool copy_thread_context(struct process *p, user_ptr address, const struct thread *t) {
#ifdef __aarch64__
    struct user_thread_context out;
    _Static_assert(sizeof(out.r) + 3 * sizeof(out.sp) == sizeof(t->context) &&
                       sizeof(out.v) == sizeof(t->runtime.v),
                   "thread context layout");
    memcpy(&out, &t->context, sizeof(t->context));
    memcpy(out.v, t->runtime.v, sizeof(out.v));
    out.fpsr = t->runtime.fpsr;
    out.fpcr = t->runtime.fpcr;
    return vm_copy_to_user(&p->space, address, &out, sizeof(out));
#else
    _Static_assert(sizeof(struct user_thread_context) == sizeof(t->context),
                   "thread context layout");
    return vm_copy_to_user(&p->space, address, &t->context, sizeof(t->context));
#endif
}

static int32_t create_thread(struct process *p, user_ptr address) {
    struct user_thread_start start;
    if (!vm_copy_from_user(&p->space, &start, address, sizeof(start))) return USER_EFAULT;
    if ((start.entry & 3) || !vm_resolve(&p->space, start.entry, VM_EXEC) ||
        (start.stack & (USER_STACK_ALIGN - 1)) || (start.size & (USER_STACK_ALIGN - 1)) ||
        start.size < PAGE_SIZE || start.size > 4 * 1024 * 1024 ||
        !vm_accessible(&p->space, start.stack, start.size, VM_WRITE) ||
        (start.tls && !vm_accessible(&p->space, start.tls, 4, VM_READ)) ||
        (start.completion &&
         ((start.completion & 3) || !vm_accessible(&p->space, start.completion, 4, VM_WRITE))))
        return USER_EINVAL;
    unsigned index = 0;
    while (index < USER_THREAD_LIMIT && p->threads[index].state != T_UNUSED) ++index;
    if (index == USER_THREAD_LIMIT || p->next_tid > INT32_MAX) return USER_EAGAIN;
    uint32_t id = p->next_tid++;
    if (start.completion && !store_word(p, start.completion, id)) return USER_EFAULT;
    p->threads[index] =
        (struct thread){.state = T_READY,
                        .id = id,
                        .stack = start.stack,
                        .size = (uint32_t)start.size,
                        .completion = start.completion,
                        .runtime = {.thread_pointer = start.tls},
                        .context = {.r = {start.argument},
                                    USER_CONTEXT_FIELDS(start.entry, start.stack + start.size)}};
    return (int32_t)id;
}

static void exit_thread(struct process *p, struct thread *t, uint32_t result) {
    t->result = result;
    t->state = T_DONE;
    if (t->completion) {
        store_word(p, t->completion, 0);
        wake_word(p, t->completion, USER_THREAD_LIMIT);
    }
    for (unsigned i = 0; i < USER_THREAD_LIMIT; ++i)
        if (p->threads[i].state != T_UNUSED && p->threads[i].state != T_DONE) return;
    p->state = EXITED;
}

/* [lower, upper) lies in the engine's JIT window (include/user_abi.h), which
 * only web page processes may use. */
static bool jit_window(const struct process *p, user_ptr lower, user_ptr upper) {
    return !p->app && lower >= USER_JIT_BASE && lower < upper && upper <= USER_JIT_END;
}

static unsigned hint_range(user_ptr address) {
    return address >= USER_JIT_BASE && address < USER_JIT_END;
}

/* Pages from address on were unmapped: a run of count pages may now start up
 * to count - 1 pages below it (runs below a hint are shorter than count). */
static void lower_hints(struct process *p, user_ptr address) {
    unsigned range = hint_range(address);
    user_ptr base = range ? USER_JIT_BASE : USER_HEAP_BASE;
    for (unsigned i = 0; i < RANGE_HINTS; ++i) {
        struct range_hint *hint = &p->hints[range][i];
        user_ptr reach = hint->count ? (user_ptr)(hint->count - 1) * PAGE_SIZE : 0;
        user_ptr floor = address - base > reach ? address - reach : base;
        if (hint->address > floor) hint->address = floor;
    }
}

/* First-fit placement of count pages in [lower, upper), within the heap or
 * the JIT window, resumed where earlier searches for count pages ended. */
static user_ptr allocate_range(struct process *p, unsigned count, user_ptr lower, user_ptr upper,
                               unsigned flags) {
    unsigned range = hint_range(lower);
    struct range_hint *hint = 0;
    for (unsigned i = 0; i < RANGE_HINTS; ++i)
        if (p->hints[range][i].count == count) hint = &p->hints[range][i];
    user_ptr known = hint ? hint->address : range ? USER_JIT_BASE : USER_HEAP_BASE;
    user_ptr from = known > lower ? known : lower;
    if (from >= upper) return 0;
    user_ptr address = vm_allocate_range(&p->space, count, from, upper, flags);
    /* Only a search that covered the range from its start teaches anything;
     * a failure may be the page pool running out, not a lack of room. */
    if (address && from == known) {
        if (!hint) {
            hint = &p->hints[range][p->next_hint[range]];
            p->next_hint[range] = (p->next_hint[range] + 1) % RANGE_HINTS;
            hint->count = count;
        }
        hint->address = address + (user_ptr)count * PAGE_SIZE;
    }
    return address;
}

static user_result memory_call(struct process *p, uint32_t number, user_ptr address, unsigned count,
                               unsigned flags) {
    bool jit = jit_window(p, address, address + (user_ptr)count * PAGE_SIZE);
    if (!count || count > (USER_HEAP_END - USER_HEAP_BASE) / PAGE_SIZE || (flags & ~7u) ||
        ((flags & 6) == 6 && !jit) || (flags && !(flags & VM_READ)))
        return USER_EINVAL;
    if (number == SYS_VM_MAP && !address) {
        address = allocate_range(p, count, USER_HEAP_BASE, USER_HEAP_END, flags);
        return address ? (user_result)address : USER_ENOMEM;
    }
    if (number == SYS_VM_CHECK) {
        if ((address & (PAGE_SIZE - 1)) || address < USER_BASE || address >= USER_END ||
            count > (USER_END - address) / PAGE_SIZE)
            return USER_EINVAL;
        for (unsigned i = 0; i < count; ++i)
            if (!vm_mapped(&p->space, address + i * PAGE_SIZE)) return USER_EINVAL;
        return 0;
    }
    if ((address & (PAGE_SIZE - 1)) ||
        (!jit && (address < USER_HEAP_BASE || address >= USER_HEAP_END ||
                  count > (USER_HEAP_END - address) / PAGE_SIZE)))
        return USER_EINVAL;
    for (unsigned i = 0; i < count; ++i) {
        bool mapped = vm_mapped(&p->space, address + i * PAGE_SIZE);
        if (number == SYS_VM_MAP ? mapped : !mapped) return USER_EINVAL;
    }
    if (number == SYS_VM_MAP) {
        unsigned i = 0;
        while (i < count && vm_map(&p->space, address + i * PAGE_SIZE, flags)) ++i;
        if (i == count) return (user_result)address;
        if (i && !vm_release(&p->space, address, i)) panic("mapping rollback failed");
        return USER_ENOMEM;
    }
    if (number == SYS_VM_UNMAP) {
        /* One release, not a page at a time: it frees emptied tables once. */
        if (!vm_release(&p->space, address, count)) return USER_EINVAL;
        lower_hints(p, address);
        return 0;
    }
    for (unsigned i = 0; i < count; ++i) {
        user_ptr at = address + i * PAGE_SIZE;
        if (number == SYS_VM_DISCARD) vm_clear(&p->space, at);
        else vm_protect(&p->space, at, flags);
    }
    return 0;
}

static void syscall(struct process *p) {
    struct thread *t = current_thread;
    user_reg *r = t->context.r;
    switch (r[SYSCALL_NUMBER]) {
        case SYS_EXIT: p->state = EXITED; break;
        case SYS_YIELD: r[0] = 0; break;
        case SYS_SLEEP:
            if ((uint32_t)r[0] > 1000000) {
                r[0] = (user_reg)USER_EINVAL;
                break;
            }
            t->deadline = time_us() + (uint32_t)r[0];
            t->state = T_SLEEP;
            r[0] = 0;
            break;
        case SYS_GETPID: r[0] = p->pid; break;
        case SYS_CPU_COUNT: {
            unsigned mask = arm_smp_online(), count = 0;
            for (unsigned i = 0; i < ARM_CPU_MAX; ++i)
                if (mask & (1u << i)) ++count;
            /* CPU 0 is reserved for services when worker CPUs are available. */
            r[0] = count > 1 ? count - 1 : 1;
            break;
        }
        case SYS_THREAD_CREATE: r[0] = (user_reg)create_thread(p, r[0]); break;
        case SYS_THREAD_EXIT: exit_thread(p, t, (uint32_t)r[0]); break;
        case SYS_THREAD_ID: r[0] = t->id; break;
        case SYS_THREAD_TLS:
            if (r[0] && !vm_accessible(&p->space, r[0], 4, VM_READ)) r[0] = (user_reg)USER_EFAULT;
            else {
                t->runtime.thread_pointer = r[0];
                r[0] = 0;
            }
            break;
        case SYS_THREAD_JOIN: {
            struct thread *target = thread_by_id(p, (uint32_t)r[0]);
            if (!target) {
                r[0] = (user_reg)USER_ESRCH;
                break;
            }
            if (target == t) {
                r[0] = (user_reg)USER_EDEADLK;
                break;
            }
            if (target->state != T_DONE) {
                r[0] = (user_reg)USER_EAGAIN;
                break;
            }
            if (r[1] && !vm_copy_to_user(&p->space, r[1], &target->result, 4)) {
                r[0] = (user_reg)USER_EFAULT;
                break;
            }
            *target = (struct thread){0};
            r[0] = 0;
            break;
        }
        case SYS_THREAD_INFO: {
            struct thread *target = (uint32_t)r[0] ? thread_by_id(p, (uint32_t)r[0]) : t;
            if (!target) {
                r[0] = (user_reg)USER_ESRCH;
                break;
            }
            struct user_thread_info info = {.id = target->id,
                                            .stack = target->stack,
                                            .size = target->size,
                                            .tls = target->runtime.thread_pointer};
            r[0] =
                vm_copy_to_user(&p->space, r[1], &info, sizeof(info)) ? 0 : (user_reg)USER_EFAULT;
            break;
        }
        case SYS_THREAD_SUSPEND:
        case SYS_THREAD_RESUME:
        case SYS_THREAD_CONTEXT: {
            struct thread *target = thread_by_id(p, (uint32_t)r[0]);
            if (!target || target->state == T_DONE) {
                r[0] = (user_reg)USER_ESRCH;
                break;
            }
            if (target == t) {
                r[0] = (user_reg)USER_EDEADLK;
                break;
            }
            if (r[SYSCALL_NUMBER] == SYS_THREAD_SUSPEND) {
                if (target->suspended == 16) r[0] = (user_reg)USER_EAGAIN;
                else {
                    ++target->suspended;
                    r[0] = 0;
                }
            } else if (!target->suspended) r[0] = (user_reg)USER_EINVAL;
            else if (r[SYSCALL_NUMBER] == SYS_THREAD_RESUME) {
                --target->suspended;
                r[0] = 0;
            } else r[0] = copy_thread_context(p, r[1], target) ? 0 : (user_reg)USER_EFAULT;
            break;
        }
        case SYS_WAIT_WORD:
        case SYS_WAKE_WORD: {
            uint32_t value;
            if (!load_word(p, r[0], &value)) {
                r[0] = (user_reg)USER_EFAULT;
                break;
            }
            uint32_t expected = (uint32_t)r[1], timeout = (uint32_t)r[2];
            if (r[SYSCALL_NUMBER] == SYS_WAKE_WORD) r[0] = wake_word(p, r[0], expected);
            else if (value != expected) r[0] = (user_reg)USER_EAGAIN;
            else if (!timeout) r[0] = (user_reg)USER_ETIMEDOUT;
            else {
                t->state = T_WAIT;
                t->wait_address = r[0];
                t->deadline = timeout == USER_WAIT_FOREVER ? UINT64_MAX : time_us() + timeout;
                r[0] = 0;
            }
            break;
        }
        case SYS_CLOCK: {
            uint32_t clock = (uint32_t)r[0];
            if (clock > 3) {
                r[0] = (user_reg)USER_EINVAL;
                break;
            }
            uint64_t value = time_us();
            if (clock == 1) {
                if (!system_clock_read(&value)) {
                    r[0] = (user_reg)USER_ENODEV;
                    break;
                }
            } else if (clock == 2) value = p->cpu_us;
            else if (clock == 3) value = t->cpu_us;
            r[0] = vm_copy_to_user(&p->space, r[1], &value, 8) ? 0 : (user_reg)USER_EFAULT;
            break;
        }
        case SYS_RANDOM: {
            uint32_t size = (uint32_t)r[1];
            if (size > 256) {
                r[0] = (user_reg)USER_EINVAL;
                break;
            }
            if (!size) {
                r[0] = 0;
                break;
            }
            if (!vm_accessible(&p->space, r[0], size, VM_WRITE)) {
                r[0] = (user_reg)USER_EFAULT;
                break;
            }
            if (!random_ready) {
                r[0] = (user_reg)USER_ENODEV;
                break;
            }
            current_thread->state = T_SERVICE;
            continue_slice = false;
            break;
        }
        case SYS_VM_MAP:
        case SYS_VM_PROTECT:
        case SYS_VM_UNMAP:
        case SYS_VM_DISCARD:
        case SYS_VM_CHECK:
            r[0] = (user_reg)memory_call(p, (uint32_t)r[SYSCALL_NUMBER], r[0], (uint32_t)r[1],
                                         (uint32_t)r[2]);
            break;
        case SYS_VM_ALLOC_RANGE: {
            user_ptr lower = r[0], upper = r[1];
            unsigned count = (uint32_t)r[2], flags = (uint32_t)r[3];
            bool jit = jit_window(p, lower, upper);
            if (((lower < USER_HEAP_BASE || upper > USER_HEAP_END) && !jit) || lower >= upper ||
                ((lower | upper) & (PAGE_SIZE - 1)) || !count ||
                count > (upper - lower) / PAGE_SIZE || (flags & ~7u) ||
                ((flags & 6) == 6 && !jit) || (flags && !(flags & VM_READ))) {
                r[0] = (user_reg)USER_EINVAL;
                break;
            }
            user_ptr address = allocate_range(p, count, lower, upper, flags);
            r[0] = address ? address : (user_reg)USER_ENOMEM;
            break;
        }
        case SYS_MEMORY_INFO: {
            unsigned available = USER_PAGE_QUOTA - p->space.mapped_pages;
            if (available > pages_available()) available = pages_available();
            struct user_memory_info info = {PAGE_SIZE, p->space.mapped_pages, p->peak_pages,
                                            USER_PAGE_QUOTA, available};
            r[0] =
                vm_copy_to_user(&p->space, r[0], &info, sizeof(info)) ? 0 : (user_reg)USER_EFAULT;
            break;
        }
        case SYS_PRESENT:
            r[0] = (user_reg)page_host_present(p->pid - 1, &p->space, r[0], (uint32_t)r[1]);
            break;
        case SYS_PRESENT_DAMAGE: {
            struct user_present_damage damage;
            if (r[1] != sizeof(damage)) {
                r[0] = (user_reg)USER_EINVAL;
                break;
            }
            if (!vm_copy_from_user(&p->space, &damage, r[0], sizeof(damage))) {
                r[0] = (user_reg)USER_EFAULT;
                break;
            }
            r[0] = (user_reg)page_host_present_damage(p->pid - 1, &p->space, &damage);
            break;
        }
        case SYS_SURFACE_STATE: r[0] = page_host_surface_state(p->pid - 1); break;
        case SYS_PRESENT_FRAME: {
            struct user_present_frame frame;
            if (r[1] != sizeof(frame)) {
                r[0] = (user_reg)USER_EINVAL;
                break;
            }
            if (!vm_copy_from_user(&p->space, &frame, r[0], sizeof(frame))) {
                r[0] = (user_reg)USER_EFAULT;
                break;
            }
            r[0] = (user_reg)page_host_present_frame(p->pid - 1, &p->space, &frame);
            break;
        }
        case SYS_INPUT_METHOD:
            r[0] = (user_reg)page_host_input_method(p->pid - 1, (uint32_t)r[0]);
            break;
        case SYS_BACK_KEY: r[0] = (user_reg)page_host_back_key(p->pid - 1, (uint32_t)r[0]); break;
        case SYS_HAPTIC_TAP: r[0] = (user_reg)page_host_haptic_tap(p->pid - 1); break;
        case SYS_UPDATE:
            /* The system's updater's alone; INFO and APPS need no I/O. */
            if (!p->app || !p->app->system) r[0] = (user_reg)USER_ENOSYS;
            else if ((uint32_t)r[0] == USER_UPDATE_INFO)
                r[0] = (user_reg)system_update_info(&p->space, r[1], (uint32_t)r[2]);
            else if ((uint32_t)r[0] == USER_UPDATE_APPS)
                r[0] = (user_reg)system_update_apps(&p->space, r[1], (uint32_t)r[2]);
            else {
                current_thread->state = T_SERVICE;
                continue_slice = false;
            }
            break;
        case SYS_BROWSER_COMMAND: {
            struct user_browser_command command;
            if (r[1] != sizeof(command)) {
                r[0] = (user_reg)USER_EINVAL;
                break;
            }
            if (!vm_accessible(&p->space, r[0], sizeof(command), VM_WRITE)) {
                r[0] = (user_reg)USER_EFAULT;
                break;
            }
            if (!page_host_browser_command(p->pid - 1, &command)) {
                r[0] = 0;
                break;
            }
            r[0] = vm_copy_to_user(&p->space, r[0], &command, sizeof(command))
                       ? 1
                       : (user_reg)USER_EFAULT;
            break;
        }
        case SYS_BROWSER_STATE: {
            struct user_browser_state state;
            if (r[1] != sizeof(state)) {
                r[0] = (user_reg)USER_EINVAL;
                break;
            }
            if (!vm_copy_from_user(&p->space, &state, r[0], sizeof(state))) {
                r[0] = (user_reg)USER_EFAULT;
                break;
            }
            r[0] = (user_reg)page_host_browser_state(p->pid - 1, &state);
            break;
        }
        case SYS_NET_INFO:
        case SYS_NET_CONNECT:
        case SYS_NET_STATUS:
        case SYS_NET_SEND:
        case SYS_NET_RECV:
        case SYS_NET_CLOSE:
            r[0] = (user_reg)network_call(p->pid, &p->space, (uint32_t)r[SYSCALL_NUMBER], r[0],
                                          r[1], r[2]);
            break;
        case SYS_NEXT_EVENT: {
            struct user_input_event event;
            if (r[1] != sizeof(event)) {
                r[0] = (user_reg)USER_EINVAL;
                break;
            }
            if (!vm_accessible(&p->space, r[0], sizeof(event), VM_WRITE)) {
                r[0] = (user_reg)USER_EFAULT;
                break;
            }
            if (!page_host_next(p->pid - 1, &event)) {
                r[0] = 0;
                break;
            }
            r[0] =
                vm_copy_to_user(&p->space, r[0], &event, sizeof(event)) ? 1 : (user_reg)USER_EFAULT;
            break;
        }
        case SYS_FILE_SEEK:
            r[0] = (user_reg)fs_seek(&p->files, (uint32_t)r[0], (uint32_t)r[1]);
            break;
        case SYS_GPU:
        case SYS_FILE_OPEN:
        case SYS_FILE_READ:
        case SYS_FILE_SIZE:
        case SYS_FILE_CLOSE:
        case SYS_FILE_WRITE:
        case SYS_FILE_TRUNCATE:
        case SYS_FILE_SYNC:
        case SYS_PATH_MKDIR:
        case SYS_FILE_LOCK:
        case SYS_PATH_REMOVE:
        case SYS_PATH_RENAME:
        case SYS_FILE_INFO:
        case SYS_FILE_NEXT:
        case SYS_PATH_INFO:
        case SYS_FS_INFO:
            current_thread->state = T_SERVICE;
            continue_slice = false;
            break;
        default: r[0] = (user_reg)USER_ENOSYS; break;
    }
}

static bool needs_quiescence(uint32_t call) {
    switch (call) {
        case SYS_VM_MAP:
        case SYS_VM_PROTECT:
        case SYS_VM_UNMAP:
        case SYS_VM_DISCARD:
        case SYS_VM_ALLOC_RANGE:
        case SYS_THREAD_SUSPEND:
        case SYS_THREAD_CONTEXT: return true;
        default: return false;
    }
}

static bool quiescence_pending(const struct process *p) {
    for (unsigned i = 0; i < USER_THREAD_LIMIT; ++i)
        if (p->threads[i].state == T_QUIESCE || p->threads[i].state == T_SERVICE) return true;
    return false;
}

static void handle_trap(const struct arm_trap *trap, uint64_t elapsed) {
    /* All syscalls and mutable kernel services run on CPU 0. The worker has
     * switched to the kernel root before handing this context back to us. */
    continue_slice = false;
    current->cpu_us += elapsed;
    current_thread->cpu_us += elapsed;
    current_thread->context = trap->context;
    if (trap->vector == 2) {
        /* Mapping changes and GC suspension require every peer to leave user
         * mode first. Stop dispatching this address space, then collect its
         * bounded timer slices before changing translations or taking stacks. */
        if (current->running_mask && needs_quiescence((uint32_t)trap->context.r[SYSCALL_NUMBER])) {
            current_thread->state = T_QUIESCE;
            return;
        }
        continue_slice = trap->context.r[SYSCALL_NUMBER] != SYS_YIELD;
        syscall(current);
        if (current->space.mapped_pages > current->peak_pages)
            current->peak_pages = current->space.mapped_pages;
    } else if (trap->vector != 6) { /* 6: the timer ended the slice */
        current->state = FAULTED;
    }
}

static void stop(struct process *p) {
    page_host_closed(p->pid - 1);
    network_close_process(p->pid);
    p->reap_pending = true;
}

static void finish_quiescent(struct process *p) {
    if (p->running_mask || p == io_owner || p->state != RUNNABLE) return;
    for (unsigned i = 0; i < USER_THREAD_LIMIT; ++i) {
        struct thread *t = &p->threads[i];
        if (t->state != T_QUIESCE) continue;
        t->state = T_READY;
        current = p;
        current_thread = t;
        syscall(p);
        if (p->space.mapped_pages > p->peak_pages) p->peak_pages = p->space.mapped_pages;
        current = 0;
        current_thread = 0;
    }
}

static void service_work(void *argument) {
    struct process *p = argument;
    if (io_thread) {
        user_reg *r = io_thread->context.r;
        if (r[SYSCALL_NUMBER] == SYS_RANDOM) {
            unsigned char bytes[256];
            size_t n = entropy_read(bytes, (uint32_t)r[1]);
            if (!n) {
                random_ready = entropy_available();
                page_host_entropy_status(random_ready, entropy_error());
            }
            r[0] = n ? (vm_copy_to_user(&p->space, r[0], bytes, n) ? (user_reg)n
                                                                   : (user_reg)USER_EFAULT)
                     : (user_reg)(random_ready ? USER_EAGAIN : USER_ENODEV);
            volatile unsigned char *wipe = bytes;
            for (unsigned i = 0; i < sizeof(bytes); ++i) wipe[i] = 0;
        } else if (r[SYSCALL_NUMBER] == SYS_GPU)
            r[0] = (user_reg)gpu_client_call(&p->gpu, &p->space, p->pid - 1, (uint32_t)r[0], r[1],
                                             (uint32_t)r[2]);
        else if (r[SYSCALL_NUMBER] == SYS_UPDATE)
            r[0] = (user_reg)system_update_call(p->pid, &p->space, (uint32_t)r[0], r[1],
                                                (uint32_t)r[2]);
        else
            r[0] = (user_reg)fs_call(&p->files, &p->space, (uint32_t)r[SYSCALL_NUMBER], r[0], r[1],
                                     r[2], r[3]);
        io_thread->state = T_READY;
        return;
    }
    /* Close may yield on a durable flush. Keep the address space and handles
     * alive until the outstanding operation and cleanup have both finished. */
    gpu_client_close(&p->gpu);
    fs_space_close(&p->files);
    vm_destroy(&p->space);
    system_update_release(p->pid);
    p->reap_pending = false;
    if (p->restart) {
        if (!spawn(p, p->pid, p->app)) {
            /* A Close during the load already closed the slot: no error then.
             * (spawn starts with restart clear; a reopen sets it again.) */
            bool closed = p->state != RUNNABLE && !p->restart;
            p->state = FAULTED;
            p->restart = false;
            if (!closed) page_host_start_failed(p->pid - 1);
        } else if (p->state == RUNNABLE) {
            return;
        } else {
            /* Close during loading leaves a partial process for a later turn. */
            p->reap_pending = true;
            return;
        }
    }
    remove_process(p);
    if (!processes && pages_available() + gpu_client_quarantined_pages() != baseline_pages)
        panic("process page leak");
}

static bool clock_turn;

static void background_work(void *unused) {
    (void)unused;
    if (battery_pending()) {
        battery_service(0);
        return;
    }
    if (system_clock_pending() && (clock_turn || !network_service_pending())) {
        clock_turn = false;
        system_clock_service(0);
    } else {
        clock_turn = true;
        network_service_work(0);
    }
}

static void service_step(void) {
    if (!kernel_io_busy()) {
        io_owner = 0;
        io_thread = 0;
        bool background_pending =
            battery_pending() || system_clock_pending() || network_service_pending();
        /* Choose the background turn before advancing either process cursor.
         * Selecting then discarding a process skips the same busy page forever
         * when both page queues and the background queue stay ready. */
        if (background_pending && background_turn) {
            background_turn = false;
            kernel_io_start(background_work, 0);
        } else {
            unsigned count = 0;
            for (struct process *p = processes; p; p = p->next) ++count;
            for (unsigned checked = 0; checked < count; ++checked) {
                struct process *p = next_io ? next_io : processes;
                next_io = p->next ? p->next : processes;
                if (p->running_mask) continue;
                if (p->reap_pending) {
                    io_owner = p;
                    break;
                }
                if (p->state != RUNNABLE) continue;
                for (unsigned i = 0; i < USER_THREAD_LIMIT; ++i) {
                    unsigned index = (p->next_service_thread + i) % USER_THREAD_LIMIT;
                    if (p->threads[index].state == T_SERVICE) {
                        io_thread = &p->threads[index];
                        io_owner = p;
                        p->next_service_thread = (index + 1) % USER_THREAD_LIMIT;
                        break;
                    }
                }
                if (io_owner) break;
            }
            if (io_owner) {
                background_turn = true;
                kernel_io_start(service_work, io_owner);
            } else if (background_pending) {
                background_turn = false;
                kernel_io_start(background_work, 0);
            }
        }
    }
    kernel_io_step();
    if (!kernel_io_busy()) {
        io_owner = 0;
        io_thread = 0;
    }
}

static int32_t open_slot(const struct app *app) {
    if (current) return USER_EBUSY;
    unsigned pid = 1;
    struct process **link = &processes;
    while (*link && (*link)->pid == pid) {
        ++pid;
        link = &(*link)->next;
    }
    if (pid > INT32_MAX - 1000) return USER_ENOMEM;
    /* CPU-only: dispatch scans these records on every pass, so keep them cached. */
    struct process *p = vm_cpu_alloc(PROCESS_PAGES);
    if (!p) return USER_ENOMEM;
    if (!page_host_opened(pid - 1, app)) {
        vm_cpu_free(p, PROCESS_PAGES);
        return USER_ENOMEM;
    }
    *p = (struct process){.next = *link,
                          .pid = pid,
                          .state = EXITED,
                          .restart = true,
                          .reap_pending = true,
                          .app = app};
    *link = p;
    return (int32_t)pid - 1;
}

int32_t process_web_open(void) {
    if (current) return USER_EBUSY;
    if (!random_ready || !runtime_file_find("/bin/sos2-web")) return USER_ENODEV;
    return open_slot(0);
}

int32_t process_app_open(const struct app *app) { return app ? open_slot(app) : USER_ENODEV; }

bool process_app_running(const struct app *app) {
    for (struct process *p = processes; p; p = p->next)
        if (p->app == app) return true;
    return false;
}

int32_t process_web_control(unsigned slot, bool restart) {
    if (current) return USER_EBUSY;
    struct process *p = process_for(slot);
    if (!p) return USER_ENODEV;
    if (restart && !p->app && (!random_ready || !runtime_file_find("/bin/sos2-web")))
        return USER_ENODEV;
    stop(p);
    p->state = EXITED;
    p->restart = restart;
    if (restart && !page_host_opened(slot, p->app)) {
        p->restart = false;
        return USER_ENOMEM;
    }
    return 0;
}

static void collect(unsigned cpu) {
    if (arm_cpu_failed(cpu)) panic("secondary CPU kernel exception");
    const struct arm_cpu *result = arm_cpu_result(cpu);
    if (!result) return;
    struct process *p = dispatched[cpu].process;
    struct thread *t = dispatched[cpu].thread;
    if (!p || !t || t->running_cpu != cpu + 1 || !(p->running_mask & (1u << cpu)))
        panic("CPU completion ownership corrupt");
    p->running_mask &= ~(1u << cpu);
    t->running_cpu = 0;
    dispatched[cpu].process = 0;
    dispatched[cpu].thread = 0;
    /* Close cancels the in-flight syscall as well as its user continuation. */
    if (p->state == RUNNABLE) {
        t->runtime = result->runtime;
        current = p;
        current_thread = t;
        handle_trap(&result->trap, result->elapsed);
        current = 0;
        current_thread = 0;
        t->resume = continue_slice && p->state == RUNNABLE && t->state == T_READY && !t->suspended;
        if (p->state != RUNNABLE) stop(p);
    }
    arm_cpu_release(cpu);
}

static void dispatch(unsigned cpu) {
    if (frozen || !arm_cpu_idle(cpu)) return;
    unsigned count = 0;
    for (struct process *p = processes; p; p = p->next) ++count;
    for (unsigned checked = 0; checked < count; ++checked) {
        struct process *p = next_process ? next_process : processes;
        next_process = p->next ? p->next : processes;
        /* CPU 0 still owns all kernel services. Independent user threads can
         * run together until a mapping, suspension or I/O operation needs the
         * whole address space quiescent. Never dispatch one thread twice. */
        if (p == io_owner || p->state != RUNNABLE || quiescence_pending(p)) continue;
        struct thread *t = 0;
        uint64_t now = time_us();
        for (unsigned pass = 0; pass < 2 && !t; ++pass) {
            for (unsigned attempt = 0; attempt < USER_THREAD_LIMIT; ++attempt) {
                unsigned index = (p->next_thread + attempt) % USER_THREAD_LIMIT;
                struct thread *candidate = &p->threads[index];
                if (candidate->running_cpu) continue;
                if (candidate->state == T_SLEEP && time_us() >= candidate->deadline)
                    candidate->state = T_READY;
                if (candidate->state == T_WAIT) {
                    if (!vm_resolve(&p->space, candidate->wait_address, VM_READ)) {
                        candidate->context.r[0] = (user_reg)USER_EFAULT;
                        candidate->state = T_READY;
                    } else if (candidate->deadline != UINT64_MAX &&
                               time_us() >= candidate->deadline) {
                        candidate->context.r[0] = (user_reg)USER_ETIMEDOUT;
                        candidate->state = T_READY;
                    }
                }
                if (candidate->state == T_READY && !candidate->suspended &&
                    (pass || (candidate->resume && now < candidate->slice_deadline))) {
                    t = candidate;
                    p->next_thread = (index + 1) % USER_THREAD_LIMIT;
                    break;
                }
            }
        }
        if (!t) continue;
        if (!t->resume || now >= t->slice_deadline) t->slice_deadline = now + 5000;
        t->resume = false;
        t->running_cpu = cpu + 1;
        p->running_mask |= 1u << cpu;
        dispatched[cpu].process = p;
        dispatched[cpu].thread = t;
        now = time_us();
        unsigned budget = now < t->slice_deadline ? (unsigned)(t->slice_deadline - now) : 1;
        arm_cpu_submit(cpu, &p->space, &t->context, &t->runtime, budget);
        return;
    }
}

/* Standby: no user thread starts a slice; running slices end within 5 ms.
 * Kernel services, including I/O already submitted, keep running. */
void process_freeze(bool freeze) { frozen = freeze; }

void process_step(void) {
    for (unsigned cpu = 0; cpu < ARM_CPU_MAX; ++cpu) collect(cpu);
    for (struct process *p = processes; p; p = p->next) finish_quiescent(p);
    network_poll();
    service_step();
    if (random_ready != entropy_available()) {
        random_ready = entropy_available();
        page_host_entropy_status(random_ready, entropy_error());
    }
    if (arm_smp_online() == 1) {
        dispatch(0);
        collect(0);
        return;
    }
    unsigned first = next_cpu;
    next_cpu = next_cpu % (ARM_CPU_MAX - 1) + 1;
    for (unsigned i = 0; i < ARM_CPU_MAX - 1; ++i)
        dispatch(1 + (first - 1 + i) % (ARM_CPU_MAX - 1));
}
