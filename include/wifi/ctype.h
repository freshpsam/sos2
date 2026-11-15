#ifndef SOS2_WIFI_CTYPE_H
#define SOS2_WIFI_CTYPE_H

/* Locale-independent ASCII rules used by hostap's wire-format helpers. */
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }

static inline int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }

static inline int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline int isprint(int c) { return c >= 32 && c < 127; }

static inline int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static inline int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

static inline int toupper(int c) { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; }
#endif
