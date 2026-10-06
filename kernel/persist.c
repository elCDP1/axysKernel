#include "axys/persist.h"
#include "axys/disk.h"
#include "axys/heap.h"
#include "axys/printf.h"
#include "axys/string.h"
#include "axys/vfs.h"
#ifndef AXYS_HOST_TEST
#include "axys/sched.h"
#include "axys/spinlock.h"
#endif

#define SB_MAGIC "AXYSFS1"
#define SNAP_MAGIC "AXSNAP01"
#define MAX_SLOT_SECTORS 8192u /* 4 MiB of snapshot per slot */
#define MIN_SLOT_SECTORS 16u
#define MAX_PATH_LEN 255u
#define FLUSH_INTERVAL_MS 5000u

struct sb_sector {
    char magic[8];
    axys_uint32_t slot_sectors;
    axys_uint32_t version;
    axys_uint8_t pad[496];
} __attribute__((packed));

struct snap_header {
    char magic[8];
    axys_uint64_t generation;
    axys_uint32_t payload_len;
    axys_uint32_t payload_crc;
    axys_uint32_t header_crc; /* over the preceding 24 bytes */
    axys_uint8_t pad[484];
} __attribute__((packed));

struct record {
    axys_uint16_t path_len; /* 0 terminates the stream */
    axys_uint8_t type;      /* 1 file, 2 directory */
    axys_uint8_t reserved;
    axys_uint32_t mode;
    axys_uint32_t uid;
    axys_uint32_t gid;
    axys_uint32_t size;
} __attribute__((packed));

_Static_assert(sizeof(struct sb_sector) == 512, "superblock is one sector");
_Static_assert(sizeof(struct snap_header) == 512, "snapshot header is one sector");

static const char *const persist_roots[] = {"/etc", "/home", "/root", "/var", "/srv", "/opt", "/usr", "/mnt", "/media"};

static int disk_ok;
static axys_uint32_t slot_sectors;
static axys_uint64_t generation;
static int active_slot = -1; /* slot holding the newest valid snapshot */
static axys_uint64_t synced_mutations;
#ifndef AXYS_HOST_TEST
static int sync_busy;
static struct axys_spinlock sync_lock;
#endif

/* ---- CRC-32 -------------------------------------------------------- */

axys_uint32_t axys_crc32(axys_uint32_t crc, const void *data, axys_size_t length)
{
    static axys_uint32_t table[256];
    static int ready;
    const axys_uint8_t *p = (const axys_uint8_t *)data;

    if (!ready) {
        for (axys_uint32_t n = 0; n < 256; ++n) {
            axys_uint32_t c = n;

            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = 1;
    }
    crc = ~crc;
    while (length--) {
        crc = table[(crc ^ *p++) & 0xffu] ^ (crc >> 8);
    }
    return ~crc;
}

/* ---- helpers ------------------------------------------------------- */

static int path_persisted(const char *path)
{
    for (axys_size_t i = 0; i < AXYS_ARRAY_SIZE(persist_roots); ++i) {
        axys_size_t n = axys_strlen(persist_roots[i]);

        if (axys_strncmp(path, persist_roots[i], n) == 0 && (path[n] == '\0' || path[n] == '/')) {
            return 1;
        }
    }
    return 0;
}

static axys_uint32_t slot_start(int slot)
{
    return 1u + (axys_uint32_t)slot * slot_sectors;
}

struct buffer {
    axys_uint8_t *data;
    axys_size_t len;
    axys_size_t cap;
    int failed;
};

static void buf_append(struct buffer *b, const void *src, axys_size_t n)
{
    axys_size_t limit = (axys_size_t)(slot_sectors - 1u) * AXYS_SECTOR_SIZE;

    if (b->failed) {
        return;
    }
    if (b->len + n > limit) {
        b->failed = 1; /* snapshot would not fit in a slot */
        return;
    }
    if (b->len + n > b->cap) {
        axys_size_t new_cap = b->cap ? b->cap * 2u : 4096u;
        axys_uint8_t *grown;

        while (new_cap < b->len + n) {
            new_cap *= 2u;
        }
        grown = axys_kmalloc(new_cap);
        if (grown == AXYS_NULL) {
            b->failed = 1;
            return;
        }
        if (b->data != AXYS_NULL) {
            axys_memcpy(grown, b->data, b->len);
            axys_kfree(b->data);
        }
        b->data = grown;
        b->cap = new_cap;
    }
    axys_memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void serialize_node(struct buffer *b, const char *path, axys_vfs_node_t node)
{
    struct axys_vfs_attr attr;
    struct record rec;
    axys_size_t path_len = axys_strlen(path);

    if (path_len == 0 || path_len > MAX_PATH_LEN) {
        /* Unreachable through the VFS path ceiling, but dropping a node here
         * would write a snapshot that silently loses it: fail the sync. */
        b->failed = 1;
        return;
    }
    if (axys_vfs_getattr(node, &attr) != 0) {
        return; /* node vanished between lookup and save: nothing to record */
    }
    rec.path_len = (axys_uint16_t)path_len;
    rec.type = (axys_uint8_t)attr.type;
    rec.reserved = 0;
    rec.mode = attr.mode;
    rec.uid = attr.uid;
    rec.gid = attr.gid;
    rec.size = attr.type == 1u ? attr.size : 0u;
    buf_append(b, &rec, sizeof(rec));
    buf_append(b, path, path_len);
    if (attr.type == 1u && attr.size != 0) {
        /* Read in chunks so the file may change size between calls without
         * ever overrunning our buffer: we copy exactly rec.size bytes. */
        axys_uint8_t chunk[256];
        axys_uint32_t off = 0;

        while (off < attr.size) {
            axys_size_t want = attr.size - off < sizeof(chunk) ? attr.size - off : sizeof(chunk);
            axys_int32_t got = axys_vfs_pread(node, off, chunk, want);

            if (got <= 0) {
                axys_memset(chunk, 0, want); /* file shrank under us: pad */
                got = (axys_int32_t)want;
            }
            buf_append(b, chunk, (axys_size_t)got);
            off += (axys_uint32_t)got;
        }
    }
}

static void serialize_tree(struct buffer *b, const char *path, axys_vfs_node_t dir)
{
    /* Explicit stack instead of recursion: tree depth is attacker-influenced
     * (nested mkdirs), so recursion risks a kernel stack overflow, and the
     * old `depth > 16 → return` silently dropped every deeper subtree while
     * still reporting sync success (data loss with no error). Depth here is
     * naturally bounded by the 255-byte path ceiling (~127 levels of 1-char
     * names); the 512-entry cap is unreachable defense that fails the sync
     * loudly instead of losing data quietly. */
    struct ser_frame {
        axys_vfs_node_t dir;
        axys_vfs_node_t child;
        axys_size_t path_len;
    };
    enum { SER_STACK_CAP = 512 };
    char cur[MAX_PATH_LEN + 1];
    char name_buf[AXYS_VFS_NAME_MAX];
    axys_size_t len = axys_strlen(path);
    struct ser_frame *stack;
    axys_size_t top = 0;

    if (len >= sizeof(cur)) {
        b->failed = 1; /* unreachable with fixed persist roots: fail loud */
        return;
    }
    axys_memcpy(cur, path, len + 1);
    stack = axys_kmalloc(SER_STACK_CAP * sizeof(*stack));
    if (stack == AXYS_NULL) {
        b->failed = 1;
        return;
    }
    stack[top].dir = dir;
    stack[top].child = -1;
    stack[top].path_len = len;
    ++top;
    while (top != 0 && !b->failed) {
        struct ser_frame *f = &stack[top - 1];
        axys_vfs_node_t child = axys_vfs_next_child(f->dir, f->child);
        char child_path[MAX_PATH_LEN + 1];
        axys_size_t pl;
        axys_size_t nl;
        axys_size_t cl;

        if (child < 0) {
            --top;
            if (top != 0) {
                cur[stack[top - 1].path_len] = '\0';
            }
            continue;
        }
        f->child = child;
        pl = f->path_len;
        if (axys_vfs_name_copy(child, name_buf, sizeof(name_buf)) < 0) {
            continue; /* node went away under us: skip it */
        }
        nl = axys_strlen(name_buf);
        if (pl + nl + 2 > sizeof(child_path)) {
            /* Unreachable (VFS paths are bounded the same way), but skipping
             * would persist a tree that is missing this subtree: fail loud. */
            b->failed = 1;
            break;
        }
        axys_memcpy(child_path, cur, pl);
        cl = pl;
        if (cl > 1) {
            child_path[cl++] = '/';
        }
        axys_strlcpy(child_path + cl, name_buf, sizeof(child_path) - cl);
        if (!path_persisted(child_path)) {
            continue;
        }
        serialize_node(b, child_path, child);
        if (b->failed) {
            break;
        }
        if (axys_vfs_type(child) == AXYS_VFS_DIR) {
            if (top == SER_STACK_CAP) {
                b->failed = 1; /* unreachable via the path ceiling; fail loud */
                break;
            }
            axys_memcpy(cur, child_path, sizeof(cur));
            stack[top].dir = child;
            stack[top].child = -1;
            stack[top].path_len = axys_strlen(child_path);
            ++top;
        }
    }
    axys_kfree(stack);
}

/* Persisted roots that do not exist yet (they always do after vfs_init) are
 * saved via the recursion below; start at "/". */
static void serialize_all(struct buffer *b)
{
    static const struct record end = {0, 0, 0, 0, 0, 0, 0};

    for (axys_size_t i = 0; i < AXYS_ARRAY_SIZE(persist_roots); ++i) {
        axys_vfs_node_t root = axys_vfs_lookup(persist_roots[i]);

        if (root < 0) {
            continue;
        }
        serialize_node(b, persist_roots[i], root);
        serialize_tree(b, persist_roots[i], root);
    }
    buf_append(b, &end, sizeof(end));
}

static int read_header(int slot, struct snap_header *h)
{
    axys_uint32_t crc;

    if (axys_disk_read(slot_start(slot), 1, h) != 0 || axys_memcmp(h->magic, SNAP_MAGIC, 8) != 0) {
        return -1;
    }
    crc = axys_crc32(0, h, 24);
    if (crc != h->header_crc || h->payload_len > (axys_uint32_t)(slot_sectors - 1u) * AXYS_SECTOR_SIZE) {
        return -1;
    }
    return 0;
}

/* Restore one payload into the VFS. Returns the number of files restored or -1
 * on a malformed stream (nothing after the bad record is applied). */

struct seen_dir {
    axys_size_t off;
    axys_size_t len;
};

/* True when `parent` (exact bytes, not NUL-terminated) names a directory that
 * applying the payload so far guarantees: a live directory, or one of the
 * earlier DIR records. */
static int dir_available(const axys_uint8_t *payload, const struct seen_dir *seen, axys_size_t nseen,
                         const char *parent)
{
    char tmp[MAX_PATH_LEN + 1];
    axys_size_t plen = axys_strlen(parent);

    if (plen == 0 || plen > MAX_PATH_LEN) {
        return 0;
    }
    axys_memcpy(tmp, parent, plen + 1);
    if (axys_vfs_lookup(tmp) >= 0 && axys_vfs_type(axys_vfs_lookup(tmp)) == AXYS_VFS_DIR) {
        return 1;
    }
    for (axys_size_t i = 0; i < nseen; ++i) {
        if (seen[i].len == plen && axys_memcmp(payload + seen[i].off, parent, plen) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Every component must satisfy the same rules walk() enforces: non-empty,
 * shorter than AXYS_VFS_NAME_MAX, and never "." or "..". */
static int components_valid(const char *path, axys_size_t path_len)
{
    axys_size_t i = 1; /* skip the leading '/' */

    if (path_len == 0 || path[0] != '/') {
        return 0;
    }
    while (i < path_len) {
        axys_size_t start = i;
        axys_size_t seg_len = 0;

        while (i < path_len && path[i] != '/') {
            ++i;
            ++seg_len;
        }
        if (seg_len == 0 || seg_len >= AXYS_VFS_NAME_MAX) {
            return 0;
        }
        if ((seg_len == 1 && path[start] == '.') ||
            (seg_len == 2 && path[start] == '.' && path[start + 1] == '.')) {
            return 0;
        }
        if (i < path_len) {
            ++i; /* skip '/' */
        }
    }
    return 1;
}

/* Read-only validation pass over a snapshot payload (audit P0: transactional
 * restore). Returns 0 when every record is well-formed AND applicable to the
 * current VFS without mutation: framing, persisted-roots confinement, name
 * rules, 128 MiB file cap, live type conflicts, parent availability through
 * earlier DIR records, and node-budget fit. Returns -1 otherwise, leaving the
 * VFS untouched so the caller falls back to the older slot on a pristine
 * tree. Anything this accepts, apply_payload() below is guaranteed to apply
 * short of heap/pool exhaustion at runtime. */
static int validate_payload(const axys_uint8_t *p, axys_size_t len)
{
    axys_size_t off = 0;
    struct seen_dir *seen = AXYS_NULL;
    axys_size_t nseen = 0;
    axys_size_t seencap = 0;
    axys_uint64_t need = 0;
    int rc = -1;

    for (;;) {
        struct record rec;
        char path[MAX_PATH_LEN + 1];
        char parent[MAX_PATH_LEN + 1];
        axys_size_t plen;
        axys_vfs_node_t node;

        if (len - off < sizeof(rec)) {
            goto out;
        }
        axys_memcpy(&rec, p + off, sizeof(rec));
        off += sizeof(rec);
        if (rec.path_len == 0) {
            rc = 0; /* clean end of stream */
            goto out;
        }
        if (rec.path_len > MAX_PATH_LEN || len - off < rec.path_len ||
            (rec.type != 1 && rec.type != 2)) {
            goto out;
        }
        if (rec.type == 1 &&
            (len - off - rec.path_len < rec.size || rec.size > AXYS_VFS_MAX_FILE_BYTES)) {
            goto out;
        }
        axys_memcpy(path, p + off, rec.path_len);
        path[rec.path_len] = '\0';
        off += rec.path_len;
        if (rec.type == 1) {
            off += rec.size;
        }
        plen = rec.path_len;
        if (!components_valid(path, plen) || plen >= AXYS_VFS_MAX_PATH || !path_persisted(path)) {
            goto out; /* not absolute, not confined, or not walkable */
        }
        /* Parent availability: "/" always exists; otherwise a live directory
         * or an earlier DIR record of this same payload. */
        {
            axys_size_t slash = 0;

            for (axys_size_t i = 0; i < plen; ++i) {
                if (path[i] == '/') {
                    slash = i;
                }
            }
            if (slash == 0) {
                axys_memcpy(parent, "/", 2);
            } else {
                if (slash > sizeof(parent) - 1) {
                    goto out;
                }
                axys_memcpy(parent, path, slash);
                parent[slash] = '\0';
            }
        }
        if (!dir_available(p, seen, nseen, parent)) {
            goto out;
        }
        node = axys_vfs_lookup(path);
        if (rec.type == 2) {
            if (node >= 0 && axys_vfs_type(node) != AXYS_VFS_DIR) {
                goto out; /* dir record over a live file can never apply */
            }
            if (node < 0) {
                if (nseen == seencap) {
                    axys_size_t ncap = seencap ? seencap * 2u : 16u;
                    struct seen_dir *grown = axys_kmalloc(ncap * sizeof(*grown));

                    if (grown == AXYS_NULL) {
                        goto out;
                    }
                    if (seen != AXYS_NULL) {
                        axys_memcpy(grown, seen, nseen * sizeof(*grown));
                        axys_kfree(seen);
                    }
                    seen = grown;
                    seencap = ncap;
                }
                seen[nseen].off = off - (rec.type == 1 ? rec.size : 0u) - plen;
                seen[nseen].len = plen;
                ++nseen;
                ++need;
            }
        } else {
            if (node >= 0 && axys_vfs_type(node) != AXYS_VFS_FILE) {
                goto out; /* file record over a live directory can never apply */
            }
            if (node < 0) {
                ++need;
            }
        }
    }
out:
    axys_kfree(seen);
    if (rc == 0 && need > AXYS_VFS_MAX_NODES - axys_vfs_live_nodes()) {
        rc = -1; /* would exhaust the node pool mid-apply: refuse upfront */
    }
    return rc;
}

/* Restore a payload that validate_payload() already accepted. Still
 * defensive (returns -1 on any surprise), but the only failures left here
 * are runtime resource exhaustion, never malformed input. */
static int apply_payload(const axys_uint8_t *p, axys_size_t len)
{
    axys_size_t off = 0;
    int files = 0;

    for (;;) {
        struct record rec;
        char path[MAX_PATH_LEN + 1];
        axys_vfs_node_t node;

        if (len - off < sizeof(rec)) {
            return -1;
        }
        axys_memcpy(&rec, p + off, sizeof(rec));
        off += sizeof(rec);
        if (rec.path_len == 0) {
            return files; /* clean end of stream */
        }
        if (rec.path_len > MAX_PATH_LEN || len - off < rec.path_len ||
            len - off - rec.path_len < (rec.type == 1 ? rec.size : 0u) || (rec.type != 1 && rec.type != 2)) {
            return -1;
        }
        axys_memcpy(path, p + off, rec.path_len);
        path[rec.path_len] = '\0';
        off += rec.path_len;
        if (path[0] != '/' || !path_persisted(path)) {
            return -1; /* a snapshot may only touch user-data paths */
        }
        node = axys_vfs_lookup(path);
        if (rec.type == 2) {
            if (node < 0) {
                node = axys_vfs_create_as(path, AXYS_VFS_DIR, rec.mode, rec.uid, rec.gid);
            } else if (axys_vfs_type(node) == AXYS_VFS_DIR) {
                (void)axys_vfs_chmod(node, rec.mode);
                (void)axys_vfs_chown(node, rec.uid, rec.gid);
            } else {
                return -1; /* dir record over a live file: refuse, like validation */
            }
            if (node < 0) {
                return -1;
            }
        } else {
            if (node < 0) {
                node = axys_vfs_create_as(path, AXYS_VFS_FILE, rec.mode, rec.uid, rec.gid);
            } else if (axys_vfs_type(node) == AXYS_VFS_FILE) {
                (void)axys_vfs_chmod(node, rec.mode);
                (void)axys_vfs_chown(node, rec.uid, rec.gid);
            } else {
                return -1;
            }
            if (node < 0 || axys_vfs_write(node, p + off, rec.size) < 0) {
                return -1;
            }
            off += rec.size;
            ++files;
        }
    }
}

static int load_slot(int slot, const struct snap_header *h)
{
    axys_size_t len = h->payload_len;
    axys_uint32_t sectors = (axys_uint32_t)((len + AXYS_SECTOR_SIZE - 1u) / AXYS_SECTOR_SIZE);
    axys_uint8_t *payload;
    int rc = -1;

    if (len == 0 || sectors == 0) {
        return -1;
    }
    payload = axys_kmalloc((axys_size_t)sectors * AXYS_SECTOR_SIZE);
    if (payload == AXYS_NULL) {
        return -1;
    }
    if (axys_disk_read(slot_start(slot) + 1u, sectors, payload) == 0 &&
        axys_crc32(0, payload, len) == h->payload_crc &&
        validate_payload(payload, len) == 0) {
        rc = apply_payload(payload, len);
    }
    axys_kfree(payload);
    return rc;
}

int axys_persist_available(void)
{
    return disk_ok;
}

axys_uint64_t axys_persist_generation(void)
{
    return generation;
}

int axys_persist_init(void)
{
    struct sb_sector sb;
    axys_uint32_t sectors;
    struct snap_header h[2];
    int valid[2];
    int pick = -1;
    int restored = 0;

    disk_ok = 0;
    active_slot = -1;
    generation = 0;
    if (axys_disk_init() != 0) {
        return -1;
    }
    sectors = axys_disk_sectors();
    if (sectors < 1u + 2u * MIN_SLOT_SECTORS || axys_disk_read(0, 1, &sb) != 0) {
        return -1;
    }
    slot_sectors = (sectors - 1u) / 2u;
    if (slot_sectors > MAX_SLOT_SECTORS) {
        slot_sectors = MAX_SLOT_SECTORS;
    }

    if (axys_memcmp(sb.magic, SB_MAGIC, 8) == 0) {
        if (sb.slot_sectors < MIN_SLOT_SECTORS || sb.slot_sectors > slot_sectors) {
            return -2; /* written for a different geometry */
        }
        slot_sectors = sb.slot_sectors;
    } else {
        /* Fresh disk only: refuse to format anything that holds data. */
        axys_uint8_t *raw = (axys_uint8_t *)&sb;

        for (axys_size_t i = 0; i < sizeof(sb); ++i) {
            if (raw[i] != 0) {
                return -2;
            }
        }
        axys_memset(&sb, 0, sizeof(sb));
        axys_memcpy(sb.magic, SB_MAGIC, 8);
        sb.slot_sectors = slot_sectors;
        sb.version = 1;
        if (axys_disk_write(0, 1, &sb) != 0 || axys_disk_flush() != 0) {
            return -1;
        }
    }
    disk_ok = 1;

    for (int s = 0; s < 2; ++s) {
        valid[s] = read_header(s, &h[s]) == 0;
    }
    if (valid[0] && (!valid[1] || h[0].generation >= h[1].generation)) {
        pick = 0;
    } else if (valid[1]) {
        pick = 1;
    }
    /* Try the newest snapshot, then fall back to the older one if its payload
     * is damaged (torn write). */
    for (int attempt = 0; attempt < 2 && pick >= 0; ++attempt) {
        int rc = load_slot(pick, &h[pick]);

        if (rc >= 0) {
            restored = rc;
            generation = h[pick].generation;
            active_slot = pick;
            break;
        }
        pick = (pick == 0 && valid[1]) ? 1 : (pick == 1 && valid[0]) ? 0 : -1;
    }
    synced_mutations = axys_vfs_mutations();
    return restored;
}

int axys_persist_sync(void)
{
    struct buffer b = {AXYS_NULL, 0, 0, 0};
    struct snap_header h;
    axys_uint32_t sectors;
    axys_uint8_t *padded;
    int target;
    int rc = -1;
    axys_uint64_t mutations_before;

    if (!disk_ok) {
        return -1;
    }
#ifndef AXYS_HOST_TEST
    for (;;) { /* one sync at a time */
        axys_uint64_t flags = axys_spin_lock_irqsave(&sync_lock);

        if (!sync_busy) {
            sync_busy = 1;
            axys_spin_unlock_irqrestore(&sync_lock, flags);
            break;
        }
        axys_spin_unlock_irqrestore(&sync_lock, flags);
        axys_yield();
    }
#endif
    mutations_before = axys_vfs_mutations();
    serialize_all(&b);
    if (b.failed) {
        goto out;
    }
    sectors = (axys_uint32_t)((b.len + AXYS_SECTOR_SIZE - 1u) / AXYS_SECTOR_SIZE);
    padded = axys_kzalloc((axys_size_t)sectors * AXYS_SECTOR_SIZE);
    if (padded == AXYS_NULL) {
        goto out;
    }
    axys_memcpy(padded, b.data, b.len);

    target = active_slot == 0 ? 1 : 0;
    axys_memset(&h, 0, sizeof(h));
    axys_memcpy(h.magic, SNAP_MAGIC, 8);
    h.generation = generation + 1;
    h.payload_len = (axys_uint32_t)b.len;
    h.payload_crc = axys_crc32(0, b.data, b.len);
    h.header_crc = axys_crc32(0, &h, 24);

    /* Payload first, then the header that makes it valid. */
    if (axys_disk_write(slot_start(target) + 1u, sectors, padded) == 0 && axys_disk_flush() == 0 &&
        axys_disk_write(slot_start(target), 1, &h) == 0 && axys_disk_flush() == 0) {
        generation = h.generation;
        active_slot = target;
        synced_mutations = mutations_before;
        rc = 0;
    }
    axys_kfree(padded);
out:
    axys_kfree(b.data);
#ifndef AXYS_HOST_TEST
    {
        axys_uint64_t flags = axys_spin_lock_irqsave(&sync_lock);

        sync_busy = 0;
        axys_spin_unlock_irqrestore(&sync_lock, flags);
    }
#endif
    return rc;
}

#ifndef AXYS_HOST_TEST
static void flusher(void *arg)
{
    (void)arg;
    for (;;) {
        axys_task_sleep_ms(FLUSH_INTERVAL_MS);
        if (disk_ok && axys_vfs_mutations() != synced_mutations) {
            (void)axys_persist_sync();
        }
    }
}

void axys_persist_start_flusher(void)
{
    struct axys_task *task;

    if (!disk_ok) {
        return;
    }
    task = axys_task_create("flushd", flusher, AXYS_NULL);
    if (task != AXYS_NULL) {
        axys_task_detach(task);
    }
}
#endif
