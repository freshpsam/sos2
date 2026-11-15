#include <stddef.h>
#include <stdbool.h>
#include <limits.h>

/* Small freestanding helpers required by the unmodified protocol stack. */
int strncmp(const char *a, const char *b, size_t n) {
    while (n) {
        --n;
        if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
        ++a;
        ++b;
    }
    return 0;
}

char *strstr(const char *text, const char *needle) {
    if (!*needle) return (char *)text;
    for (; *text; ++text) {
        const char *a = text, *b = needle;
        while (*b && *a == *b) {
            ++a;
            ++b;
        }
        if (!*b) return (char *)text;
    }
    return 0;
}

int atoi(const char *text) {
    unsigned value = 0;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n' || *text == '\v' ||
           *text == '\f')
        ++text;
    bool negative = false;
    if (*text == '-' || *text == '+') {
        negative = *text == '-';
        ++text;
    }
    unsigned limit = negative ? (unsigned)INT_MAX + 1u : (unsigned)INT_MAX;
    while (*text >= '0' && *text <= '9') {
        unsigned digit = (unsigned)(*text++ - '0');
        if (value > (limit - digit) / 10) return negative ? INT_MIN : INT_MAX;
        value = value * 10 + digit;
    }
    if (negative && value == (unsigned)INT_MAX + 1u) return INT_MIN;
    return negative ? -(int)value : (int)value;
}
