#include <assert.h>
#include <string.h>

#include "axys/vfs.h"

static void test_root_is_lookup_only(void)
{
    axys_vfs_init();
    assert(axys_vfs_lookup("/") == 0);
    assert(axys_vfs_type(0) == AXYS_VFS_DIR);
    assert(axys_vfs_create("/", AXYS_VFS_DIR) == -1);
    assert(axys_vfs_create("/", AXYS_VFS_FILE) == -1);
}

static void test_create_rejects_trailing_slash(void)
{
    axys_vfs_init();
    assert(axys_vfs_lookup("/etc") >= 0);
    assert(axys_vfs_create("/etc/", AXYS_VFS_FILE) == -1);
    assert(axys_vfs_create("/etc", AXYS_VFS_FILE) == -1);
}

static void test_create_and_io_still_work(void)
{
    static const char payload[] = "axys";
    char out[sizeof(payload)] = {0};
    axys_vfs_node_t file;

    axys_vfs_init();
    file = axys_vfs_create("/tmp/vfs-test", AXYS_VFS_FILE);
    assert(file >= 0);
    assert(axys_vfs_type(file) == AXYS_VFS_FILE);
    assert(axys_vfs_write(file, payload, sizeof(payload)) == (int)sizeof(payload));
    assert(axys_vfs_read(file, out, sizeof(out)) == (int)sizeof(out));
    assert(strcmp(out, payload) == 0);
}

static void test_dynamic_files_unlink_mkdirs(void)
{
    char buf[8];
    axys_vfs_node_t f;

    axys_vfs_init();
    f = axys_vfs_create("/tmp/a", AXYS_VFS_FILE);
    assert(f >= 0 && axys_vfs_size(f) == 0);
    assert(axys_vfs_pwrite(f, 1000, "xyz", 3) == 3);
    assert(axys_vfs_size(f) == 1003);
    assert(axys_vfs_pread(f, 0, buf, 4) == 4 && buf[0] == 0);
    assert(axys_vfs_pread(f, 1000, buf, 8) == 3 && memcmp(buf, "xyz", 3) == 0);
    assert(axys_vfs_pread(f, 5000, buf, 8) == 0);
    assert(axys_vfs_pwrite(f, AXYS_VFS_MAX_FILE_BYTES, "x", 1) == -1);
    assert(axys_vfs_mkdirs("/opt/a/b/c") == 0 && axys_vfs_lookup("/opt/a/b/c") >= 0);
    assert(axys_vfs_mkdirs("/tmp/a/x") == -1); /* a file in the way */
    assert(axys_vfs_unlink("/opt/a") == -1);   /* not empty */
    assert(axys_vfs_unlink("/opt/a/b/c") == 0 && axys_vfs_unlink("/tmp/a") == 0);
    assert(axys_vfs_lookup("/tmp/a") == -1 && axys_vfs_unlink("/") == -1);
    for (int i = 0; i < 200; ++i) { /* recycle nodes far past the live count */
        assert(axys_vfs_create("/tmp/r", AXYS_VFS_FILE) >= 0);
        assert(axys_vfs_unlink("/tmp/r") == 0);
    }
}

static void test_permissions(void)
{
    struct axys_vfs_attr a;
    axys_vfs_node_t f;

    axys_vfs_init();
    f = axys_vfs_create_as("/tmp/p", AXYS_VFS_FILE, 0640, 1000, 50);
    assert(f >= 0 && axys_vfs_getattr(f, &a) == 0 && a.mode == 0640 && a.uid == 1000 && a.gid == 50);
    assert(axys_vfs_access(f, 1000, 1, AXYS_PERM_R | AXYS_PERM_W) == 0); /* owner rw */
    assert(axys_vfs_access(f, 1000, 1, AXYS_PERM_X) == -1);
    assert(axys_vfs_access(f, 2000, 50, AXYS_PERM_R) == 0);              /* group r */
    assert(axys_vfs_access(f, 2000, 50, AXYS_PERM_W) == -1);
    assert(axys_vfs_access(f, 2000, 51, AXYS_PERM_R) == -1);             /* other none */
    assert(axys_vfs_access(f, 0, 0, AXYS_PERM_R | AXYS_PERM_W) == 0);    /* root rw */
    assert(axys_vfs_access(f, 0, 0, AXYS_PERM_X) == -1);                 /* root needs an x bit */
    assert(axys_vfs_chmod(f, 0755) == 0 && axys_vfs_access(f, 0, 0, AXYS_PERM_X) == 0);
    assert(axys_vfs_chown(f, 7, 8) == 0 && axys_vfs_getattr(f, &a) == 0 && a.uid == 7 && a.gid == 8);
    assert(axys_vfs_getattr(axys_vfs_lookup("/root"), &a) == 0 && a.mode == 0700);
    assert(axys_vfs_getattr(axys_vfs_lookup("/tmp"), &a) == 0 && a.mode == 0777);
    assert(axys_vfs_access(axys_vfs_lookup("/etc"), 1000, 1000, AXYS_PERM_R | AXYS_PERM_X) == 0);
    assert(axys_vfs_access(axys_vfs_lookup("/etc"), 1000, 1000, AXYS_PERM_W) == -1);
    assert(axys_vfs_create_as("/tmp/q", (axys_vfs_type_t)9, 0, 0, 0) == -1);
    assert(axys_vfs_access(-1, 0, 0, AXYS_PERM_R) == -1);
}

/* Regression: a freed node slot is reused, so a node index alone must never be
 * trusted across an unlink. The generation tells the old handle apart. */
static void test_slot_reuse_changes_generation(void)
{
    axys_vfs_node_t a;
    axys_vfs_node_t b;
    axys_uint32_t gen_a;

    axys_vfs_init();
    a = axys_vfs_create("/tmp/old", AXYS_VFS_FILE);
    assert(a >= 0);
    gen_a = axys_vfs_generation(a);
    assert(gen_a != 0);
    assert(axys_vfs_unlink("/tmp/old") == 0);
    assert(axys_vfs_generation(a) == 0); /* dead slot is never "live" */
    b = axys_vfs_create("/tmp/new", AXYS_VFS_FILE);
    assert(b == a);                       /* LIFO free list reuses the slot */
    assert(axys_vfs_generation(b) != gen_a);
    assert(axys_vfs_generation(-1) == 0);
}

/* Regression: replacing a file must not destroy it when the new data is refused. */
static void test_failed_replace_keeps_contents(void)
{
    static const char keep[] = "keep";
    axys_vfs_node_t f;
    char buf[8] = {0};

    axys_vfs_init();
    f = axys_vfs_create("/tmp/k", AXYS_VFS_FILE);
    assert(f >= 0 && axys_vfs_write(f, keep, 4) == 4);
    assert(axys_vfs_write(f, keep, (axys_size_t)AXYS_VFS_MAX_FILE_BYTES + 1u) == -1);
    assert(axys_vfs_size(f) == 4 && axys_vfs_read(f, buf, 4) == 4 && buf[0] == 'k');
}

int main(void)
{
    test_permissions();
    test_dynamic_files_unlink_mkdirs();
    test_root_is_lookup_only();
    test_create_rejects_trailing_slash();
    test_create_and_io_still_work();
    test_slot_reuse_changes_generation();
    test_failed_replace_keeps_contents();
    return 0;
}
