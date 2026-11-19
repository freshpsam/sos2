#include "fdt.h"
#include "wire.h"

/* Root-bus devices, RAM, reserved memory and a firmware-owned simplefb only.
 * No general bus address translation is attempted. The chosen/reserved-memory
 * containers must explicitly declare an identity mapping with empty ranges.
 * lk2nd's handoff does this: lk2nd/display/simplefb.c in msm8916-mainline/lk2nd.
 */
static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const unsigned char *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

#ifdef SOS2_BOARD_GTA7LITE
static void copy_string(char *out, size_t capacity, const unsigned char *p) {
    size_t i = 0;
    while (i + 1 < capacity && p[i]) {
        out[i] = (char)p[i];
        ++i;
    }
    out[i] = 0;
}
#endif

static bool equals(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool terminated(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (!p[i]) return true;
    return false;
}

static bool single_string(const unsigned char *p, size_t n) {
    return n && !p[n - 1] && !terminated(p, n - 1);
}

static bool string_list(const unsigned char *p, size_t n) {
    if (!n || p[n - 1]) return false;
    while (n) {
        size_t length = 0;
        while (length < n && p[length]) ++length;
        if (!length || length == n) return false;
        ++length;
        p += length;
        n -= length;
    }
    return true;
}

/* The value's text up to its first NUL, or all of it, equals s: how Linux
 * reads a status (strcmp in of_device_is_available). */
static bool text_is(const unsigned char *p, size_t n, const char *s) {
    size_t i = 0;
    while (i < n && p[i] && s[i] && p[i] == (unsigned char)s[i]) ++i;
    return !s[i] && (i == n || !p[i]);
}

static bool compatible(const unsigned char *p, size_t n, const char *s) {
    while (n) {
        size_t length = 0;
        while (length < n && p[length]) ++length;
        if (length == n) return false;
        if (equals((const char *)p, s)) return true;
        ++length;
        p += length;
        n -= length;
    }
    return false;
}

/* SOS2's own arguments, the sos2.* tokens of /chosen/bootargs, in order,
 * wherever the loader put the image's command line: lk2nd starts its
 * bootargs with it, the tablet's LK ends a long one with it. The kernel reads
 * nothing else there. Only whole tokens are kept; one that does not fit ends
 * the list, which can drop an argument but never change one. */
static void own_arguments(char *out, size_t capacity, const unsigned char *p) {
    size_t used = 0;
    out[0] = 0;
    for (size_t i = 0; p[i];) {
        while (p[i] == ' ' || p[i] == '\t') ++i;
        size_t start = i;
        while (p[i] && p[i] != ' ' && p[i] != '\t') ++i;
        size_t n = i - start;
        if (n < 5 || p[start] != 's' || p[start + 1] != 'o' || p[start + 2] != 's' ||
            p[start + 3] != '2' || p[start + 4] != '.')
            continue;
        if (used + (used ? 1 : 0) + n >= capacity) break;
        if (used) out[used++] = ' ';
        for (size_t k = 0; k < n; ++k) out[used++] = (char)p[start + k];
        out[used] = 0;
    }
}

static uint64_t cells(const unsigned char *p, unsigned count) {
    uint64_t v = 0;
    while (count--) {
        v = (v << 32) | be32(p);
        p += 4;
    }
    return v;
}

static bool overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a < b + bs && b < a + as;
}

static bool add_region(struct memory_region *regions, size_t *count, size_t max, uint64_t address,
                       uint64_t size) {
    if (!size || size > UINT64_MAX - address || *count == max) return false;
    regions[(*count)++] = (struct memory_region){address, size};
    return true;
}

struct node {
    unsigned ac, sc, parent_ac, parent_sc;
    const unsigned char *reg, *format;
    uint32_t reg_length, format_length, width, height, stride;
    uint32_t phandle, memory_region;
    bool fwcfg, virtio, memory, disabled, simplefb, pl061, gpio_keys;
    bool key_code_seen, key_gpio_seen;
    uint32_t key_code, key_gpio[2];
    bool chosen, reserved_container, ranges_seen, ranges_empty, children_started;
    bool width_seen, height_seen, stride_seen, bad_fb, memory_region_seen;
    bool cpus, cpu, cpu_psci, psci, psci_standard;
#ifdef SOS2_BOARD_GTA7LITE
    bool gicv3;
#endif
    enum cpu_boot_method method;
    uint32_t cpu_on;
};

struct reservation_reference {
    uint32_t phandle;
    uint64_t address, size;
};

static bool reg_shape(const struct node *n, unsigned *bytes) {
    if (!n->reg || n->parent_ac < 1 || n->parent_ac > 2 || n->parent_sc < 1 || n->parent_sc > 2)
        return false;
    *bytes = 4 * (n->parent_ac + n->parent_sc);
    return n->reg_length && !(n->reg_length % *bytes);
}

static bool framebuffer(const struct node *n, struct platform_devices *d,
                        const struct reservation_reference *refs, size_t ref_count) {
    unsigned bytes;
    uint64_t address = 0, size = 0;
    if (n->bad_fb || !n->width_seen || !n->height_seen || !n->stride_seen || !n->width ||
        !n->height || n->width > 8192 || n->height > 8192 || !n->format ||
        !single_string(n->format, n->format_length))
        return false;
    if (n->reg) {
        if (!reg_shape(n, &bytes) || n->reg_length != bytes) return false;
        address = cells(n->reg, n->parent_ac);
        size = cells(n->reg + 4 * n->parent_ac, n->parent_sc);
    }
    /* memory-region is authoritative when present, per the simplefb binding. */
    if (n->memory_region_seen) {
        bool found = false;
        for (size_t i = 0; i < ref_count; ++i)
            if (refs[i].phandle == n->memory_region) {
                if (found) return false;
                address = refs[i].address;
                size = refs[i].size;
                found = true;
            }
        if (!found) return false;
    }
    if (!address || !size || size > UINT64_MAX - address || address > UINTPTR_MAX ||
        size - 1 > UINTPTR_MAX - address || size > 64u * 1024u * 1024u)
        return false;
    enum pixel_format format;
    unsigned bpp;
    if (equals((const char *)n->format, "r5g6b5")) {
        format = PIXEL_RGB565;
        bpp = 2;
    } else if (equals((const char *)n->format, "r8g8b8")) {
        format = PIXEL_RGB888;
        bpp = 3;
    } else if (equals((const char *)n->format, "x8r8g8b8")) {
        format = PIXEL_XRGB8888;
        bpp = 4;
    } else if (equals((const char *)n->format, "a8r8g8b8")) {
        format = PIXEL_ARGB8888;
        bpp = 4;
    } else return false;
    uint64_t needed = (uint64_t)n->stride * n->height;
    if (n->stride < (uint64_t)n->width * bpp || !needed || needed > size || needed > SIZE_MAX)
        return false;
    /* The 24-bit format uses byte stores; 16/32-bit surfaces need alignment. */
    if (bpp != 3 && ((address % bpp) || (n->stride % bpp))) return false;
    d->framebuffer =
        (struct framebuffer){(void *)(uintptr_t)address, n->width, n->height, n->stride, format};
    d->framebuffer_size = size;
    d->has_framebuffer = true;
    return true;
}

bool fdt_discover(const void *blob, struct platform_devices *d) {
    if (!d) return false;
    *d = (struct platform_devices){0};
    if (!blob) return false;
    const unsigned char *b = blob;
    if (be32(b) != 0xd00dfeed) return false;
    uint32_t total = be32(b + 4), off = be32(b + 8), strings_off = be32(b + 12),
             reserve_off = be32(b + 16);
    uint32_t strings_size = be32(b + 32), structure_size = be32(b + 36);
    if (be32(b + 20) < 17 || be32(b + 24) > 17 || total < 40 || total > 2 * 1024 * 1024 ||
        off < 40 || (off & 3) || off > total || (structure_size & 3) ||
        structure_size > total - off || strings_off < 40 || strings_off > total ||
        strings_size > total - strings_off ||
        overlap(off, structure_size, strings_off, strings_size) || reserve_off < 40 ||
        (reserve_off & 7) || reserve_off > total)
        return false;
    size_t reserve = reserve_off;
    for (;;) {
        if (reserve > total || total - reserve < 16 || overlap(reserve, 16, off, structure_size) ||
            overlap(reserve, 16, strings_off, strings_size))
            return false;
        uint64_t address = be64(b + reserve), size = be64(b + reserve + 8);
        reserve += 16;
        /* Like Linux, a zero size ends the map (libfdt writes all zeros). */
        if (!size) break;
        if (!add_region(d->reserved_regions, &d->reserved_region_count, MAX_RESERVED_REGIONS,
                        address, size))
            return false;
    }
    const unsigned char *p = b + off, *end = p + structure_size, *strings = b + strings_off;
    struct node stack[16], candidate = {0};
    struct reservation_reference refs[MAX_RESERVED_REGIONS];
    size_t ref_count = 0;
    int depth = -1;
    bool root_seen = false, candidate_seen = false;
    while ((size_t)(end - p) >= 4) {
        uint32_t token = be32(p);
        p += 4;
        if (token == 1) {
            if (depth == 15 || (depth < 0 && root_seen)) return false;
            if (depth < 0) root_seen = true;
            struct node next = {.ac = 2, .sc = 1};
            if (depth >= 0) {
                next.parent_ac = stack[depth].ac;
                next.parent_sc = stack[depth].sc;
                next.disabled = stack[depth].disabled;
                stack[depth].children_started = true;
            }
            size_t n = 0;
            while (p + n < end && p[n]) ++n;
            if (p + n == end) return false;
            size_t padded = (n + 4) & ~(size_t)3;
            if (padded > (size_t)(end - p)) return false;
            if (depth == -1 && n) return false;
            if (depth == 0) {
                next.chosen = equals((const char *)p, "chosen");
                next.reserved_container = equals((const char *)p, "reserved-memory");
                next.cpus = equals((const char *)p, "cpus");
            }
            stack[++depth] = next;
            p += padded;
        } else if (token == 3) {
            if (depth < 0 || stack[depth].children_started || (size_t)(end - p) < 8) return false;
            uint32_t len = be32(p), name_off = be32(p + 4);
            p += 8;
            /* Check before rounding: len+3 can wrap size_t on ARM32. */
            if (len > (size_t)(end - p)) return false;
            size_t padded = ((size_t)len + 3) & ~(size_t)3;
            if (padded > (size_t)(end - p) || name_off >= strings_size ||
                !terminated(strings + name_off, strings_size - name_off))
                return false;
            const char *name = (const char *)strings + name_off;
            struct node *n = &stack[depth];
            if (equals(name, "#address-cells")) {
                if (len != 4) return false;
                n->ac = be32(p);
            } else if (equals(name, "#size-cells")) {
                if (len != 4) return false;
                n->sc = be32(p);
            } else if (equals(name, "reg")) {
                n->reg = p;
                n->reg_length = len;
            } else if (equals(name, "ranges")) {
                n->ranges_seen = true;
                n->ranges_empty = !len;
            } else if (equals(name, "compatible")) {
                if (!string_list(p, len)) return false;
                n->fwcfg = compatible(p, len, "qemu,fw-cfg-mmio");
                n->virtio = compatible(p, len, "virtio,mmio");
                n->simplefb = compatible(p, len, "simple-framebuffer");
                n->pl061 = compatible(p, len, "arm,pl061");
                n->gpio_keys = compatible(p, len, "gpio-keys");
                n->psci_standard =
                    compatible(p, len, "arm,psci-0.2") || compatible(p, len, "arm,psci-1.0");
                n->psci = n->psci_standard || compatible(p, len, "arm,psci");
#ifdef SOS2_BOARD_GTA7LITE
                n->gicv3 = compatible(p, len, "arm,gic-v3");
#endif
                if (depth == 0) d->redmi2_wt88047 = compatible(p, len, "wingtech,wt88047");
            } else if (equals(name, "bootargs") && depth == 1 && n->chosen) {
                if (!single_string(p, len)) return false;
                own_arguments(d->bootargs, sizeof(d->bootargs), p);
            } else if (depth == 1 && n->chosen &&
                       (equals(name, "linux,initrd-start") || equals(name, "linux,initrd-end"))) {
                if (len != 4 && len != 8) return false;
                bool start = equals(name, "linux,initrd-start");
                if (start ? d->has_initrd_start : d->has_initrd_end) return false;
                uint64_t address = len == 4 ? be32(p) : be64(p);
                if (start) {
                    d->initrd_start = address;
                    d->has_initrd_start = true;
                } else {
                    d->initrd_end = address;
                    d->has_initrd_end = true;
                }
#ifdef SOS2_BOARD_GTA7LITE
            } else if (depth == 1 && n->chosen && equals(name, "atag,videolfb")) {
                /* LK's raw little-endian struct tag_videolfb: u64 fb_base,
                 * u32 islcmfound, fps and vram, then the LCM driver's name.
                 * Anything shorter or unterminated is not one. */
                uint32_t name_end = 20;
                while (name_end < len && p[name_end]) ++name_end;
                if (name_end < len) {
                    d->videolfb_base = le32(p) | (uint64_t)le32(p + 4) << 32;
                    d->videolfb_vram = le32(p + 16);
                    copy_string(d->lcm_name, sizeof(d->lcm_name), p + 20);
                    d->has_videolfb = true;
                }
            } else if (depth == 1 && n->chosen && equals(name, "ram_console")) {
                /* LK's raw little-endian struct mem_desc_t: start, size, type
                 * (1: SRAM) and the memory info's offset. Only a small SRAM
                 * buffer below DRAM is taken. */
                if (len == 16 && le32(p + 8) == 1 && le32(p) && le32(p + 4) >= 64 &&
                    le32(p + 4) <= 0x10000 && le32(p) < 0x40000000u - le32(p + 4)) {
                    d->ram_console_start = le32(p);
                    d->ram_console_size = le32(p + 4);
                    d->has_ram_console = true;
                }
#endif
            } else if (equals(name, "device_type")) {
                n->memory = len == 7 && compatible(p, len, "memory");
                n->cpu = len == 4 && compatible(p, len, "cpu");
            } else if (equals(name, "enable-method")) {
                if (!single_string(p, len)) return false;
                n->cpu_psci = equals((const char *)p, "psci");
            } else if (equals(name, "method")) {
                if (!single_string(p, len)) return false;
                n->method = equals((const char *)p, "smc")   ? CPU_BOOT_SMC
                            : equals((const char *)p, "hvc") ? CPU_BOOT_HVC
                                                             : CPU_BOOT_NONE;
            } else if (equals(name, "cpu_on")) {
                if (len != 4) return false;
                n->cpu_on = be32(p);
            } else if (equals(name, "status")) {
                /* MediaTek's LK writes "okay" without its NUL into nodes it enables
                 * (reserve-memory-sspm_share and scp@10500000 on the Galaxy Tab A7
                 * Lite); Linux reads status as a C string. */
                n->disabled = n->disabled || !(text_is(p, len, "okay") || text_is(p, len, "ok"));
            } else if (equals(name, "width")) {
                n->width_seen = true;
                if (len != 4) n->bad_fb = true;
                else n->width = be32(p);
            } else if (equals(name, "height")) {
                n->height_seen = true;
                if (len != 4) n->bad_fb = true;
                else n->height = be32(p);
            } else if (equals(name, "stride")) {
                n->stride_seen = true;
                if (len != 4) n->bad_fb = true;
                else n->stride = be32(p);
            } else if (equals(name, "format")) {
                n->format = p;
                n->format_length = len;
            } else if (equals(name, "memory-region")) {
                n->memory_region_seen = true;
                if (len != 4 || !be32(p)) n->bad_fb = true;
                else n->memory_region = be32(p);
            } else if (depth == 2 && stack[1].gpio_keys && equals(name, "linux,code")) {
                if (len != 4) return false;
                n->key_code = be32(p);
                n->key_code_seen = true;
            } else if (depth == 2 && stack[1].gpio_keys && equals(name, "gpios")) {
                /* <controller pin flags>. Other layouts are not a key SOS2 reads. */
                if (len == 12) {
                    n->key_gpio[0] = be32(p);
                    n->key_gpio[1] = be32(p + 4);
                    n->key_gpio_seen = true;
                }
            } else if (equals(name, "phandle") || equals(name, "linux,phandle")) {
                if (len != 4 || !be32(p) || be32(p) == UINT32_MAX ||
                    (n->phandle && n->phandle != be32(p)))
                    return false;
                n->phandle = be32(p);
            }
            p += padded;
        } else if (token == 2) {
            if (depth < 0) return false;
            struct node *n = &stack[depth];
#ifdef SOS2_BOARD_GTA7LITE
            if (!n->disabled && n->gicv3) d->gicv3 = true;
#endif
            if (!n->disabled && depth == 2 && stack[1].cpus && n->cpu) {
                if (!n->reg || n->parent_ac < 1 || n->parent_ac > 2 || n->parent_sc ||
                    n->reg_length != n->parent_ac * 4)
                    return false;
                uint64_t id = cells(n->reg, n->parent_ac);
                if (id > 0xffffff) return false;
                for (unsigned i = 0; i < d->cpu_count; ++i)
                    if (d->cpu_mpidr[i] == id) return false;
                if (n->cpu_psci && d->cpu_count < MAX_BOOT_CPUS)
                    d->cpu_mpidr[d->cpu_count++] = (uint32_t)id;
            }
            if (!n->disabled && n->psci) {
                if (d->psci_method || !n->method) return false;
                d->psci_method = n->method;
                d->psci_cpu_on = n->psci_standard ? 0x84000003 : n->cpu_on;
                if (!d->psci_cpu_on || (d->psci_cpu_on & (1u << 30)))
                    return false; /* SMC32 only. */
            }
            if (!n->disabled && depth == 2 && stack[1].gpio_keys && n->key_code_seen &&
                n->key_code == 116 && n->key_gpio_seen && !d->power_key_phandle) {
                d->power_key_phandle = n->key_gpio[0];
                d->power_key_pin = n->key_gpio[1];
            }
            bool reserved = depth == 2 && stack[1].reserved_container && !n->disabled;
            if (reserved && n->reg && (!stack[1].ranges_seen || !stack[1].ranges_empty))
                return false;
            if (!n->disabled && ((depth == 1 && (n->fwcfg || n->virtio || n->memory || n->pl061)) ||
                                 (reserved && n->reg))) {
                unsigned bytes;
                if (!reg_shape(n, &bytes)) return false;
                for (uint32_t offset = 0; offset < n->reg_length; offset += bytes) {
                    uint64_t address = cells(n->reg + offset, n->parent_ac);
                    uint64_t size = cells(n->reg + offset + 4 * n->parent_ac, n->parent_sc);
                    if (size > UINT64_MAX - address) return false;
                    if (!size) {
                        /* Like Linux, skip empty RAM and reservation tuples: CAF-LK-derived
                         * loaders such as lk2nd may append SMEM banks after the zero-size
                         * /memory placeholder instead of replacing it, and an empty
                         * reservation reserves nothing. */
                        if (reserved || n->memory) continue;
                        return false;
                    }
                    if (reserved) {
                        if (!add_region(d->reserved_regions, &d->reserved_region_count,
                                        MAX_RESERVED_REGIONS, address, size))
                            return false;
                        if (n->phandle) {
                            if (n->reg_length != bytes || ref_count == MAX_RESERVED_REGIONS)
                                return false;
                            for (size_t i = 0; i < ref_count; ++i)
                                if (refs[i].phandle == n->phandle) return false;
                            refs[ref_count++] =
                                (struct reservation_reference){n->phandle, address, size};
                        }
                    } else if (n->memory) {
                        if (!add_region(d->ram_regions, &d->ram_region_count, MAX_RAM_REGIONS,
                                        address, size))
                            return false;
                    } else if (!offset) {
                        if (address > UINTPTR_MAX) return false;
                        if (n->fwcfg) d->fwcfg = (uintptr_t)address;
                        if (n->pl061 && !d->gpio) {
                            d->gpio = (uintptr_t)address;
                            d->gpio_phandle = n->phandle;
                        }
                        if (n->virtio) {
                            if (d->virtio_count == MAX_VIRTIO) return false;
                            d->virtio[d->virtio_count++] = (uintptr_t)address;
                        }
                    }
                }
            }
            if (!n->disabled && n->simplefb && (depth == 1 || (depth == 2 && stack[1].chosen))) {
                if (depth == 2 && (!stack[1].ranges_seen || !stack[1].ranges_empty)) return false;
                if (candidate_seen) return false;
                candidate = *n;
                candidate_seen = true;
            }
            --depth;
        } else if (token == 9) {
            if (depth != -1 || !root_seen) return false;
            if (candidate_seen && !framebuffer(&candidate, d, refs, ref_count)) return false;
            d->power_key = d->gpio && d->power_key_phandle &&
                           d->power_key_phandle == d->gpio_phandle && d->power_key_pin < 8;
            d->dtb_size = total;
            return true;
        } else if (token != 4) return false;
    }
    return false;
}
