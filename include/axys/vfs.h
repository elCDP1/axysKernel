#ifndef AXYS_VFS_H
#define AXYS_VFS_H

#include "axys/types.h"

/* Minimal in-memory virtual filesystem.
 *
 * axysOS targets the same root layout as Linux (FHS-style): a single tree
 * rooted at "/" with /bin, /etc, /home, ... underneath it. Real backing
 * stores (disk, initramfs) mount onto this tree later; for now every node
 * lives in a small static pool so the kernel has a working namespace with
 * zero heap/allocator dependency.
 */

#define AXYS_VFS_NAME_MAX 32
#define AXYS_VFS_MAX_NODES 1024
#define AXYS_VFS_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define AXYS_VFS_MAX_PATH 256

typedef enum {
    AXYS_VFS_DIR = 0,
    AXYS_VFS_FILE = 1,
} axys_vfs_type_t;

typedef axys_int32_t axys_vfs_node_t; /* index into the node pool, -1 = none */

/* Bring up "/" and populate the standard Linux-style top-level directories
 * (/bin /boot /dev /etc /home /lib /media /mnt /opt /proc /root /run /sbin
 * /srv /sys /tmp /usr /var). Must be called once before any other VFS call. */
void axys_vfs_init(void);

/* Resolve an absolute path ("/", "/etc", "/home/user/file"). Returns -1 if
 * any component is missing. */
axys_vfs_node_t axys_vfs_lookup(const char *path);

/* Create a directory or empty file at `path`; the parent must already exist.
 * Returns the new node, or -1 on failure (parent missing, pool full, name
 * too long, or the entry already exists). */
axys_vfs_node_t axys_vfs_create(const char *path, axys_vfs_type_t type);

/* Replace/read the whole contents of a file node from offset 0. Returns the byte count written/read, or -1 on a
 * type mismatch (e.g. reading a directory). */
axys_int32_t axys_vfs_write(axys_vfs_node_t node, const void *data, axys_size_t length);
axys_int32_t axys_vfs_read(axys_vfs_node_t node, void *buffer, axys_size_t length);

/* Slot generation of a live node, 0 if `node` is not live. A node index alone is
 * not a stable identity: freed slots are reused. Anything that keeps an index
 * across calls must also keep this value and re-check it before use. */
axys_uint32_t axys_vfs_generation(axys_vfs_node_t node);

const char *axys_vfs_name(axys_vfs_node_t node);
axys_vfs_type_t axys_vfs_type(axys_vfs_node_t node);

/* Iterate children of a directory node: pass child = -1 to get the first
 * child, then the previous return value to get the next one. Returns -1
 * once there are no more children. */
axys_vfs_node_t axys_vfs_next_child(axys_vfs_node_t dir, axys_vfs_node_t child);

/* Directory/file management beyond create. */
axys_int32_t axys_vfs_mkdirs(const char *path);      /* mkdir -p: 0 or -1 */
axys_int32_t axys_vfs_unlink(const char *path);      /* file or empty dir: 0 or -1 */
axys_int32_t axys_vfs_size(axys_vfs_node_t node);    /* file size, -1 for dirs/invalid */

/* Positional I/O. pwrite grows the file (zero-filling any gap) up to
 * AXYS_VFS_MAX_FILE_BYTES; pread returns 0 at/after end of file. Both return
 * the byte count or -1 on a bad node/type/argument/out-of-memory. */
axys_int32_t axys_vfs_pread(axys_vfs_node_t node, axys_size_t offset, void *buffer, axys_size_t length);
axys_int32_t axys_vfs_pwrite(axys_vfs_node_t node, axys_size_t offset, const void *data, axys_size_t length);

/* ---- ownership and permissions (POSIX-style rwx for owner/group/other) ---- */
#define AXYS_PERM_R 4u
#define AXYS_PERM_W 2u
#define AXYS_PERM_X 1u

struct axys_vfs_attr {
    axys_uint32_t type; /* 1 = file, 2 = directory */
    axys_uint32_t size;
    axys_uint32_t mode; /* permission bits (07777) */
    axys_uint32_t uid;
    axys_uint32_t gid;
};

/* create + set owner and mode in one step (axys_vfs_create makes root:root,
 * 0755 for directories and 0644 for files). */
axys_vfs_node_t axys_vfs_create_as(const char *path, axys_vfs_type_t type, axys_uint32_t mode,
                                   axys_uint32_t uid, axys_uint32_t gid);
axys_int32_t axys_vfs_getattr(axys_vfs_node_t node, struct axys_vfs_attr *out);
axys_int32_t axys_vfs_chmod(axys_vfs_node_t node, axys_uint32_t mode);
axys_int32_t axys_vfs_chown(axys_vfs_node_t node, axys_uint32_t uid, axys_uint32_t gid);

/* May (uid, gid) perform every access in `want` (AXYS_PERM_* bits) on `node`?
 * Returns 0 if allowed, -1 if not. Root may read/write anything but may only
 * execute files that have at least one execute bit set. */
axys_int32_t axys_vfs_access(axys_vfs_node_t node, axys_uint32_t uid, axys_uint32_t gid, axys_uint32_t want);

/* Monotonic count of modifications (create, unlink, write, chmod, chown). The
 * persistence layer uses it to know when a snapshot is worth writing. */
axys_uint64_t axys_vfs_mutations(void);

/* Print the whole tree to the console (used at boot to prove the namespace
 * is up). */
void axys_vfs_dump(void);

/* Exercises create/lookup/read/write/dump against the standard hierarchy.
 * Panics on the first mismatch, same pattern as axys_exception_selftest(). */
void axys_vfs_selftest(void);

#endif
