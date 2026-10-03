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

    if (path_len == 0 || path_len > MAX_PATH_LEN || axys_vfs_getattr(node, &attr) != 0) {
        return;
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

static void serialize_tree(struct buffer *b, const char *path, axys_vfs_node_t dir, unsigned depth)
{
    axys_vfs_node_t child = -1;

    if (depth > 16) {
        return;
    }
    while ((child = axys_vfs_next_child(dir, child)) >= 0) {
        char child_path[MAX_PATH_LEN + 1];
        axys_size_t pl = axys_strlen(path);
        const char *name = axys_vfs_name(child);

        if (pl + axys_strlen(name) + 2 > sizeof(child_path)) {
            continue;
        }
        axys_memcpy(child_path, path, pl);
        if (pl > 1) {
            child_path[pl++] = '/';
        }
        axys_strlcpy(child_path + pl, name, sizeof(child_path) - pl);
        if (!path_persisted(child_path)) {
            continue;
        }
        serialize_node(b, child_path, child);
        if (axys_vfs_type(child) == AXYS_VFS_DIR) {
            serialize_tree(b, child_path, child, depth + 1);
        }
    }
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
        serialize_tree(b, persist_roots[i], root, 0);
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
        axys_crc32(0, payload, len) == h->payload_crc) {
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
