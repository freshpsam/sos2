#include <stddef.h>
#include <stdint.h>
/* Byte buffers can alias any object. Use aligned scalar words, including on
 * uncached compositor surfaces; never issue an unaligned ARM word access. */
typedef uint32_t copy_word __attribute__((__may_alias__));

void *memset(void *dst, int value, size_t n) {
    unsigned char *p = dst;
    while (n && ((uintptr_t)p & 3)) {
        *p++ = (unsigned char)value;
        --n;
    }
    copy_word word = (unsigned char)value * UINT32_C(0x01010101);
    copy_word *out = (copy_word *)p;
    while (n >= 16) {
        out[0] = word;
        out[1] = word;
        out[2] = word;
        out[3] = word;
        out += 4;
        n -= 16;
    }
    while (n >= 4) {
        *out++ = word;
        n -= 4;
    }
    p = (unsigned char *)out;
    while (n--) *p++ = (unsigned char)value;
    return dst;
}

static void copy_forward(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (!(((uintptr_t)d ^ (uintptr_t)s) & 3)) {
        while (n && ((uintptr_t)d & 3)) {
            *d++ = *s++;
            --n;
        }
        copy_word *out = (copy_word *)d;
        const copy_word *in = (const copy_word *)s;
        while (n >= 16) {
            out[0] = in[0];
            out[1] = in[1];
            out[2] = in[2];
            out[3] = in[3];
            out += 4;
            in += 4;
            n -= 16;
        }
        while (n >= 4) {
            *out++ = *in++;
            n -= 4;
        }
        d = (unsigned char *)out;
        s = (const unsigned char *)in;
    }
    while (n--) *d++ = *s++;
}

void *memcpy(void *dst, const void *src, size_t n) {
    copy_forward(dst, src, n);
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (!n || d == s) return dst;
    if ((uintptr_t)d <= (uintptr_t)s) {
        copy_forward(d, s, n);
    } else {
        d += n;
        s += n;
        if (!(((uintptr_t)d ^ (uintptr_t)s) & 3)) {
            while (n && ((uintptr_t)d & 3)) {
                *--d = *--s;
                --n;
            }
            copy_word *out = (copy_word *)d;
            const copy_word *in = (const copy_word *)s;
            while (n >= 4) {
                *--out = *--in;
                n -= 4;
            }
            d = (unsigned char *)out;
            s = (const unsigned char *)in;
        }
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y) return *x - *y;
        ++x;
        ++y;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

void *memchr(const void *data, int byte, size_t size) {
    const unsigned char *p = data;
    while (size--) {
        if (*p == (unsigned char)byte) return (void *)p;
        ++p;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    do {
        if (*s == (char)c) return (char *)s;
    } while (*s++);
    return 0;
}

char *strcpy(char *out, const char *s) {
    char *start = out;
    while ((*out++ = *s++)) { }
    return start;
}

char *stpcpy(char *out, const char *s) {
    while ((*out = *s++)) ++out;
    return out;
}

char *decimal(char *out, unsigned value) {
    char reverse[10];
    unsigned n = 0;
    do {
        reverse[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) *out++ = reverse[--n];
    *out = 0;
    return out;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) ++n;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) ++n;
    return n;
}
