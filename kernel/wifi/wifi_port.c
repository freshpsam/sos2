#include "wifi/port.h"
#include "utils/includes.h"
#include "utils/common.h"
#include "utils/eloop.h"
#include "platform.h"

enum { ARENA_SIZE = 512 * 1024, TIMER_COUNT = 16 };

static unsigned char arena[ARENA_SIZE] __attribute__((aligned(16)));

struct block {
    uint32_t size, used, reserved[2];
};

static struct timer {
    uint64_t when;
    eloop_timeout_handler callback;
    void *context, *user;
} timers[TIMER_COUNT];

static uint64_t current_time;
static bool initialized, failed;
static int (*random_bytes)(void *, unsigned char *, size_t);
static void *random_context;
static void (*yield_callback)(void *);
static void *yield_context;

void wifi_port_set_yield(void (*callback)(void *), void *context) {
    yield_callback = callback;
    yield_context = context;
}

void wifi_port_yield(void) {
    if (yield_callback) yield_callback(yield_context);
}

/* hostap constructs diagnostic labels even with logging disabled. Keep this
 * formatter private: it never sends text (or keys) to the shell or serial. */
int wifi_os_snprintf(char *out, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    size_t count = 0;
    while (*format) {
        char bytes[32];
        const char *text = bytes;
        size_t length = 0, width = 0;
        char pad = ' ';
        if (*format != '%') bytes[length++] = *format++;
        else {
            ++format;
            if (*format == '0') {
                pad = '0';
                ++format;
            }
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format++ - '0');
                if (width > 128) goto invalid;
            }
            bool wide = false;
            if (*format == 'z' || *format == 'l') {
                wide = true;
                ++format;
            }
            char type = *format;
            if (!type) goto invalid;
            ++format;
            if (type == 's') {
                text = va_arg(args, const char *);
                if (!text) text = "(null)";
                length = strlen(text);
            } else if (type == 'c' || type == '%')
                bytes[length++] = type == '%' ? '%' : (char)va_arg(args, int);
            else if (type == 'u' || type == 'd' || type == 'i' || type == 'x' || type == 'X') {
                unsigned long value;
                bool negative = false;
                if (type == 'd' || type == 'i') {
                    long v = wide ? va_arg(args, long) : va_arg(args, int);
                    negative = v < 0;
                    value = negative ? 0ul - (unsigned long)v : (unsigned long)v;
                } else value = wide ? va_arg(args, unsigned long) : va_arg(args, unsigned);
                unsigned base = type == 'x' || type == 'X' ? 16 : 10;
                char reverse[32];
                unsigned used = 0;
                do {
                    unsigned digit = value % base;
                    reverse[used++] =
                        (char)(digit < 10 ? '0' + digit : (type == 'X' ? 'A' : 'a') + digit - 10);
                    value /= base;
                } while (value);
                if (negative) bytes[length++] = '-';
                while (used) bytes[length++] = reverse[--used];
            } else goto invalid;
        }
        while (width > length) {
            if (count + 1 < size) out[count] = pad;
            ++count;
            --width;
        }
        for (size_t i = 0; i < length; ++i) {
            if (count + 1 < size) out[count] = text[i];
            ++count;
        }
        if (count > INT32_MAX) goto invalid;
    }
    if (size) out[count < size ? count : size - 1] = 0;
    va_end(args);
    return (int)count;
invalid:
    if (size) out[0] = 0;
    va_end(args);
    return -1;
}

void wifi_crypto_wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

void mbedtls_platform_zeroize(void *p, size_t n) { wifi_crypto_wipe(p, n); }

void wifi_port_reset(void) {
    wifi_crypto_wipe(arena, sizeof(arena));
    memset(timers, 0, sizeof(timers));
    ((struct block *)arena)->size = sizeof(arena) - sizeof(struct block);
    initialized = true;
    failed = false;
    random_bytes = 0;
    random_context = 0;
    yield_callback = 0;
    yield_context = 0;
}

void wifi_port_bind(int (*random)(void *, unsigned char *, size_t), void *context, uint64_t now) {
    if (!initialized) wifi_port_reset();
    random_bytes = random;
    random_context = context;
    current_time = now;
}

bool wifi_port_failed(void) { return failed; }

void *wifi_os_malloc(size_t n) {
    if (!initialized) wifi_port_reset();
    if (!n) n = 1;
    if (n > ARENA_SIZE - 32) return 0;
    n = (n + 15) & ~(size_t)15;
    for (size_t at = 0; at < ARENA_SIZE;) {
        struct block *b = (void *)(arena + at);
        if (b->size > ARENA_SIZE - at - sizeof(*b)) {
            failed = true;
            return 0;
        }
        if (!b->used && b->size >= n) {
            if (b->size >= n + sizeof(*b) + 16) {
                struct block *next = (void *)(arena + at + sizeof(*b) + n);
                *next = (struct block){.size = b->size - n - sizeof(*b)};
                b->size = n;
            }
            b->used = 1;
            return b + 1;
        }
        at += sizeof(*b) + b->size;
    }
    return 0;
}

void *wifi_os_zalloc(size_t n) {
    void *p = wifi_os_malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

char *wifi_os_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = wifi_os_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

void *os_memdup(const void *s, size_t n) {
    void *p = wifi_os_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

int os_memcmp_const(const void *left, const void *right, size_t n) {
    const volatile unsigned char *a = left, *b = right;
    unsigned difference = 0;
    while (n--) difference |= *a++ ^ *b++;
    return difference;
}

void *wifi_crypto_calloc(size_t count, size_t n) {
    if (n && count > SIZE_MAX / n) return 0;
    return wifi_os_zalloc(count * n);
}

static struct block *find_block(void *p) {
    for (size_t at = 0; at < ARENA_SIZE;) {
        struct block *b = (void *)(arena + at);
        if (b->size > ARENA_SIZE - at - sizeof(*b)) break;
        if ((void *)(b + 1) == p && b->used) return b;
        at += sizeof(*b) + b->size;
    }
    failed = true;
    return 0;
}

void wifi_os_free(void *p) {
    if (!p) return;
    struct block *b = find_block(p);
    if (!b) return;
    wifi_crypto_wipe(b + 1, b->size);
    b->used = 0;
    for (size_t at = 0; at < ARENA_SIZE;) {
        b = (void *)(arena + at);
        size_t next = at + sizeof(*b) + b->size;
        if (next < ARENA_SIZE && !b->used && !((struct block *)(arena + next))->used) {
            b->size += sizeof(*b) + ((struct block *)(arena + next))->size;
            continue;
        }
        at = next;
    }
}

void *wifi_os_realloc(void *p, size_t n) {
    if (!p) return wifi_os_malloc(n);
    if (!n) {
        wifi_os_free(p);
        return 0;
    }
    struct block *b = find_block(p);
    if (!b) return 0;
    void *q = wifi_os_malloc(n);
    if (!q) return 0;
    memcpy(q, p, n < b->size ? n : b->size);
    wifi_os_free(p);
    return q;
}

int wifi_crypto_random(void *unused, unsigned char *p, size_t n) {
    (void)unused;
    if (!random_bytes || random_bytes(random_context, p, n)) {
        wifi_crypto_wipe(p, n);
        failed = true;
        return -1;
    }
    return 0;
}

int os_get_random(unsigned char *p, size_t n) { return wifi_crypto_random(0, p, n); }

int os_get_time(struct os_time *t) {
    t->sec = current_time / 1000000;
    t->usec = current_time % 1000000;
    return 0;
}

int eloop_register_timeout(unsigned secs, unsigned usecs, eloop_timeout_handler cb, void *context,
                           void *user) {
    uint64_t delta = (uint64_t)secs * 1000000 + usecs;
    if (!cb || delta > UINT64_MAX - current_time) {
        failed = true;
        return -1;
    }
    for (unsigned i = 0; i < TIMER_COUNT; ++i)
        if (!timers[i].callback) {
            timers[i] = (struct timer){current_time + delta, cb, context, user};
            return 0;
        }
    failed = true;
    return -1;
}

int eloop_cancel_timeout(eloop_timeout_handler cb, void *context, void *user) {
    int count = 0;
    for (unsigned i = 0; i < TIMER_COUNT; ++i)
        if (timers[i].callback == cb &&
            (context == ELOOP_ALL_CTX || timers[i].context == context) &&
            (user == ELOOP_ALL_CTX || timers[i].user == user)) {
            timers[i] = (struct timer){0};
            ++count;
        }
    return count;
}

void wifi_port_tick(uint64_t now) {
    current_time = now;
    /* A callback can rearm itself; each slot runs at most once per poll. */
    for (unsigned i = 0; i < TIMER_COUNT; ++i)
        if (timers[i].callback && timers[i].when <= now) {
            struct timer t = timers[i];
            timers[i] = (struct timer){0};
            t.callback(t.context, t.user);
        }
}

_Noreturn void wifi_os_abort(void) { panic("wireless authentication internal invariant"); }
