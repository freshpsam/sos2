/* The temporary volume's file system (include/ramfs.h). The region's first
 * pages hold the nodes; the rest are 4 KiB pages for files' data and their
 * indexes, the free ones in a list threaded through their first two bytes.
 * A file's index pages each name 2048 data pages, 0 for a hole, so a file
 * reaches 32 MiB with four of them. Bytes past a file's end are always zero,
 * so growing it exposes nothing old. Directories list their children, the
 * newest first. */
#include "ramfs.h"
#ifdef SOS2_RAMFS_HOST
#include <string.h>
#else
#include "platform.h"
#endif
#include "user_abi.h"

enum {
    PAGE = 4096,
    SLOTS = PAGE / 2,
    INDEXES = 4,
    NODES = 512,
    NONE = 0xffff,
    NOT_EMPTY = -39, /* littlefs's LFS_ERR_NOTEMPTY */
};

_Static_assert(INDEXES * SLOTS * (uint64_t)PAGE == RAMFS_FILE_MAX, "four index pages");

struct node {
    uint16_t parent, child, sibling;
    uint8_t type, linked;
    uint32_t refs, size;
    uint16_t index[INDEXES];
    char name[RAMFS_NAME_MAX + 1];
};

static unsigned char *base;
static struct node *nodes;
static uint32_t page_count, node_pages, free_count;
static uint16_t free_head;

static void *page_at(uint16_t page) { return base + (size_t)page * PAGE; }

static uint16_t page_take(void) {
    uint16_t page = free_head;
    if (!page) return 0;
    memcpy(&free_head, page_at(page), sizeof(free_head));
    --free_count;
    memset(page_at(page), 0, PAGE);
    return page;
}

static void page_give(uint16_t page) {
    memcpy(page_at(page), &free_head, sizeof(free_head));
    free_head = page;
    ++free_count;
}

bool ramfs_init(void *memory, size_t bytes) {
    node_pages = (NODES * sizeof(struct node) + PAGE - 1) / PAGE;
    if (!memory || (uintptr_t)memory % PAGE || bytes % PAGE || bytes / PAGE <= node_pages ||
        bytes / PAGE > NONE)
        return false;
    base = memory;
    nodes = memory;
    page_count = (uint32_t)(bytes / PAGE);
    memset(memory, 0, (size_t)node_pages * PAGE);
    free_head = 0;
    free_count = 0;
    /* Pages go out in ascending order. */
    for (uint32_t page = page_count; page-- > node_pages;) page_give((uint16_t)page);
    nodes[0] = (struct node){.child = NONE, .sibling = NONE, .type = RAMFS_DIR, .linked = 1};
    return true;
}

uint32_t ramfs_pages(void) { return page_count - node_pages; }

uint32_t ramfs_free_pages(void) { return free_count; }

/* The slot naming a file's data page, its index page made when asked. */
static uint16_t *slot(struct node *n, uint32_t page, bool make) {
    unsigned k = page / SLOTS;
    if (k >= INDEXES) return 0;
    if (!n->index[k] && (!make || !(n->index[k] = page_take()))) return 0;
    return (uint16_t *)page_at(n->index[k]) + page % SLOTS;
}

/* Pages past size given back, and the last kept page zeroed past size. */
static void cut(struct node *n, uint32_t size) {
    uint32_t keep = (uint32_t)(((uint64_t)size + PAGE - 1) / PAGE);
    for (unsigned k = 0; k < INDEXES; ++k) {
        if (!n->index[k]) continue;
        uint16_t *slots = page_at(n->index[k]);
        bool used = false;
        for (unsigned s = 0; s < SLOTS; ++s) {
            if (!slots[s]) continue;
            if (k * SLOTS + s >= keep) {
                page_give(slots[s]);
                slots[s] = 0;
            } else used = true;
        }
        if (!used) {
            page_give(n->index[k]);
            n->index[k] = 0;
        }
    }
    uint16_t *last = size % PAGE ? slot(n, size / PAGE, false) : 0;
    if (last && *last) memset((unsigned char *)page_at(*last) + size % PAGE, 0, PAGE - size % PAGE);
}

static void node_free(unsigned id) {
    cut(&nodes[id], 0);
    nodes[id] = (struct node){0};
}

static unsigned node_take(unsigned type, const char *name, size_t length) {
    for (unsigned id = 1; id < NODES; ++id) {
        if (nodes[id].type) continue;
        nodes[id] = (struct node){.child = NONE, .sibling = NONE, .type = (uint8_t)type};
        memcpy(nodes[id].name, name, length);
        nodes[id].name[length] = 0;
        return id;
    }
    return NONE;
}

static void link_node(unsigned dir, unsigned id) {
    nodes[id].parent = (uint16_t)dir;
    nodes[id].sibling = nodes[dir].child;
    nodes[dir].child = (uint16_t)id;
    nodes[id].linked = 1;
}

static void unlink_node(unsigned id) {
    uint16_t *at = &nodes[nodes[id].parent].child;
    while (*at != id) at = &nodes[*at].sibling;
    *at = nodes[id].sibling;
    nodes[id].sibling = NONE;
    nodes[id].linked = 0;
}

/* Out of the tree: gone now unless it is open. */
static void drop(unsigned id) {
    unlink_node(id);
    if (!nodes[id].refs) node_free(id);
}

/* Where a path leads: its node (NONE when only its last name is missing),
 * and that name in its parent directory (NONE for the root). */
struct place {
    unsigned parent, node;
    const char *name;
    size_t length;
};

static int locate(const char *path, struct place *p) {
    if (path[0] != '/') return USER_EINVAL;
    *p = (struct place){.parent = NONE, .node = 0, .name = path, .length = 0};
    for (const char *at = path + 1; *at;) {
        const char *end = at;
        while (*end && *end != '/') ++end;
        size_t length = (size_t)(end - at);
        if (!length) return USER_EINVAL;
        if (length > RAMFS_NAME_MAX) return USER_ENAMETOOLONG;
        if (p->node == NONE) return USER_ENOENT;
        if (nodes[p->node].type != RAMFS_DIR) return USER_ENOTDIR;
        p->parent = p->node;
        p->name = at;
        p->length = length;
        p->node = NONE;
        for (unsigned id = nodes[p->parent].child; id != NONE; id = nodes[id].sibling)
            if (!memcmp(nodes[id].name, at, length) && !nodes[id].name[length]) {
                p->node = id;
                break;
            }
        at = *end ? end + 1 : end;
    }
    return 0;
}

int ramfs_open(const char *path, unsigned flags, unsigned *node) {
    struct place p;
    int err = locate(path, &p);
    if (err) return err;
    if (p.node == NONE) {
        if (!(flags & RAMFS_CREATE)) return USER_ENOENT;
        p.node = node_take(RAMFS_FILE, p.name, p.length);
        if (p.node == NONE) return USER_ENOSPC;
        link_node(p.parent, p.node);
    } else {
        if ((flags & (RAMFS_CREATE | RAMFS_EXCLUSIVE)) == (RAMFS_CREATE | RAMFS_EXCLUSIVE))
            return USER_EEXIST;
        if (nodes[p.node].type != RAMFS_FILE) return USER_EISDIR;
        if (flags & RAMFS_TRUNCATE) {
            cut(&nodes[p.node], 0);
            nodes[p.node].size = 0;
        }
    }
    ++nodes[p.node].refs;
    *node = p.node;
    return 0;
}

int ramfs_open_dir(const char *path, unsigned *node) {
    struct place p;
    int err = locate(path, &p);
    if (err) return err;
    if (p.node == NONE) return USER_ENOENT;
    if (nodes[p.node].type != RAMFS_DIR) return USER_ENOTDIR;
    ++nodes[p.node].refs;
    *node = p.node;
    return 0;
}

void ramfs_release(unsigned node) {
    struct node *n = &nodes[node];
    if (n->refs) --n->refs;
    if (!n->refs && !n->linked && node) node_free(node);
}

uint32_t ramfs_size(unsigned node) { return nodes[node].size; }

int ramfs_read(unsigned node, uint32_t offset, void *out, uint32_t size) {
    struct node *n = &nodes[node];
    if (n->type != RAMFS_FILE) return USER_EISDIR;
    if (offset >= n->size) return 0;
    if (size > n->size - offset) size = n->size - offset;
    unsigned char *to = out;
    for (uint32_t done = 0; done < size;) {
        uint32_t at = offset + done, within = at % PAGE;
        uint32_t step = PAGE - within < size - done ? PAGE - within : size - done;
        uint16_t *s = slot(n, at / PAGE, false);
        if (s && *s) memcpy(to + done, (unsigned char *)page_at(*s) + within, step);
        else memset(to + done, 0, step);
        done += step;
    }
    return (int)size;
}

int ramfs_write(unsigned node, uint32_t offset, const void *data, uint32_t size) {
    struct node *n = &nodes[node];
    if (n->type != RAMFS_FILE) return USER_EISDIR;
    if (!size) return 0;
    if (offset > RAMFS_FILE_MAX || size > RAMFS_FILE_MAX - offset) return USER_EFBIG;
    uint32_t first = offset / PAGE, last = (offset + size - 1) / PAGE, needed = 0;
    for (unsigned k = first / SLOTS; k <= last / SLOTS; ++k) needed += !n->index[k];
    for (uint32_t page = first; page <= last; ++page) {
        uint16_t *s = slot(n, page, false);
        needed += !s || !*s;
    }
    if (needed > free_count) return USER_ENOSPC;
    const unsigned char *from = data;
    for (uint32_t done = 0; done < size;) {
        uint32_t at = offset + done, within = at % PAGE;
        uint32_t step = PAGE - within < size - done ? PAGE - within : size - done;
        uint16_t *s = slot(n, at / PAGE, true);
        if (!*s) *s = page_take();
        memcpy((unsigned char *)page_at(*s) + within, from + done, step);
        done += step;
    }
    if (offset + size > n->size) n->size = offset + size;
    return (int)size;
}

int ramfs_truncate(unsigned node, uint32_t size) {
    struct node *n = &nodes[node];
    if (n->type != RAMFS_FILE) return USER_EISDIR;
    if (size > RAMFS_FILE_MAX) return USER_EFBIG;
    if (size < n->size) cut(n, size);
    n->size = size;
    return 0;
}

static void describe(unsigned id, const char *name, struct ramfs_entry *e) {
    e->type = nodes[id].type;
    e->size = nodes[id].type == RAMFS_FILE ? nodes[id].size : 0;
    size_t length = strlen(name);
    memcpy(e->name, name, length + 1);
}

int ramfs_read_dir(unsigned node, unsigned *position, struct ramfs_entry *e) {
    if (nodes[node].type != RAMFS_DIR) return USER_ENOTDIR;
    if (*position < 2) {
        describe(node, *position ? ".." : ".", e);
        e->type = RAMFS_DIR;
        ++*position;
        return 1;
    }
    unsigned id = nodes[node].child;
    for (unsigned i = 2; id != NONE && i < *position; ++i) id = nodes[id].sibling;
    if (id == NONE) return 0;
    describe(id, nodes[id].name, e);
    ++*position;
    return 1;
}

int ramfs_stat(const char *path, struct ramfs_entry *e) {
    struct place p;
    int err = locate(path, &p);
    if (err) return err;
    if (p.node == NONE) return USER_ENOENT;
    describe(p.node, p.node ? nodes[p.node].name : "/", e);
    return 0;
}

int ramfs_mkdir(const char *path) {
    struct place p;
    int err = locate(path, &p);
    if (err) return err;
    if (p.node != NONE) return USER_EEXIST;
    unsigned id = node_take(RAMFS_DIR, p.name, p.length);
    if (id == NONE) return USER_ENOSPC;
    link_node(p.parent, id);
    return 0;
}

int ramfs_remove(const char *path) {
    struct place p;
    int err = locate(path, &p);
    if (err) return err;
    if (p.node == NONE) return USER_ENOENT;
    if (!p.node) return USER_EINVAL;
    if (nodes[p.node].type == RAMFS_DIR && nodes[p.node].child != NONE) return NOT_EMPTY;
    drop(p.node);
    return 0;
}

static void remove_below(unsigned dir) {
    while (nodes[dir].child != NONE) {
        unsigned id = nodes[dir].child;
        if (nodes[id].type == RAMFS_DIR) remove_below(id);
        drop(id);
    }
}

int ramfs_remove_tree(const char *path) {
    struct place p;
    int err = locate(path, &p);
    if (err) return err;
    if (p.node == NONE) return USER_ENOENT;
    if (!p.node) return USER_EINVAL;
    if (nodes[p.node].type == RAMFS_DIR) remove_below(p.node);
    drop(p.node);
    return 0;
}

int ramfs_rename(const char *from, const char *to) {
    struct place a, b;
    int err = locate(from, &a);
    if (err) return err;
    if (a.node == NONE) return USER_ENOENT;
    if (!a.node) return USER_EINVAL;
    err = locate(to, &b);
    if (err) return err;
    if (b.parent == NONE) return USER_EINVAL;
    /* Never below itself. */
    for (unsigned dir = b.parent;; dir = nodes[dir].parent) {
        if (dir == a.node) return USER_EINVAL;
        if (!dir) break;
    }
    if (b.node == a.node) return 0;
    if (b.node != NONE) {
        if (nodes[b.node].type == RAMFS_DIR) {
            if (nodes[a.node].type != RAMFS_DIR) return USER_EISDIR;
            if (nodes[b.node].child != NONE) return NOT_EMPTY;
        } else if (nodes[a.node].type == RAMFS_DIR) return USER_ENOTDIR;
        drop(b.node);
    }
    unlink_node(a.node);
    memcpy(nodes[a.node].name, b.name, b.length);
    nodes[a.node].name[b.length] = 0;
    link_node(b.parent, a.node);
    return 0;
}
