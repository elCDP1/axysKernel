#include <assert.h>
#include <stdio.h>
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
    assert(axys_vfs_getattr(axys_vfs_lookup("/tmp"), &a) == 0 && a.mode == 01777);
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

/* Regression: rm -rf with 10 children freed only 6 (swap-remove skipped the
 * entry swapped into the hole while the forward index kept advancing). */
static void test_remove_tree_frees_all_children(void)
{
    char path[32];

    axys_vfs_init();
    assert(axys_vfs_create("/tmp/wide", AXYS_VFS_DIR) >= 0);
    for (int i = 0; i < 10; ++i) {
        snprintf(path, sizeof(path), "/tmp/wide/f%d", i);
        assert(axys_vfs_create(path, AXYS_VFS_FILE) >= 0);
    }
    assert(axys_vfs_remove_tree("/tmp/wide") == 11); /* dir + 10 files */
    assert(axys_vfs_lookup("/tmp/wide") == -1);
    for (int i = 0; i < 10; ++i) {
        snprintf(path, sizeof(path), "/tmp/wide/f%d", i);
        assert(axys_vfs_lookup(path) == -1);
    }
    /* Nested directories go too, and the count covers every node. */
    assert(axys_vfs_mkdirs("/tmp/deep/a/b") == 0);
    assert(axys_vfs_create("/tmp/deep/a/f", AXYS_VFS_FILE) >= 0);
    assert(axys_vfs_remove_tree("/tmp/deep") == 4);
    assert(axys_vfs_lookup("/tmp/deep") == -1);
    assert(axys_vfs_remove_tree("/") == -1); /* never the root */
}

/* Regression: rename accepted "." and ".." as the destination leaf, planting
 * nodes with those names in the tree. */
static void test_rename_rejects_dot_names(void)
{
    axys_vfs_init();
    assert(axys_vfs_create("/tmp/src", AXYS_VFS_FILE) >= 0);
    assert(axys_vfs_rename("/tmp/src", "/tmp/.") != 0);
    assert(axys_vfs_rename("/tmp/src", "/tmp/..") != 0);
    assert(axys_vfs_lookup("/tmp/src") >= 0); /* source untouched */
    assert(axys_vfs_rename("/tmp/src", "/moved") == 0); /* root parent works */
    assert(axys_vfs_lookup("/moved") >= 0 && axys_vfs_lookup("/tmp/src") == -1);
}

static void test_name_copy_and_sticky(void)
{
    char buf[AXYS_VFS_NAME_MAX];
    char small[4];
    axys_vfs_node_t tmp;
    axys_vfs_node_t victim;
    axys_vfs_node_t mine;
    axys_vfs_node_t plain;

    axys_vfs_init();
    tmp = axys_vfs_lookup("/tmp");
    assert(tmp >= 0);
    victim = axys_vfs_create_as("/tmp/victim", AXYS_VFS_FILE, 0644, 0, 0);
    mine = axys_vfs_create_as("/tmp/mine", AXYS_VFS_FILE, 0644, 1000, 1000);
    assert(victim >= 0 && mine >= 0);

    /* name_copy under lock. */
    assert(axys_vfs_name_copy(victim, buf, sizeof(buf)) == 6);
    assert(strcmp(buf, "victim") == 0);
    assert(axys_vfs_name_copy(-1, buf, sizeof(buf)) == -1);
    assert(axys_vfs_name_copy(victim, buf, 0) == -1);
    assert(axys_vfs_name_copy(victim, small, sizeof(small)) == -1); /* too small */

    /* Sticky /tmp: uid 1000 may not remove root's file... */
    assert(axys_vfs_sticky_ok(tmp, victim, 1000) == -1);
    /* ...but may remove their own, and root may remove anything. */
    assert(axys_vfs_sticky_ok(tmp, mine, 1000) == 0);
    assert(axys_vfs_sticky_ok(tmp, victim, 0) == 0);
    assert(axys_vfs_sticky_ok(-1, victim, 1000) == -1);

    /* Non-sticky directories impose no extra restriction. */
    plain = axys_vfs_create_as("/var/plain", AXYS_VFS_FILE, 0644, 0, 0);
    assert(plain >= 0);
    assert(axys_vfs_sticky_ok(axys_vfs_lookup("/var"), plain, 1000) == 0);
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
    test_remove_tree_frees_all_children();
    test_rename_rejects_dot_names();
    test_name_copy_and_sticky();
    return 0;
}
