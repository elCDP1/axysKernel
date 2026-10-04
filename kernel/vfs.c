#include "axys/vfs.h"
#include "axys/console.h"
#include "axys/heap.h"
#include "axys/panic.h"
#include "axys/printf.h"
#include "axys/string.h"
#ifdef AXYS_HOST_TEST
/* Host unit tests run in user space where cli/xchg-based locking is neither
 * possible nor needed: single thread, no interrupts. */
struct axys_spinlock { int unused; };
#define axys_spin_lock_irqsave(l) ((void)(l), (axys_uint64_t)0)
#define axys_spin_unlock_irqrestore(l, f) ((void)(l), (void)(f))
#else
#include "axys/spinlock.h"
#endif

/* One coarse lock guards the whole tree (nodes, free list, child arrays, file
 * buffers). Public entry points take it; the *_nl helpers assume it is held.
 * Lock order: vfs_lock -> heap_lock -> pmm_lock. Never call a public entry
 * point while holding it (the lock is not recursive). */
static struct axys_spinlock vfs_lock;
static axys_uint64_t mutation_count;

axys_uint64_t axys_vfs_mutations(void)
{
    return mutation_count;
}

/* In-memory tree. Nodes live in a fixed pool (indices are the handles); file
 * contents and directory child lists are heap-allocated and grow on demand, so
 * there is no per-file or per-directory cap other than AXYS_VFS_MAX_FILE_BYTES
 * and the pool size. Unlinked nodes are recycled through a free list. */
struct vfs_node {
    char name[AXYS_VFS_NAME_MAX];
    axys_vfs_type_t type;
    axys_int32_t parent; /* also the free-list link while !used */
    int used;
    axys_int32_t *children; /* dirs: heap array of node indices */
    axys_uint32_t child_count;
    axys_uint32_t child_cap;
    axys_uint8_t *data; /* files: heap buffer */
    axys_size_t size;
    axys_size_t cap;
    axys_uint32_t mode;
    axys_uint32_t uid;
    axys_uint32_t gid;
};

static struct vfs_node nodes[AXYS_VFS_MAX_NODES];
static axys_uint32_t node_high_water; /* nodes[0..high_water) have been handed out */
static axys_int32_t free_list = -1;
static axys_uint32_t live_nodes;
/* Per-slot generation, bumped every time a slot is handed out. It lives outside
 * struct vfs_node so node_release()'s memset cannot reset it. Holders of a
 * long-lived node index (open file descriptors) record the generation and must
 * treat a mismatch as a dead handle: without it, unlink + create reuses the slot
 * and an old descriptor would silently read or write an unrelated file whose
 * permissions were never checked against that descriptor's owner. */
static axys_uint32_t node_generation[AXYS_VFS_MAX_NODES];

/* Standard Linux-style top-level directories (FHS): user space finds /bin,
 * /etc, /home, ... exactly where it expects them. */
static const char *const standard_dirs[] = {
    "bin",  "boot", "dev", "etc", "home", "lib", "media", "mnt",
    "opt",  "proc", "root", "run", "sbin", "srv", "sys",  "tmp",
    "usr",  "var",
};

static int node_valid(axys_vfs_node_t node)
{
    return node >= 0 && (axys_uint32_t)node < node_high_water && nodes[node].used;
}

static axys_int32_t node_alloc(const char *name, axys_vfs_type_t type, axys_int32_t parent)
{
    axys_int32_t index;
    struct vfs_node *node;

    if (free_list >= 0) {
        index = free_list;
        free_list = nodes[index].parent;
    } else if (node_high_water < AXYS_VFS_MAX_NODES) {
        index = (axys_int32_t)node_high_water++;
    } else {
        return -1;
    }
    node = &nodes[index];
    axys_memset(node, 0, sizeof(*node));
    axys_strlcpy(node->name, name, sizeof(node->name));
    node->type = type;
    node->mode = type == AXYS_VFS_DIR ? 0755u : 0644u;
    node->parent = parent;
    node->used = 1;
    if (++node_generation[index] == 0) {
        node_generation[index] = 1; /* 0 is reserved for "no such node" */
    }
    ++live_nodes;
    return index;
}

static void node_release(axys_int32_t index)
{
    struct vfs_node *node = &nodes[index];

    axys_kfree(node->children);
    axys_kfree(node->data);
    axys_memset(node, 0, sizeof(*node));
    node->parent = free_list; /* used == 0 */
    free_list = index;
    --live_nodes;
}

static axys_int32_t node_find_child(axys_int32_t dir, const char *name, axys_size_t name_len)
{
    if (!node_valid(dir) || nodes[dir].type != AXYS_VFS_DIR) {
        return -1;
    }
    for (axys_uint32_t i = 0; i < nodes[dir].child_count; ++i) {
        axys_int32_t child = nodes[dir].children[i];

        /* node_valid() before dereferencing: a stale index left behind by an
         * inconsistent tree must never make us read another slot's name. */
        if (node_valid(child) &&
            axys_strncmp(nodes[child].name, name, name_len) == 0 &&
            nodes[child].name[name_len] == '\0') {
            return child;
        }
    }
    return -1;
}

static int node_link(axys_int32_t parent, axys_int32_t child)
{
    struct vfs_node *dir = &nodes[parent];

    if (dir->child_count == dir->child_cap) {
        axys_uint32_t new_cap = dir->child_cap ? dir->child_cap * 2u : 8u;
        axys_int32_t *grown = axys_kmalloc((axys_size_t)new_cap * sizeof(axys_int32_t));

        if (grown == AXYS_NULL) {
            return -1;
        }
        if (dir->children != AXYS_NULL) {
            axys_memcpy(grown, dir->children, (axys_size_t)dir->child_count * sizeof(axys_int32_t));
            axys_kfree(dir->children);
        }
        dir->children = grown;
        dir->child_cap = new_cap;
    }
    dir->children[dir->child_count++] = child;
    return 0;
}

static void node_unlink_from_parent(axys_int32_t child)
{
    axys_int32_t parent = nodes[child].parent;
    struct vfs_node *dir;

    if (!node_valid(parent) || nodes[parent].type != AXYS_VFS_DIR) {
        return; /* defensive: never touch a stale/free parent slot */
    }
    dir = &nodes[parent];
    for (axys_uint32_t i = 0; i < dir->child_count; ++i) {
        if (dir->children[i] == child) {
            dir->children[i] = dir->children[--dir->child_count];
            return;
        }
    }
}

/* Walk `path` one component at a time. With `create`, the final component is
 * created (the parent must exist); it fails if it already exists. */
static axys_vfs_node_t walk(const char *path, int create, axys_vfs_type_t create_type)
{
    axys_int32_t current = 0; /* node 0 is always "/" */
    const char *cursor;
    axys_size_t path_len = 1;

    if (path == AXYS_NULL || path[0] != '/') {
        return -1;
    }
    cursor = path + 1;
    if (*cursor == '\0') {
        return create ? -1 : current;
    }

    while (*cursor != '\0') {
        const char *segment_start = cursor;
        axys_size_t segment_len = 0;
        int is_last;
        axys_int32_t next;

        while (cursor[segment_len] != '\0' && cursor[segment_len] != '/') {
            ++segment_len;
            if (segment_len >= AXYS_VFS_NAME_MAX || path_len + segment_len >= AXYS_VFS_MAX_PATH) {
                return -1;
            }
        }
        if (segment_len == 0) {
            return -1;
        }
        /* Reject "." and ".." outright: the tree has no hard links, so a link
         * named "." would make node_find_child() match the parent itself and
         * walk("/x/./y") would loop forever. Creating them is an error, and
         * looking them up fails cleanly instead of hanging or aliasing. */
        if (segment_len == 1 && segment_start[0] == '.') {
            return -1;
        }
        if (segment_len == 2 && segment_start[0] == '.' && segment_start[1] == '.') {
            return -1;
        }
        is_last = (segment_start[segment_len] == '\0');
        next = node_find_child(current, segment_start, segment_len);

        if (next < 0) {
            char name[AXYS_VFS_NAME_MAX];

            if (!create || !is_last || nodes[current].type != AXYS_VFS_DIR) {
                return -1;
            }
            axys_memcpy(name, segment_start, segment_len);
            name[segment_len] = '\0';
            next = node_alloc(name, create_type, current);
            if (next < 0) {
                return -1;
            }
            if (node_link(current, next) != 0) {
                node_release(next);
                return -1;
            }
        } else if (create && is_last) {
            return -1; /* already exists */
        }

        current = next;
        path_len += segment_len;
        cursor = segment_start + segment_len;
        if (*cursor == '/') {
            ++path_len;
            if (path_len >= AXYS_VFS_MAX_PATH) {
                return -1;
            }
            ++cursor;
            if (create && *cursor == '\0') {
                return -1; /* a trailing slash names an existing node */
            }
        }
    }
    return current;
}

static void vfs_init_nl(void)
{
    for (axys_uint32_t i = 0; i < node_high_water; ++i) {
        if (nodes[i].used) {
            axys_kfree(nodes[i].children);
            axys_kfree(nodes[i].data);
        }
    }
    axys_memset(nodes, 0, sizeof(nodes));
    node_high_water = 0;
    free_list = -1;
    live_nodes = 0;

    if (node_alloc("/", AXYS_VFS_DIR, -1) != 0) {
        axys_panic("vfs: cannot allocate root");
    }
    for (axys_size_t i = 0; i < AXYS_ARRAY_SIZE(standard_dirs); ++i) {
        char path[AXYS_VFS_NAME_MAX + 1];

        path[0] = '/';
        axys_strlcpy(path + 1, standard_dirs[i], sizeof(path) - 1);
        if (walk(path, 1, AXYS_VFS_DIR) < 0) {
            axys_panic("vfs: failed to build standard hierarchy");
        }
        if (axys_strcmp(standard_dirs[i], "root") == 0) {
            nodes[walk(path, 0, AXYS_VFS_DIR)].mode = 0700u; /* root's home: private */
        } else if (axys_strcmp(standard_dirs[i], "tmp") == 0) {
            /* World-writable scratch with the sticky bit (01777): anyone may
             * create entries, but only the owner, the directory owner or root
             * may remove or rename them (see axys_vfs_sticky_ok). */
            nodes[walk(path, 0, AXYS_VFS_DIR)].mode = 01777u;
        }
    }
}

static axys_vfs_node_t vfs_lookup_nl(const char *path)
{
    return walk(path, 0, AXYS_VFS_DIR);
}

static axys_vfs_node_t vfs_create_nl(const char *path, axys_vfs_type_t type)
{
    if (type != AXYS_VFS_FILE && type != AXYS_VFS_DIR) {
        return -1;
    }
    return walk(path, 1, type);
}

static axys_int32_t vfs_mkdirs_nl(const char *path)
{
    char partial[AXYS_VFS_MAX_PATH];
    axys_size_t length;

    if (path == AXYS_NULL || path[0] != '/') {
        return -1;
    }
    length = axys_strlen(path);
    if (length >= sizeof(partial)) {
        return -1;
    }
    /* Trailing slashes ("/a/b/") must not produce an empty final component:
     * normalize them away before the ancestor walk. */
    while (length > 1 && path[length - 1] == '/') {
        --length;
    }
    for (axys_size_t i = 1; i <= length; ++i) {
        if (path[i] != '/' && path[i] != '\0') {
            continue;
        }
        if (i == 1) {
            continue;
        }
        axys_memcpy(partial, path, i);
        partial[i] = '\0';
        {
            axys_vfs_node_t existing = walk(partial, 0, AXYS_VFS_DIR);

            if (existing >= 0) {
                if (nodes[existing].type != AXYS_VFS_DIR) {
                    return -1;
                }
            } else if (walk(partial, 1, AXYS_VFS_DIR) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

static axys_int32_t vfs_unlink_nl(const char *path)
{
    axys_vfs_node_t node = walk(path, 0, AXYS_VFS_DIR);

    if (node <= 0) { /* missing, or root (0) */
        return -1;
    }
    if (nodes[node].type == AXYS_VFS_DIR && nodes[node].child_count != 0) {
        return -1;
    }
    node_unlink_from_parent(node);
    node_release(node);
    return 0;
}

static axys_int32_t vfs_size_nl(axys_vfs_node_t node)
{
    if (!node_valid(node) || nodes[node].type != AXYS_VFS_FILE) {
        return -1;
    }
    return (axys_int32_t)nodes[node].size;
}

static int file_reserve(struct vfs_node *node, axys_size_t needed)
{
    axys_size_t new_cap;
    axys_uint8_t *grown;

    if (needed > AXYS_VFS_MAX_FILE_BYTES) {
        return -1;
    }
    if (needed <= node->cap) {
        return 0;
    }
    new_cap = node->cap ? node->cap : 64u;
    while (new_cap < needed) {
        new_cap *= 2u;
    }
    if (new_cap > AXYS_VFS_MAX_FILE_BYTES) {
        new_cap = AXYS_VFS_MAX_FILE_BYTES;
    }
    grown = axys_kzalloc(new_cap);
    if (grown == AXYS_NULL) {
        return -1;
    }
    if (node->data != AXYS_NULL) {
        axys_memcpy(grown, node->data, node->size);
        axys_kfree(node->data);
    }
    node->data = grown;
    node->cap = new_cap;
    return 0;
}

static axys_int32_t vfs_pwrite_nl(axys_vfs_node_t node, axys_size_t offset, const void *data, axys_size_t length)
{
    struct vfs_node *file;
    axys_size_t end;

    if (!node_valid(node) || nodes[node].type != AXYS_VFS_FILE || (data == AXYS_NULL && length != 0)) {
        return -1;
    }
    if (offset > AXYS_VFS_MAX_FILE_BYTES || length > AXYS_VFS_MAX_FILE_BYTES - offset) {
        return -1;
    }
    file = &nodes[node];
    end = offset + length;
    if (file_reserve(file, end) != 0) {
        return -1;
    }
    if (offset > file->size) {
        axys_memset(file->data + file->size, 0, offset - file->size);
    }
    if (length != 0) {
        axys_memcpy(file->data + offset, data, length);
    }
    if (end > file->size) {
        file->size = end;
    }
    return (axys_int32_t)length;
}

static axys_int32_t vfs_pread_nl(axys_vfs_node_t node, axys_size_t offset, void *buffer, axys_size_t length)
{
    struct vfs_node *file;

    if (!node_valid(node) || nodes[node].type != AXYS_VFS_FILE || (buffer == AXYS_NULL && length != 0)) {
        return -1;
    }
    file = &nodes[node];
    if (offset >= file->size || length == 0) {
        return 0;
    }
    if (length > file->size - offset) {
        length = file->size - offset;
    }
    axys_memcpy(buffer, file->data + offset, length);
    return (axys_int32_t)length;
}

static axys_int32_t vfs_write_nl(axys_vfs_node_t node, const void *data, axys_size_t length)
{
    if (!node_valid(node) || nodes[node].type != AXYS_VFS_FILE) {
        return -1;
    }
    /* Reserve first: truncating before a failed allocation would destroy the
     * old contents while still reporting an error to the caller. */
    if (length > AXYS_VFS_MAX_FILE_BYTES || file_reserve(&nodes[node], length) != 0) {
        return -1;
    }
    nodes[node].size = 0; /* replace whole contents */
    return vfs_pwrite_nl(node, 0, data, length);
}

static axys_int32_t vfs_read_nl(axys_vfs_node_t node, void *buffer, axys_size_t length)
{
    return vfs_pread_nl(node, 0, buffer, length);
}

/* The returned pointer stays valid until the node is unlinked; callers that
 * race with unlink must serialise themselves (names are diagnostics only). */
const char *axys_vfs_name(axys_vfs_node_t node)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    const char *name = node_valid(node) ? nodes[node].name : "";

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return name;
}

axys_vfs_type_t axys_vfs_type(axys_vfs_node_t node)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_vfs_type_t type = node_valid(node) ? nodes[node].type : AXYS_VFS_FILE;

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return type;
}

axys_int32_t axys_vfs_name_copy(axys_vfs_node_t node, char *out, axys_size_t cap)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_int32_t rc = -1;

    if (node_valid(node) && out != AXYS_NULL && cap != 0) {
        axys_size_t len = axys_strlen(nodes[node].name);

        if (len < cap) {
            axys_memcpy(out, nodes[node].name, len + 1);
            rc = (axys_int32_t)len;
        }
    }
    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return rc;
}

int axys_vfs_sticky_ok(axys_vfs_node_t parent, axys_vfs_node_t target, axys_uint32_t uid)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    int rc = -1;

    if (node_valid(parent) && nodes[parent].type == AXYS_VFS_DIR && node_valid(target)) {
        if ((nodes[parent].mode & 01000u) == 0u || uid == 0 ||
            uid == nodes[target].uid || uid == nodes[parent].uid) {
            rc = 0;
        }
    }
    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return rc;
}

static axys_vfs_node_t vfs_next_child_nl(axys_vfs_node_t dir, axys_vfs_node_t child)
{
    axys_uint32_t start = 0;

    if (!node_valid(dir) || nodes[dir].type != AXYS_VFS_DIR) {
        return -1;
    }
    if (child >= 0) {
        for (axys_uint32_t i = 0; i < nodes[dir].child_count; ++i) {
            if (nodes[dir].children[i] == child) {
                start = i + 1u;
                break;
            }
        }
    }
    return start < nodes[dir].child_count ? nodes[dir].children[start] : -1;
}

/* Depth of a directory from the root (root itself is 0), following parent
 * links. A consistent tree terminates at 0; if a corrupted parent chain ever
 * looped, this returns -1 after a bounded walk instead of hanging forever. */
static axys_int32_t dir_depth(axys_int32_t node)
{
    axys_int32_t depth = 0;

    while (node != 0) {
        node = nodes[node].parent;
        if (!node_valid(node)) {
            return -1;
        }
        if (++depth > AXYS_VFS_MAX_NODES) {
            return -1; /* impossible in a sane tree: cycle guard */
        }
    }
    return depth;
}

/* Split `to` into destination-directory path + final component name. The
 * destination's parent must already exist; the final component must not.
 * Returns 0 and fills the outputs, -1 on any malformed/occupied spelling. */
static int rename_split_target(const char *to, char *parent_path, axys_size_t parent_cap,
                               const char **name_out, axys_size_t *name_len_out)
{
    const char *slash;

    if (to == AXYS_NULL || to[0] != '/') {
        return -1;
    }
    slash = to;
    while (*slash != '\0' && *slash != '/') {
        ++slash;
    }
    /* only '/' or empty -> renaming onto the root itself */
    if (*slash == '\0' || slash[1] == '\0') {
        return -1;
    }
    /* find the last slash */
    {
        const char *last = AXYS_NULL;
        const char *c;

        for (c = to; *c != '\0'; ++c) {
            if (*c == '/') {
                last = c;
            }
        }
        if (last == to) {
            /* "/name": the destination parent is the root itself. */
            if (parent_cap < 2u) {
                return -1;
            }
            parent_path[0] = '/';
            parent_path[1] = '\0';
        } else {
            axys_size_t parent_len = (axys_size_t)(last - to);

            if (parent_len >= parent_cap) {
                return -1;
            }
            axys_memcpy(parent_path, to, parent_len);
            parent_path[parent_len] = '\0';
        }
        *name_out = last + 1;
        *name_len_out = 0;
        while ((*name_out)[*name_len_out] != '\0') {
            ++(*name_len_out);
            if (*name_len_out >= AXYS_VFS_NAME_MAX) {
                return -1;
            }
        }
        if (*name_len_out == 0) {
            return -1; /* trailing slash */
        }
        /* "." and ".." are never valid entry names: walk() rejects them so
         * the destination could otherwise smuggle a node with such a name
         * into the tree, aliasing the parent or breaking future walks. */
        if (*name_len_out == 1 && (*name_out)[0] == '.') {
            return -1;
        }
        if (*name_len_out == 2 && (*name_out)[0] == '.' && (*name_out)[1] == '.') {
            return -1;
        }
    }
    return 0;
}

/* Move `node` under the (already validated) destination directory `new_parent`
 * as `new_name`. Rejects moving a directory into its own subtree (which would
 * orphan every node below it and let walk() loop forever). Caller holds vfs_lock. */
static int node_rename(axys_int32_t node, axys_int32_t new_parent, const char *new_name,
                       axys_size_t new_name_len)
{
    struct vfs_node *dir = &nodes[new_parent];
    axys_int32_t check;

    if (nodes[node].type == AXYS_VFS_DIR) {
        check = new_parent;
        for (;;) {
            if (check == node) {
                return -1; /* cannot move a directory into itself/its subtree */
            }
            if (check == 0) {
                break;
            }
            check = nodes[check].parent;
            if (!node_valid(check)) {
                return -1;
            }
        }
        if (dir_depth(node) + dir_depth(new_parent) > AXYS_VFS_MAX_NODES) {
            return -1; /* bounded depth keeps future walks O(n) at worst */
        }
    }

    if (new_name_len >= AXYS_VFS_NAME_MAX) {
        return -1;
    }
    if ((new_name_len == 1 && new_name[0] == '.') ||
        (new_name_len == 2 && new_name[0] == '.' && new_name[1] == '.')) {
        return -1;
    }
    if (dir->child_count == dir->child_cap) {
        /* Grow first so we never unlink from the old parent on ENOMEM. */
        axys_uint32_t new_cap = dir->child_cap ? dir->child_cap * 2u : 8u;
        axys_int32_t *grown = axys_kmalloc((axys_size_t)new_cap * sizeof(axys_int32_t));

        if (grown == AXYS_NULL) {
            return -1;
        }
        if (dir->children != AXYS_NULL) {
            axys_memcpy(grown, dir->children, (axys_size_t)dir->child_count * sizeof(axys_int32_t));
            axys_kfree(dir->children);
        }
        dir->children = grown;
        dir->child_cap = new_cap;
    }
    node_unlink_from_parent(node);
    axys_strlcpy(nodes[node].name, new_name, sizeof(nodes[node].name));
    nodes[node].parent = new_parent;
    dir->children[dir->child_count++] = node;
    return 0;
}

/* rename(2)-style move. Both paths are resolved under one lock acquisition,
 * so there is no TOCTOU window between lookup and the actual relink. Moving a
 * directory into its own subtree is rejected (it would orphan its descendants
 * and make walks non-terminating). Files may overwrite an existing regular
 * file at the destination (POSIX semantics); directories never do. */
axys_int32_t axys_vfs_rename(const char *from, const char *to)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_int32_t rc = -1;
    axys_vfs_node_t src;
    char to_parent[AXYS_VFS_MAX_PATH];
    const char *name;
    axys_size_t name_len;
    axys_vfs_node_t dst_dir;
    axys_int32_t existing;

    src = walk(from, 0, AXYS_VFS_DIR);
    if (src <= 0) { /* missing or the root itself */
        goto out;
    }
    if (rename_split_target(to, to_parent, sizeof(to_parent), &name, &name_len) != 0) {
        goto out;
    }
    dst_dir = walk(to_parent, 0, AXYS_VFS_DIR);
    if (dst_dir < 0 || nodes[dst_dir].type != AXYS_VFS_DIR) {
        goto out;
    }
    existing = node_find_child(dst_dir, name, name_len);
    if (existing >= 0) {
        if (nodes[existing].type == AXYS_VFS_DIR ||
            nodes[src].type != AXYS_VFS_FILE || existing == src) {
            goto out; /* dir->dir replace, file->dir, or self-rename */
        }
        node_unlink_from_parent(existing);
        node_release(existing);
    }
    if (node_rename(src, dst_dir, name, name_len) == 0) {
        ++mutation_count;
        rc = 0;
    }
out:
    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return rc;
}

void axys_vfs_init(void)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);

    vfs_init_nl();
    axys_spin_unlock_irqrestore(&vfs_lock, flags);
}

axys_vfs_node_t axys_vfs_lookup(const char *path)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_vfs_node_t result = vfs_lookup_nl(path);

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return result;
}

axys_vfs_node_t axys_vfs_create(const char *path, axys_vfs_type_t type)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    ++mutation_count;
    axys_vfs_node_t result = vfs_create_nl(path, type);

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return result;
}

axys_int32_t axys_vfs_mkdirs(const char *path)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    ++mutation_count;
    axys_int32_t result = vfs_mkdirs_nl(path);

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return result;
}

axys_int32_t axys_vfs_unlink(const char *path)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    ++mutation_count;
    axys_int32_t result = vfs_unlink_nl(path);

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return result;
}

/* Iterative post-order release of `node` and every descendant. A plain
 * unlink still refuses non-empty directories; this is the explicit recursive
 * removal used by rm -rf in user space. Recursion is deliberately avoided:
 * an attacker-controlled tree up to AXYS_VFS_MAX_NODES deep would overflow
 * the kernel stack (hardware fault / triple state), so the traversal keeps
 * its bookkeeping in a bounded heap scratch array instead. Returns the
 * number of nodes released, -1 if the start node is invalid/root, or -2 when
 * the scratch array cannot be allocated (the tree is left untouched). */
axys_int32_t axys_vfs_remove_tree(const char *path)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_vfs_node_t node = walk(path, 0, AXYS_VFS_DIR);
    axys_int32_t result = -1;

    if (node > 0) {
        /* Explicit stack of pending directories. Each entry is a directory
         * whose descendants are still being released. The traversal always
         * takes the LAST child of the top directory: removing the last slot
         * is a trivial pop that never moves the remaining children, so no
         * entry is ever skipped. (The previous forward-index walk called
         * node_unlink_from_parent(), which swap-removes an arbitrary slot;
         * with 10 children only 6 were freed and the rest leaked forever.) */
        axys_int32_t *stack = axys_kmalloc(AXYS_VFS_MAX_NODES * sizeof(*stack));
        axys_size_t top = 0;

        if (stack == AXYS_NULL) {
            result = -2; /* ENOMEM: refuse rather than half-remove anything */
        } else {
            axys_int32_t removed = 0;
            int failed = 0;

            stack[top++] = node;

            while (top != 0 && !failed) {
                axys_int32_t target = stack[top - 1];

                if (!node_valid(target)) {
                    /* stale reference inside our own stack: drop the frame */
                    --top;
                    continue;
                }
                if (nodes[target].child_count != 0) {
                    axys_int32_t child =
                        nodes[target].children[nodes[target].child_count - 1u];

                    if (!node_valid(child)) {
                        /* inconsistent tree: drop the stale link (it is the
                         * last slot, so no other child moves). */
                        --nodes[target].child_count;
                        continue;
                    }
                    if (nodes[child].type == AXYS_VFS_DIR &&
                        nodes[child].child_count != 0) {
                        if (top == AXYS_VFS_MAX_NODES) {
                            failed = 1; /* cannot happen: dirs <= nodes */
                            break;
                        }
                        stack[top++] = child;
                    } else {
                        node_unlink_from_parent(child);
                        node_release(child);
                        ++removed;
                    }
                    continue;
                }
                /* all children dispatched: release the directory itself
                 * (never the root, which cannot appear here since node > 0
                 * and no child index can be the root in a valid tree). */
                --top;
                node_unlink_from_parent(target);
                node_release(target);
                ++removed;
            }
            axys_kfree(stack);
            if (!failed) {
                ++mutation_count;
                result = removed;
            }
        }
    }
    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return result;
}

axys_int32_t axys_vfs_size(axys_vfs_node_t node)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_int32_t result = vfs_size_nl(node);

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return result;
}

axys_int32_t axys_vfs_pwrite(axys_vfs_node_t node, axys_size_t offset, const void *data, axys_size_t length)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    ++mutation_count;
    axys_int32_t result = vfs_pwrite_nl(node, offset, data, length);

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return result;
}

axys_int32_t axys_vfs_pread(axys_vfs_node_t node, axys_size_t offset, void *buffer, axys_size_t length)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_int32_t result = vfs_pread_nl(node, offset, buffer, length);

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return result;
}

axys_int32_t axys_vfs_write(axys_vfs_node_t node, const void *data, axys_size_t length)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    ++mutation_count;
    axys_int32_t result = vfs_write_nl(node, data, length);

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return result;
}

axys_int32_t axys_vfs_read(axys_vfs_node_t node, void *buffer, axys_size_t length)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_int32_t result = vfs_read_nl(node, buffer, length);

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return result;
}

axys_vfs_node_t axys_vfs_next_child(axys_vfs_node_t dir, axys_vfs_node_t child)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_vfs_node_t result = vfs_next_child_nl(dir, child);

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return result;
}

axys_vfs_node_t axys_vfs_create_as(const char *path, axys_vfs_type_t type, axys_uint32_t mode,
                                   axys_uint32_t uid, axys_uint32_t gid)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    ++mutation_count;
    axys_vfs_node_t node = type == AXYS_VFS_FILE || type == AXYS_VFS_DIR ? walk(path, 1, type) : -1;

    if (node >= 0) {
        nodes[node].mode = mode & 07777u;
        nodes[node].uid = uid;
        nodes[node].gid = gid;
    }
    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return node;
}

axys_int32_t axys_vfs_getattr(axys_vfs_node_t node, struct axys_vfs_attr *out)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_int32_t rc = -1;

    if (node_valid(node) && out != AXYS_NULL) {
        out->type = nodes[node].type == AXYS_VFS_DIR ? 2u : 1u;
        out->size = nodes[node].type == AXYS_VFS_DIR ? 0u : (axys_uint32_t)nodes[node].size;
        out->mode = nodes[node].mode;
        out->uid = nodes[node].uid;
        out->gid = nodes[node].gid;
        rc = 0;
    }
    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return rc;
}

axys_uint32_t axys_vfs_generation(axys_vfs_node_t node)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_uint32_t gen = node_valid(node) ? node_generation[node] : 0u;

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return gen;
}

axys_uint64_t axys_vfs_live_nodes(void)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_uint64_t live = live_nodes;

    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return live;
}

axys_int32_t axys_vfs_chmod(axys_vfs_node_t node, axys_uint32_t mode)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    ++mutation_count;
    axys_int32_t rc = -1;

    if (node_valid(node)) {
        nodes[node].mode = mode & 07777u;
        rc = 0;
    }
    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return rc;
}

axys_int32_t axys_vfs_chown(axys_vfs_node_t node, axys_uint32_t uid, axys_uint32_t gid)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    ++mutation_count;
    axys_int32_t rc = -1;

    if (node_valid(node)) {
        nodes[node].uid = uid;
        nodes[node].gid = gid;
        rc = 0;
    }
    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return rc;
}

axys_int32_t axys_vfs_access(axys_vfs_node_t node, axys_uint32_t uid, axys_uint32_t gid, axys_uint32_t want)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&vfs_lock);
    axys_int32_t rc = -1;

    if (node_valid(node)) {
        axys_uint32_t mode = nodes[node].mode;
        axys_uint32_t have;

        if (uid == 0) {
            have = AXYS_PERM_R | AXYS_PERM_W;
            if (nodes[node].type == AXYS_VFS_DIR || (mode & 0111u) != 0) {
                have |= AXYS_PERM_X;
            }
        } else if (uid == nodes[node].uid) {
            have = (mode >> 6) & 7u;
        } else if (gid == nodes[node].gid) {
            have = (mode >> 3) & 7u;
        } else {
            have = mode & 7u;
        }
        rc = (have & want) == want ? 0 : -1;
    }
    axys_spin_unlock_irqrestore(&vfs_lock, flags);
    return rc;
}

static void dump_recursive(axys_vfs_node_t node, unsigned depth)
{
    axys_vfs_node_t child = -1;

    for (unsigned i = 0; i < depth; ++i) {
        axys_console_write("  ");
    }
    if (node == 0) {
        axys_console_write("/\n");
    } else {
        char name[AXYS_VFS_NAME_MAX];

        if (axys_vfs_name_copy(node, name, sizeof(name)) < 0) {
            axys_console_write("?\n");
        } else {
            axys_printf("%s%s\n", name, axys_vfs_type(node) == AXYS_VFS_DIR ? "/" : "");
        }
    }
    while ((child = axys_vfs_next_child(node, child)) >= 0) {
        dump_recursive(child, depth + 1);
    }
}

void axys_vfs_dump(void)
{
    dump_recursive(0, 0);
}

void axys_vfs_selftest(void)
{
    axys_vfs_node_t hostname;
    axys_vfs_node_t big;
    static const char msg[] = "axys\n";
    char readback[8];
    static const char tail[] = "END";
    char probe[3];

    if (axys_vfs_lookup("/") != 0) {
        axys_panic("vfs: root lookup failed");
    }
    if (axys_vfs_lookup("/etc") < 0 || axys_vfs_lookup("/home") < 0 || axys_vfs_lookup("/bin") < 0) {
        axys_panic("vfs: standard hierarchy missing");
    }
    if (axys_vfs_lookup("/does/not/exist") >= 0) {
        axys_panic("vfs: lookup accepted a missing path");
    }
    if (axys_vfs_create("/", AXYS_VFS_DIR) >= 0 || axys_vfs_create("/etc/", AXYS_VFS_FILE) >= 0) {
        axys_panic("vfs: create accepted an existing path spelling");
    }
    if (axys_vfs_create("/etc/hostname", AXYS_VFS_FILE) < 0) {
        axys_panic("vfs: create file failed");
    }
    if (axys_vfs_create("/etc/hostname", AXYS_VFS_FILE) >= 0) {
        axys_panic("vfs: create allowed a duplicate");
    }
    if (axys_vfs_create("/home/user", AXYS_VFS_DIR) < 0) {
        axys_panic("vfs: create directory failed");
    }
    if (axys_vfs_create("/no/such/dir/file", AXYS_VFS_FILE) >= 0) {
        axys_panic("vfs: create allowed a missing parent");
    }

    hostname = axys_vfs_lookup("/etc/hostname");
    if (axys_vfs_write(hostname, msg, sizeof(msg) - 1u) != (axys_int32_t)(sizeof(msg) - 1u)) {
        axys_panic("vfs: write returned wrong length");
    }
    axys_memset(readback, 0, sizeof(readback));
    if (axys_vfs_read(hostname, readback, sizeof(readback)) != (axys_int32_t)(sizeof(msg) - 1u) ||
        axys_memcmp(readback, msg, sizeof(msg) - 1u) != 0) {
        axys_panic("vfs: read data did not match write");
    }
    if (axys_vfs_write(0, msg, sizeof(msg)) >= 0) {
        axys_panic("vfs: write accepted a directory node");
    }

    /* Dynamic growth: a 100 KiB file, sparse write, read at offset. */
    big = axys_vfs_create("/tmp/big", AXYS_VFS_FILE);
    if (big < 0 || axys_vfs_pwrite(big, 100u * 1024u, tail, sizeof(tail)) != (axys_int32_t)sizeof(tail) ||
        axys_vfs_size(big) != (axys_int32_t)(100u * 1024u + sizeof(tail))) {
        axys_panic("vfs: large sparse write failed");
    }
    if (axys_vfs_pread(big, 100u * 1024u, probe, sizeof(probe)) != 3 || probe[0] != 'E' ||
        axys_vfs_pread(big, 5u, probe, sizeof(probe)) != 3 || probe[0] != 0) {
        axys_panic("vfs: large file readback failed");
    }
    if (axys_vfs_pread(big, 1024u * 1024u, probe, 1) != 0) {
        axys_panic("vfs: read past EOF returned data");
    }

    /* Directories beyond the old 32-child cap, then unlink + node reuse. */
    for (int i = 0; i < 100; ++i) {
        char path[32];

        axys_snprintf(path, sizeof(path), "/var/f%d", i);
        if (axys_vfs_create(path, AXYS_VFS_FILE) < 0) {
            axys_panic("vfs: directory refused a 100th child");
        }
    }
    if (axys_vfs_unlink("/tmp/big") != 0 || axys_vfs_lookup("/tmp/big") >= 0 ||
        axys_vfs_unlink("/home") == 0 || axys_vfs_unlink("/") == 0) {
        axys_panic("vfs: unlink misbehaved");
    }
    for (int i = 0; i < 100; ++i) {
        char path[32];

        axys_snprintf(path, sizeof(path), "/var/f%d", i);
        if (axys_vfs_unlink(path) != 0) {
            axys_panic("vfs: unlink of created file failed");
        }
    }
    if (axys_vfs_mkdirs("/usr/local/share/axys") != 0 || axys_vfs_lookup("/usr/local/share/axys") < 0) {
        axys_panic("vfs: mkdirs failed");
    }

    /* The selftest runs on the live tree, not on a scratch copy: everything
     * it created above must be removed again, otherwise /etc/hostname keeps a
     * test payload (hiding the real default installed later) and /home/user
     * stays owned by uid 0 instead of the 1000:1000 owner that
     * axys_initrd_install_defaults() would assign on a clean tree. */
    if (axys_vfs_unlink("/etc/hostname") != 0) {
        axys_panic("vfs: selftest cleanup of /etc/hostname failed");
    }
    if (axys_vfs_unlink("/home/user") != 0) {
        axys_panic("vfs: selftest cleanup of /home/user failed");
    }
    if (axys_vfs_remove_tree("/usr/local") < 0) {
        axys_panic("vfs: selftest cleanup of /usr/local failed");
    }

    axys_printf("vfs: selftest passed (%u live nodes)\n", live_nodes);
}
