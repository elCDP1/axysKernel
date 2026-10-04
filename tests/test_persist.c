#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "axys/persist.h"
#include "axys/vfs.h"

extern int ramdisk_present;
extern unsigned ramdisk_fail_writes_after;
extern unsigned char *ramdisk_bytes(void);

static int read_file(const char *path, char *out, size_t cap)
{
    axys_vfs_node_t n = axys_vfs_lookup(path);
    int got;

    if (n < 0) {
        return -1;
    }
    got = axys_vfs_read(n, out, cap - 1);
    if (got >= 0) {
        out[got] = '\0';
    }
    return got;
}

static void put(const char *path, const char *text, unsigned mode, unsigned uid)
{
    axys_vfs_node_t n = axys_vfs_create_as(path, AXYS_VFS_FILE, mode, uid, uid);

    assert(n >= 0 && axys_vfs_write(n, text, strlen(text)) == (axys_int32_t)strlen(text));
}

static void reboot_expect_at(int files, int line)
{
    axys_vfs_init();
    { int got = axys_persist_init(); if (got != files) { fprintf(stderr, "reboot_expect line %d: wanted %d got %d\n", line, files, got); } assert(got == files); }
}

static void put_over(const char *path, const char *text)
{
    axys_vfs_node_t n = axys_vfs_lookup(path);

    assert(n >= 0 && axys_vfs_write(n, text, strlen(text)) == (axys_int32_t)strlen(text));
}

#define reboot_expect(n) reboot_expect_at((n), __LINE__)

int main(void)
{
    char buf[64];
    struct axys_vfs_attr a;

    /* crc32 known answer */
    assert(axys_crc32(0, "123456789", 9) == 0xcbf43926u);

    /* no disk: volatile, sync refused */
    ramdisk_present = 0;
    axys_vfs_init();
    assert(axys_persist_init() == -1 && !axys_persist_available() && axys_persist_sync() == -1);
    ramdisk_present = 1;

    /* blank disk is formatted; nothing to restore */
    axys_vfs_init();
    assert(axys_persist_init() == 0 && axys_persist_available());
    put("/home/alice.txt", "hello alice", 0640, 1000);
    put("/etc/conf", "k=v", 0644, 0);
    assert(axys_vfs_create_as("/home/dir", AXYS_VFS_DIR, 0700, 1000, 1000) >= 0);
    put("/home/dir/nested", "deep", 0600, 1000);
    put("/tmp/volatile", "gone", 0644, 0);
    put("/bin/notsaved", "x", 0755, 0);
    assert(axys_persist_sync() == 0 && axys_persist_generation() == 1);

    /* "reboot": fresh VFS, restore from disk */
    reboot_expect(3);
    assert(read_file("/home/alice.txt", buf, sizeof(buf)) == 11 && strcmp(buf, "hello alice") == 0);
    assert(axys_vfs_getattr(axys_vfs_lookup("/home/alice.txt"), &a) == 0 && a.mode == 0640 && a.uid == 1000 && a.gid == 1000);
    assert(axys_vfs_getattr(axys_vfs_lookup("/home/dir"), &a) == 0 && a.mode == 0700 && a.uid == 1000);
    assert(read_file("/home/dir/nested", buf, sizeof(buf)) == 4 && strcmp(buf, "deep") == 0);
    assert(axys_vfs_lookup("/tmp/volatile") < 0 && axys_vfs_lookup("/bin/notsaved") < 0);
    assert(axys_persist_generation() == 1);

    /* second snapshot goes to the other slot; newest wins on restore */
    put_over("/home/alice.txt", "version two");
    assert(axys_persist_sync() == 0 && axys_persist_generation() == 2);
    reboot_expect(3);
    assert(read_file("/home/alice.txt", buf, sizeof(buf)) > 0 && strcmp(buf, "version two") == 0);
    assert(axys_persist_generation() == 2);

    /* power loss at every possible point of a sync: old or new, never garbage */
    for (unsigned cut = 0; cut < 16; ++cut) {
        char want_new[32];

        snprintf(want_new, sizeof(want_new), "cut-%u", cut);
        put_over("/home/alice.txt", want_new);
        ramdisk_fail_writes_after = cut;
        (void)axys_persist_sync();
        ramdisk_fail_writes_after = 0xffffffffu;
        reboot_expect(3);
        assert(read_file("/home/alice.txt", buf, sizeof(buf)) > 0);
        assert(strcmp(buf, want_new) == 0 || strcmp(buf, "version two") == 0 || strncmp(buf, "cut-", 4) == 0);
        assert(read_file("/home/dir/nested", buf, sizeof(buf)) == 4); /* everything else intact */
        /* make the surviving state the new baseline */
        put_over("/home/alice.txt", "version two");
        assert(axys_persist_sync() == 0);
    }

    /* a damaged newest snapshot falls back to the previous one */
    put_over("/home/alice.txt", "older");
    assert(axys_persist_sync() == 0);
    put_over("/home/alice.txt", "newest");
    assert(axys_persist_sync() == 0);
    {
        unsigned char *d = ramdisk_bytes();
        axys_uint64_t g0, g1;
        unsigned newest;

        memcpy(&g0, d + 1 * 512 + 8, 8);
        memcpy(&g1, d + (1 + 2047) * 512 + 8, 8);
        newest = g0 > g1 ? 0 : 1;
        d[(1 + newest * 2047 + 1) * 512 + 3] ^= 0xff; /* flip a payload byte */
    }
    reboot_expect(3);
    assert(read_file("/home/alice.txt", buf, sizeof(buf)) > 0 && strcmp(buf, "older") == 0);
    assert(axys_persist_sync() == 0); /* rewrites the damaged slot: both slots healthy again */

    /* a CRC-valid snapshot that tries to plant /bin/evil is rejected */
    {
        unsigned char *d = ramdisk_bytes();
        axys_uint64_t g0, g1;
        unsigned newest, target;
        unsigned char payload[512];
        struct { unsigned short len; unsigned char type, res; unsigned mode, uid, gid, size; } __attribute__((packed)) rec = {9, 1, 0, 0755, 0, 0, 4};
        struct { unsigned short len; unsigned char type, res; unsigned mode, uid, gid, size; } __attribute__((packed)) end = {0, 0, 0, 0, 0, 0, 0};
        unsigned char header[512];
        axys_uint64_t gen;
        unsigned plen = 0, crc;

        memcpy(&g0, d + 1 * 512 + 8, 8);
        memcpy(&g1, d + (1 + 2047) * 512 + 8, 8);
        newest = g0 > g1 ? 0 : 1;
        target = newest ? 0 : 1;
        gen = (g0 > g1 ? g0 : g1) + 5;
        memset(payload, 0, sizeof(payload));
        memcpy(payload + plen, &rec, sizeof(rec)); plen += sizeof(rec);
        memcpy(payload + plen, "/bin/evil", 9); plen += 9;
        memcpy(payload + plen, "evil", 4); plen += 4;
        memcpy(payload + plen, &end, sizeof(end)); plen += sizeof(end);
        memset(header, 0, sizeof(header));
        memcpy(header, "AXSNAP01", 8);
        memcpy(header + 8, &gen, 8);
        memcpy(header + 16, &plen, 4);
        crc = axys_crc32(0, payload, plen);
        memcpy(header + 20, &crc, 4);
        crc = axys_crc32(0, header, 24);
        memcpy(header + 24, &crc, 4);
        memcpy(d + (1 + target * 2047 + 1) * 512, payload, 512);
        memcpy(d + (1 + target * 2047) * 512, header, 512);
        reboot_expect(3);          /* the forged (newest) snapshot is rejected; the honest one loads */
        assert(axys_vfs_lookup("/bin/evil") < 0);
        assert(axys_persist_sync() == 0); /* heal the forged slot before the next test */
    }

    /* a CRC-valid snapshot that turns bad halfway applies NOTHING: the two
     * good records must not leak into the VFS, and the older slot loads. */
    {
        unsigned char *d = ramdisk_bytes();        axys_uint64_t g0, g1;
        unsigned newest, target;
        unsigned char payload[1024];
        struct { unsigned short len; unsigned char type, res; unsigned mode, uid, gid, size; } __attribute__((packed)) rec;
        struct { unsigned short len; unsigned char type, res; unsigned mode, uid, gid, size; } __attribute__((packed)) end = {0, 0, 0, 0, 0, 0, 0};
        unsigned char header[512];
        axys_uint64_t gen, older;
        unsigned plen = 0, crc;

        memcpy(&g0, d + 1 * 512 + 8, 8);
        memcpy(&g1, d + (1 + 2047) * 512 + 8, 8);
        newest = g0 > g1 ? 0 : 1;
        target = newest ? 0 : 1;
        older = g0 > g1 ? g0 : g1; /* honest generation: the fallback lands here */
        gen = older + 5;
        memset(payload, 0, sizeof(payload));
        rec.len = 10; rec.type = 1; rec.res = 0; rec.mode = 0644; rec.uid = 0; rec.gid = 0; rec.size = 4;
        memcpy(payload + plen, &rec, sizeof(rec)); plen += sizeof(rec);
        memcpy(payload + plen, "/home/good", 10); plen += 10;
        memcpy(payload + plen, "good", 4); plen += 4;
        rec.len = 12; rec.size = 6;
        memcpy(payload + plen, &rec, sizeof(rec)); plen += sizeof(rec);
        memcpy(payload + plen, "/home/second", 12); plen += 12;
        memcpy(payload + plen, "second", 6); plen += 6;
        rec.len = 3; rec.type = 9; rec.size = 0; /* malformed trailing record */
        memcpy(payload + plen, &rec, sizeof(rec)); plen += sizeof(rec);
        memcpy(payload + plen, &end, sizeof(end)); plen += sizeof(end);
        memset(header, 0, sizeof(header));
        memcpy(header, "AXSNAP01", 8);
        memcpy(header + 8, &gen, 8);
        memcpy(header + 16, &plen, 4);
        crc = axys_crc32(0, payload, plen);
        memcpy(header + 20, &crc, 4);
        crc = axys_crc32(0, header, 24);
        memcpy(header + 24, &crc, 4);
        memcpy(d + (1 + target * 2047 + 1) * 512, payload, 1024);
        memcpy(d + (1 + target * 2047) * 512, header, 512);
        reboot_expect(3); /* falls back to the honest slot; nothing half-applied */
        assert(axys_vfs_lookup("/home/good") < 0);
        assert(axys_vfs_lookup("/home/second") < 0);
        assert(axys_persist_generation() == older);
        assert(read_file("/home/alice.txt", buf, sizeof(buf)) > 0);
        assert(axys_persist_sync() == 0); /* heal the forged slot */
    }

    /* trees deeper than the old 16-level cutoff save and restore completely */
    {
        char path[128] = "/home/deep";
        char leaf[160];

        assert(axys_vfs_mkdirs(path) == 0);
        for (int i = 0; i < 29; ++i) {
            size_t l = strlen(path);

            snprintf(path + l, sizeof(path) - l, "/d%d", i);
            assert(axys_vfs_mkdirs(path) == 0);
        }
        snprintf(leaf, sizeof(leaf), "%s/bottom.txt", path);
        put(leaf, "bottom", 0644, 0);
        assert(axys_persist_sync() == 0);
        {
            int files = 0;
            char probe[160];

            axys_vfs_init();
            files = axys_persist_init();
            assert(files > 3);
            assert(read_file(leaf, buf, sizeof(buf)) == 6 && strcmp(buf, "bottom") == 0);
            snprintf(probe, sizeof(probe), "/home/deep/d0");
            assert(axys_vfs_lookup(probe) >= 0);
            (void)files;
        }
        assert(axys_vfs_remove_tree("/home/deep") > 0);
        assert(axys_persist_sync() == 0);
    }

    /* a disk that holds somebody else's data is never touched */
    memset(ramdisk_bytes(), 0, 512);
    ramdisk_bytes()[100] = 0x42;
    axys_vfs_init();
    assert(axys_persist_init() == -2 && !axys_persist_available());
    assert(ramdisk_bytes()[100] == 0x42);

    printf("test_persist: ok\n");
    return 0;
}
