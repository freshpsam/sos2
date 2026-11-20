#include "page_alloc.h"
#include "platform.h"
#include "boot_progress.h"

/* Each run's first entry holds its length: an allocation's, or with WITHHELD
 * a gap between the pool's stretches of RAM. The rest of a run hold INTERIOR.
 * Heads reject interior, partial and double frees, and searches skip runs
 * whole. */
#define WITHHELD 0x80000000u
#define INTERIOR UINT32_MAX

static uintptr_t pool;
static unsigned capacity, available, first_free;
static uint32_t allocations[PAGE_POOL_PAGES];

bool pages_init(void *start, size_t size) {
    if (!start || ((uintptr_t)start & (PAGE_SIZE - 1)) || !size || size % PAGE_SIZE ||
        size / PAGE_SIZE > PAGE_POOL_PAGES || (uintptr_t)start > UINTPTR_MAX - size)
        return false;
    pool = (uintptr_t)start;
    capacity = available = (unsigned)(size / PAGE_SIZE);
    first_free = 0;
    memset(allocations, 0, sizeof(allocations));
    return true;
}

static void mark(unsigned index, unsigned count, uint32_t head) {
    allocations[index] = head;
    for (unsigned n = 1; n < count; ++n) allocations[index + n] = INTERIOR;
    available -= count;
}

/* The next run from a free page or a run's head. */
static unsigned next_run(unsigned index) {
    uint32_t head = allocations[index];
    return index + (!head || head == INTERIOR ? 1 : head & ~WITHHELD);
}

static void advance_first_free(void) {
    while (first_free < capacity && allocations[first_free]) first_free = next_run(first_free);
}

bool pages_withhold(void *start, size_t size) {
    uintptr_t address = (uintptr_t)start;
    if (address < pool || (address - pool) % PAGE_SIZE || !size || size % PAGE_SIZE) return false;
    uintptr_t index = (address - pool) / PAGE_SIZE, count = size / PAGE_SIZE;
    if (index >= capacity || count > capacity - index) return false;
    for (uintptr_t n = 0; n < count; ++n)
        if (allocations[index + n]) return false;
    mark((unsigned)index, (unsigned)count, WITHHELD | (uint32_t)count);
    advance_first_free();
    return true;
}

void *pages_reserve(unsigned count, unsigned alignment) {
    if (!count || count > available || !alignment || (alignment & (alignment - 1)) ||
        alignment > PAGE_POOL_PAGES)
        return 0;
    for (unsigned i = first_free; i <= capacity - count;) {
        if (allocations[i]) {
            i = next_run(i);
            continue;
        }
        if ((pool / PAGE_SIZE + i) & (alignment - 1)) {
            ++i;
            continue;
        }
        unsigned n = 0;
        while (n < count && !allocations[i + n]) ++n;
        if (n != count) {
            i += n;
            continue;
        }
        mark(i, count, count);
        advance_first_free();
        return (void *)(pool + (uintptr_t)i * PAGE_SIZE);
    }
    return 0;
}

void *pages_alloc(unsigned count, unsigned alignment) {
    unsigned char *result = pages_reserve(count, alignment);
    if (!result) return 0;
    size_t bytes = (size_t)count * PAGE_SIZE;
    for (size_t offset = 0; offset < bytes;) {
        size_t chunk = bytes - offset;
        if (chunk > 65536) chunk = 65536;
        memset(result + offset, 0, chunk);
        offset += chunk;
        boot_progress_poll();
    }
    return result;
}

bool pages_free(void *start, unsigned count) {
    uintptr_t address = (uintptr_t)start;
    if (!count || address < pool || (address - pool) % PAGE_SIZE) return false;
    uintptr_t index = (address - pool) / PAGE_SIZE;
    if (index >= capacity || count > capacity - index || allocations[index] != count) return false;
    for (unsigned n = 0; n < count; ++n) allocations[index + n] = 0;
    if (index < first_free) first_free = (unsigned)index;
    available += count;
    return true;
}

unsigned pages_available(void) { return available; }
