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
#include "audio.h"
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

enum thread_state { T_UNUSED, T_READY, T_SLEEP, T_WAIT, T_SERVICE, T_MEMORY, T_QUIESCE, T_DONE };

/* Mappings and unmappings of MEMORY_JOB_PAGES pages (1 MiB) or more take
 * MEMORY_SLICE_PAGES a turn of the kernel's loop, about 0.1 ms on the Nexus
 * 5, while their thread waits (T_MEMORY): mapping 60 MiB in one call held
 * every other syscall and the desktop for 45 ms there. */
#define MEMORY_JOB_PAGES   256
#define MEMORY_SLICE_PAGES 32

/* A thread's big mapping or unmapping (include/vm.h). */
struct memory_job {
    user_ptr address;
    unsigned count, done;
    unsigned char permissions;
    bool unmap;
    bool finish; /* The unmap's last step waits in T_QUIESCE. */
};

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
    uint64_t wait_epoch; /* vm_epoch() when wait_address was last found mapped */
    uint32_t suspended;
    unsigned running_cpu;
    bool resume;
    uint64_t deadline, cpu_us, slice_deadline;
    struct memory_job memory;
};

/* What first-fit searches in the heap or the JIT window found: no free run of
 * count pages or more starts at a multiple of alignment pages between the
 * range's start and address. Mapping keeps that true and unmapping lowers it
 * (lower_hints), so a search for count pages starts at address instead of
 * walking every mapped page's table entries (uncached) from the start, as
 * JavaScriptCore's per-block mappings would otherwise make every search do. */
struct range_hint {
    user_ptr address;
    unsigned count; /* 0: unused */
    unsigned alignment;
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
    /* process_web_park: once reaped, the record keeps the page's slot, and the
     * desktop its entry, until process_web_resume starts it again. */
    bool parked;
    const struct app *app; /* Its program and storage; NULL: a web page */
    unsigned running_mask;
    unsigned memory_jobs; /* Threads with a memory_job under way */
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
static uint64_t user_us;
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
    audio_init(devices);
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
    if (!arch_user_entry_valid(start.entry) || !vm_resolve(&p->space, start.entry, VM_EXEC) ||
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
 * to count - 1 pages below it, at a multiple of its alignment (runs below a
 * hint are shorter than count). */
static void lower_hints(struct process *p, user_ptr address) {
    unsigned range = hint_range(address);
    user_ptr base = range ? USER_JIT_BASE : USER_HEAP_BASE;
    for (unsigned i = 0; i < RANGE_HINTS; ++i) {
        struct range_hint *hint = &p->hints[range][i];
        if (!hint->count) continue;
        user_ptr reach = (user_ptr)(hint->count - 1) * PAGE_SIZE;
        user_ptr floor = address - base > reach ? address - reach : base;
        floor &= ~((user_ptr)hint->alignment * PAGE_SIZE - 1);
        if (floor < base) floor = base;
        if (hint->address > floor) hint->address = floor;
    }
}

/* First-fit placement of count pages at a multiple of alignment pages in
 * [lower, upper), within the heap or the JIT window, resumed where earlier
 * searches for such runs ended; reserved only, for a memory job, if asked. */
static user_ptr allocate_range(struct process *p, unsigned count, unsigned alignment,
                               user_ptr lower, user_ptr upper, unsigned flags, bool reserve) {
    unsigned range = hint_range(lower);
    struct range_hint *hint = 0;
    for (unsigned i = 0; i < RANGE_HINTS; ++i)
        if (p->hints[range][i].count == count && p->hints[range][i].alignment == alignment)
            hint = &p->hints[range][i];
    user_ptr known = hint ? hint->address : range ? USER_JIT_BASE : USER_HEAP_BASE;
    user_ptr from = known > lower ? known : lower;
    if (from >= upper) return 0;
    user_ptr address = reserve
                           ? vm_reserve_range(&p->space, count, alignment, from, upper, flags)
                           : vm_allocate_range(&p->space, count, alignment, from, upper, flags);
    /* Only a search that covered the range from its start teaches anything;
     * a failure may be the page pool running out, not a lack of room. */
    if (address && from == known) {
        if (!hint) {
            hint = &p->hints[range][p->next_hint[range]];
            p->next_hint[range] = (p->next_hint[range] + 1) % RANGE_HINTS;
            hint->count = count;
            hint->alignment = alignment;
        }
        hint->address = address + (user_ptr)count * PAGE_SIZE;
    }
    return address;
}

/* Whether a mapping of count pages runs as a job. */
static bool deferred_mapping(unsigned count) { return count >= MEMORY_JOB_PAGES; }

/* A job's range starts on a megabyte if one is free, where sections fit. */
static user_ptr place(struct process *p, unsigned count, unsigned alignment, user_ptr lower,
                      user_ptr upper, unsigned flags, bool reserve) {
    user_ptr address = 0;
    if (reserve && alignment < MEMORY_JOB_PAGES)
        address = allocate_range(p, count, MEMORY_JOB_PAGES, lower, upper, flags, true);
    return address ? address : allocate_range(p, count, alignment, lower, upper, flags, reserve);
}

/* The calling thread waits while memory_step runs the job. */
static void memory_defer(struct process *p, user_ptr address, unsigned count, unsigned flags,
                         bool unmap) {
    current_thread->memory = (struct memory_job){
        .address = address, .count = count, .permissions = (unsigned char)flags, .unmap = unmap};
    current_thread->state = T_MEMORY;
    continue_slice = false;
    ++p->memory_jobs;
}

static user_result memory_call(struct process *p, uint32_t number, user_ptr address, unsigned count,
                               unsigned flags) {
    bool jit = jit_window(p, address, address + (user_ptr)count * PAGE_SIZE);
    if (!count || count > (USER_HEAP_END - USER_HEAP_BASE) / PAGE_SIZE || (flags & ~7u) ||
        ((flags & 6) == 6 && !jit) || (flags && !(flags & VM_READ)))
        return USER_EINVAL;
    if (number == SYS_VM_MAP && !address) {
        bool reserve = deferred_mapping(count);
        address = place(p, count, 1, USER_HEAP_BASE, USER_HEAP_END, flags, reserve);
        if (!address) return USER_ENOMEM;
        if (reserve) memory_defer(p, address, count, flags, false);
        return (user_result)address;
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
        user_ptr at = address + i * PAGE_SIZE;
        if (number == SYS_VM_MAP ? vm_occupied(&p->space, at) : !vm_mapped(&p->space, at))
            return USER_EINVAL;
    }
    if (number == SYS_VM_MAP && deferred_mapping(count)) {
        if (!vm_reserve(&p->space, address, count, flags)) return USER_ENOMEM;
        memory_defer(p, address, count, flags, false);
        return (user_result)address;
    }
    if (number == SYS_VM_MAP) {
        unsigned i = 0;
        while (i < count && vm_map(&p->space, address + i * PAGE_SIZE, flags)) ++i;
        if (i == count) return (user_result)address;
        if (i && !vm_release(&p->space, address, i)) panic("mapping rollback failed");
        return USER_ENOMEM;
    }
    if (number == SYS_VM_UNMAP && deferred_mapping(count)) {
        /* Quiescent here, as every unmap (needs_quiescence). */
        if (!vm_unmap_begin(&p->space, address, count)) return USER_EINVAL;
        memory_defer(p, address, count, 0, true);
        return 0;
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
                t->wait_epoch = vm_epoch();
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
            /* A seeded generator answers at once; seeding, and virtio's
             * reads, wait in the service worker. */
            unsigned char bytes[256];
            if (entropy_read_now(bytes, size) == size)
                r[0] = vm_copy_to_user(&p->space, r[0], bytes, size) ? (user_reg)size
                                                                     : (user_reg)USER_EFAULT;
            else {
                current_thread->state = T_SERVICE;
                continue_slice = false;
            }
            volatile unsigned char *wipe = bytes;
            for (unsigned i = 0; i < sizeof(bytes); ++i) wipe[i] = 0;
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
            unsigned count = (uint32_t)r[2], flags = (uint32_t)r[3] & 7u;
            unsigned shift = (uint32_t)r[3] >> USER_MAP_ALIGN_SHIFT;
            bool jit = jit_window(p, lower, upper);
            if (((lower < USER_HEAP_BASE || upper > USER_HEAP_END) && !jit) || lower >= upper ||
                ((lower | upper) & (PAGE_SIZE - 1)) || !count ||
                count > (upper - lower) / PAGE_SIZE || shift > USER_MAP_ALIGN_MAX ||
                (((uint32_t)r[3] >> 3) & ((1u << (USER_MAP_ALIGN_SHIFT - 3)) - 1)) ||
                ((flags & 6) == 6 && !jit) || (flags && !(flags & VM_READ))) {
                r[0] = (user_reg)USER_EINVAL;
                break;
            }
            bool reserve = deferred_mapping(count);
            user_ptr address = place(p, count, 1u << shift, lower, upper, flags, reserve);
            r[0] = address ? address : (user_reg)USER_ENOMEM;
            if (address && reserve) memory_defer(p, address, count, flags, false);
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
        case SYS_AUDIO:
            r[0] = (user_reg)audio_call(p->pid, &p->space, (uint32_t)r[0], r[1], (uint32_t)r[2]);
            break;
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

/* Whether a call must wait for the process's other threads to leave user mode.
 * New pages that are not executable need not: entries go from invalid to
 * valid, which no CPU caches, and with the quota and the page pool holding
 * every page and table they could take, no failure halfway frees one again.
 * Allocating threads then no longer stop GC markers and compiler threads. */
static bool needs_quiescence(const struct process *p, const user_reg *r) {
    unsigned count, flags;
    switch ((uint32_t)r[SYSCALL_NUMBER]) {
        case SYS_VM_MAP:
            count = (uint32_t)r[1];
            flags = (uint32_t)r[2];
            break;
        case SYS_VM_ALLOC_RANGE:
            count = (uint32_t)r[2];
            flags = (uint32_t)r[3];
            break;
        case SYS_VM_PROTECT:
        case SYS_VM_UNMAP:
        case SYS_VM_DISCARD:
        case SYS_THREAD_SUSPEND:
        case SYS_THREAD_CONTEXT: return true;
        default: return false;
    }
    return !count || (flags & VM_EXEC) || count > USER_PAGE_QUOTA - p->space.mapped_pages ||
           pages_available() < count + count / 128 + 8;
}

static bool quiescence_pending(const struct process *p) {
    for (unsigned i = 0; i < USER_THREAD_LIMIT; ++i)
        if (p->threads[i].state == T_QUIESCE) return true;
    return false;
}

static void handle_trap(const struct arm_trap *trap, uint64_t elapsed) {
    /* All syscalls and mutable kernel services run on CPU 0. The worker has
     * switched to the kernel root before handing this context back to us. */
    continue_slice = false;
    user_us += elapsed;
    current->cpu_us += elapsed;
    current_thread->cpu_us += elapsed;
    current_thread->context = trap->context;
    if (trap->vector == 2) {
        /* Mapping changes and GC suspension require every peer to leave user
         * mode first. Stop dispatching this address space, then collect its
         * bounded timer slices before changing translations or taking stacks. */
        if (current->running_mask && needs_quiescence(current, trap->context.r)) {
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
    audio_close_process(p->pid);
    p->reap_pending = true;
}

/* A service request of p's may be waiting meanwhile: it touches p's memory
 * only between the worker's yields, through vm_copy_* walking the tables
 * then, so a mapping changed here at worst fails its copy (USER_EFAULT).
 * A memory job's unmap ends here too. */
static void finish_quiescent(struct process *p) {
    if (p->running_mask || p->state != RUNNABLE) return;
    for (unsigned i = 0; i < USER_THREAD_LIMIT; ++i) {
        struct thread *t = &p->threads[i];
        if (t->state != T_QUIESCE) continue;
        t->state = T_READY;
        if (t->memory.finish) {
            vm_unmap_end(&p->space, t->memory.address, t->memory.count);
            lower_hints(p, t->memory.address);
            t->memory.finish = false;
            t->context.r[0] = 0;
            --p->memory_jobs;
            continue;
        }
        current = p;
        current_thread = t;
        syscall(p);
        if (p->space.mapped_pages > p->peak_pages) p->peak_pages = p->space.mapped_pages;
        current = 0;
        current_thread = 0;
    }
}

/* One slice of a job. An unmap's last step frees the tables it emptied,
 * which needs the process quiescent (vm_unmap_end). */
static void memory_slice(struct process *p, struct thread *t) {
    struct memory_job *job = &t->memory;
    unsigned end = job->count;
    if (end - job->done > MEMORY_SLICE_PAGES) end = job->done + MEMORY_SLICE_PAGES;
    for (; job->done < end; ++job->done) {
        user_ptr at = job->address + (user_ptr)job->done * PAGE_SIZE;
        if (job->unmap) vm_unmap_page(&p->space, at);
        else vm_fill_reserved(&p->space, at, job->permissions);
    }
    if (job->done < job->count) return;
    if (job->unmap) {
        job->finish = true;
        t->state = T_QUIESCE;
        return;
    }
    t->context.r[0] = (user_reg)job->address;
    t->state = T_READY;
    --p->memory_jobs;
}

/* A slice of each job a turn. A process that stopped keeps its job's
 * half-done entries for vm_destroy. */
static void memory_step(void) {
    for (struct process *p = processes; p; p = p->next) {
        if (!p->memory_jobs || p->state != RUNNABLE) continue;
        for (unsigned i = 0; i < USER_THREAD_LIMIT; ++i)
            if (p->threads[i].state == T_MEMORY) memory_slice(p, &p->threads[i]);
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
            /* A Close or a park during the load already took the slot: no
             * error then. spawn starts with restart clear; a reopen or a
             * resume during the load sets it again, and the page starts
             * afresh on the next turn. */
            if (p->state != RUNNABLE && p->restart) {
                p->reap_pending = true;
                return;
            }
            bool closed = p->state != RUNNABLE;
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
    if (p->parked) return;
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
                /* A request runs beside the process's other threads, which
                 * never wait for it (finish_quiescent). Teardown waits for
                 * every thread to leave its CPU. */
                if (p->reap_pending) {
                    if (p->running_mask) continue;
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
    p->parked = false;
    p->restart = restart;
    if (restart && !page_host_opened(slot, p->app)) {
        p->restart = false;
        return USER_ENOMEM;
    }
    return 0;
}

int32_t process_web_park(unsigned slot) {
    if (current) return USER_EBUSY;
    struct process *p = process_for(slot);
    if (!p || p->app || p->parked || p->state != RUNNABLE) return USER_ENODEV;
    network_close_process(p->pid);
    audio_close_process(p->pid);
    p->state = EXITED;
    p->restart = false;
    p->parked = p->reap_pending = true;
    return 0;
}

int32_t process_web_resume(unsigned slot) {
    if (current) return USER_EBUSY;
    struct process *p = process_for(slot);
    if (!p || !p->parked) return USER_ENODEV;
    if (!random_ready || !runtime_file_find("/bin/sos2-web")) return USER_ENODEV;
    if (!page_host_opened(slot, 0)) return USER_ENOMEM;
    /* The reaper, its releases already done, starts the page (spawn clears
     * parked), even if it is still reclaiming the parked process. */
    p->parked = false;
    p->restart = p->reap_pending = true;
    return 0;
}

bool process_playing(unsigned slot) {
    struct process *p = process_for(slot);
    return p && audio_process_playing(p->pid);
}

bool process_reclaiming(void) {
    /* vm_destroy clears the space once it has freed every page. */
    for (struct process *p = processes; p; p = p->next)
        if (p->reap_pending && p->space.mapped_pages) return true;
    return false;
}

static bool collect(unsigned cpu) {
    const struct arm_cpu *result = arm_cpu_result(cpu);
    if (!result) {
        if (arm_cpu_failed(cpu)) panic("secondary CPU kernel exception");
        return false;
    }
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
    return true;
}

/* Sleeps and waits that have ended make their threads ready: once per step,
 * not once per idle CPU and pass. A waiting thread's word was mapped when it
 * began to wait; only a mapping change since (vm_epoch) can have removed it,
 * so the table walk that checks it happens only then. */
static void wake_expired(struct process *p, uint64_t now, uint64_t epoch) {
    for (unsigned i = 0; i < USER_THREAD_LIMIT; ++i) {
        struct thread *t = &p->threads[i];
        if (t->running_cpu) continue;
        if (t->state == T_SLEEP && now >= t->deadline) t->state = T_READY;
        if (t->state != T_WAIT) continue;
        if (t->wait_epoch != epoch) {
            t->wait_epoch = epoch;
            if (!vm_resolve(&p->space, t->wait_address, VM_READ)) {
                t->context.r[0] = (user_reg)USER_EFAULT;
                t->state = T_READY;
                continue;
            }
        }
        if (t->deadline != UINT64_MAX && now >= t->deadline) {
            t->context.r[0] = (user_reg)USER_ETIMEDOUT;
            t->state = T_READY;
        }
    }
}

/* The thread of p to run next: one still within the slice a syscall
 * interrupted first, then any ready one, from where the last one was found. */
static struct thread *next_ready(struct process *p, uint64_t now) {
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned attempt = 0; attempt < USER_THREAD_LIMIT; ++attempt) {
            unsigned index = (p->next_thread + attempt) % USER_THREAD_LIMIT;
            struct thread *t = &p->threads[index];
            if (!t->running_cpu && t->state == T_READY && !t->suspended &&
                (pass || (t->resume && now < t->slice_deadline))) {
                p->next_thread = (index + 1) % USER_THREAD_LIMIT;
                return t;
            }
        }
    return 0;
}

static void dispatch(unsigned cpu, uint64_t now) {
    if (!arm_cpu_idle(cpu)) return;
    unsigned count = 0;
    for (struct process *p = processes; p; p = p->next) ++count;
    for (unsigned checked = 0; checked < count; ++checked) {
        struct process *p = next_process ? next_process : processes;
        next_process = p->next ? p->next : processes;
        /* CPU 0 still owns all kernel services. Independent user threads can
         * run together, a service request included, until a mapping change or
         * a suspension needs the whole address space quiescent. Never
         * dispatch one thread twice. */
        if (p->state != RUNNABLE || quiescence_pending(p)) continue;
        struct thread *t = next_ready(p, now);
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

/* Gives idle CPUs to ready threads: CPU 0 alone while no worker is online
 * (first 0), else the workers in turn from first. */
static void dispatch_idle(unsigned first) {
    if (frozen) return;
    unsigned cpus = first ? ARM_CPU_MAX - 1 : 1, order[ARM_CPU_MAX];
    bool idle = false;
    for (unsigned i = 0; i < cpus; ++i) {
        order[i] = first ? 1 + (first - 1 + i) % (ARM_CPU_MAX - 1) : 0;
        idle = idle || arm_cpu_idle(order[i]);
    }
    if (!idle) return;
    uint64_t now = time_us(), epoch = vm_epoch();
    for (struct process *p = processes; p; p = p->next)
        if (p->state == RUNNABLE) wake_expired(p, now, epoch);
    for (unsigned i = 0; i < cpus; ++i) dispatch(order[i], now);
}

uint64_t process_user_us(void) { return user_us; }

/* Standby: no user thread starts a slice; running slices end within 5 ms.
 * Kernel services, including I/O already submitted, keep running. */
void process_freeze(bool freeze) { frozen = freeze; }

/* A turn of the loop is 20-40 us and a thread waited for one at each end of
 * every system call, 5% of YouTube's main thread on the Nexus 5. */
void process_turnaround(void) {
    if (arm_smp_online() == 1) return; /* CPU 0 runs slices only in process_step */
    bool collected = false;
    for (unsigned cpu = 1; cpu < ARM_CPU_MAX; ++cpu) collected = collect(cpu) || collected;
    if (!collected) return;
    for (struct process *p = processes; p; p = p->next) finish_quiescent(p);
    dispatch_idle(next_cpu);
}

void process_step(void) {
    process_turnaround();
    for (unsigned cpu = 0; cpu < ARM_CPU_MAX; ++cpu) collect(cpu);
    for (struct process *p = processes; p; p = p->next) finish_quiescent(p);
    memory_step();
    network_poll();
    process_turnaround();
    audio_poll();
    service_step();
    process_turnaround();
    if (random_ready != entropy_available()) {
        random_ready = entropy_available();
        page_host_entropy_status(random_ready, entropy_error());
    }
    if (arm_smp_online() == 1) {
        dispatch_idle(0);
        collect(0);
        return;
    }
    unsigned first = next_cpu;
    next_cpu = next_cpu % (ARM_CPU_MAX - 1) + 1;
    dispatch_idle(first);
}
