#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

uint32_t htonl(uint32_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

uint16_t htons(uint16_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(value);
#else
    return value;
#endif
}

uint32_t ntohl(uint32_t value) { return htonl(value); }

uint16_t ntohs(uint16_t value) { return htons(value); }

static int ipv4(const char *text, unsigned char result[4]) {
    for (unsigned i = 0; i < 4; ++i) {
        const char *start = text;
        unsigned value = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (*text++ - '0');
            if (value > 255 || text - start > 3) return 0;
        }
        if (text == start || (text - start > 1 && *start == '0')) return 0;
        result[i] = value;
        if (i == 3) return !*text;
        if (*text++ != '.') return 0;
    }
    return 0;
}

static int ipv6(const char *text, unsigned char result[16]) {
    uint16_t groups[8] = {0};
    unsigned count = 0;
    int gap = -1;
    if (*text == ':') {
        if (text[1] != ':') return 0;
        gap = 0;
        text += 2;
    }
    while (*text) {
        if (count == 8) return 0;
        const char *end = text;
        int dotted = 0;
        while (*end && *end != ':') {
            if (*end == '.') dotted = 1;
            ++end;
        }
        if (dotted) {
            unsigned char tail[4];
            if (count > 6 || !ipv4(text, tail)) return 0;
            groups[count++] = (uint16_t)((tail[0] << 8) | tail[1]);
            groups[count++] = (uint16_t)((tail[2] << 8) | tail[3]);
            break;
        }
        if (end == text || end - text > 4) return 0;
        unsigned value = 0;
        while (text != end) {
            unsigned c = (unsigned char)*text++, digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return 0;
            value = (value << 4) | digit;
        }
        groups[count++] = (uint16_t)value;
        if (!*text) break;
        ++text;
        if (*text == ':') {
            if (gap >= 0) return 0;
            gap = (int)count;
            ++text;
        } else if (!*text) return 0;
    }
    if (gap >= 0) {
        if (count == 8) return 0;
        unsigned tail = count - (unsigned)gap;
        memmove(groups + 8 - tail, groups + gap, tail * sizeof(*groups));
        memset(groups + gap, 0, (8 - count) * sizeof(*groups));
    } else if (count != 8) return 0;
    for (unsigned i = 0; i < 8; ++i) {
        result[2 * i] = groups[i] >> 8;
        result[2 * i + 1] = groups[i];
    }
    return 1;
}

int inet_pton(int family, const char *text, void *destination) {
    unsigned char result[16];
    int valid;
    if (family == AF_INET) valid = ipv4(text, result);
    else if (family == AF_INET6) valid = ipv6(text, result);
    else {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (valid) memcpy(destination, result, family == AF_INET ? 4 : 16);
    return valid;
}
