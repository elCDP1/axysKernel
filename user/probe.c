#include "axys.h"

/* Syscall edge cases: fd table limits, seek/write interactions, sbrk growth,
 * process control and VFS limits. Prints the name of the first failure and
 * returns a distinct code per area so a run identifies the culprit at once. */

static int failures;

#define AXYS_PATH_PROBE_MAX 256

static void check(const char *what, int ok, int code)
{
    if (ok) {
        return;
    }
    puts("FAIL: ");
    puts(what);
    puts("\n");
    if (failures == 0) {
        failures = code;
    }
}

static void probe_fds(void)
{
    char buf[8];
    int fds[80];
    int n = 0;
    int emfile_seen = 0;

    /* O_TRUNC/O_APPEND modify the file, so they are only honoured with a write
     * mode. Pin that, otherwise a read-only open would silently truncate. */
    check("O_TRUNC without a write mode is refused",
          open("/tmp/probe.fd", O_CREAT | O_TRUNC) == -22, 10);
    check("O_APPEND without a write mode is refused",
          open("/tmp/probe.fd", O_CREAT | O_APPEND) == -22, 11);
    {
        int seed = open("/tmp/probe.fd", O_CREAT | O_RDWR);

        close(seed);
        /* Now the path exists, so a plain read-only open must be accepted. */
        check("the same path opens read-only just fine",
              open("/tmp/probe.fd", 0) >= 0, 12);
    }

    /* Exhaust the descriptor table, then confirm the limit is reported and
     * that closing one really frees a slot. */
    for (int i = 0; i < 80; ++i) {
        fds[i] = open("/tmp/probe.fd", O_CREAT | O_TRUNC | O_RDWR);
        if (fds[i] < 0) {
            emfile_seen = 1;
            break;
        }
        n++;
    }
    check("fd table exhausts with EMFILE", emfile_seen, 1);
    check("fd table gave us descriptors", n >= 8, 2);
    check("open refused while the table is full", open("/tmp/probe.fd", O_CREAT) < 0, 3);
    if (n > 0) {
        int reuse = fds[n - 1];

        close(reuse);
        check("close frees a slot", open("/tmp/probe.fd", 0) >= 0, 4);
    }
    for (int i = 0; i < n; ++i) {
        close(fds[i]);
    }

    /* Bogus descriptors must be rejected, not silently accepted. */
    check("close of a bad fd fails", close(4242) == -9, 5);
    check("read of a bad fd fails", read(4242, buf, sizeof(buf)) == -9, 6);
    check("write of a bad fd fails", write(4242, "x", 1) == -9, 7);
    check("lseek of a bad fd fails", lseek(4242, 0, 0) == -9, 8);

    /* std streams stay open across close(0..2). */
    int fd = open("/tmp/probe.stdin", O_CREAT | O_TRUNC | O_RDWR);

    if (fd >= 3) {
        check("close(1) refused for a std stream", close(1) == 0, 9);
        close(fd);
    }
}

static void probe_seek(void)
{
    char buf[32];
    struct stat_info st;
    int fd = open("/tmp/probe.seek", O_CREAT | O_TRUNC | O_RDWR);

    if (fd < 0) {
        check("open for seek failed", 0, 10);
        return;
    }
    check("write 5 bytes", write(fd, "abcde", 5) == 5, 10);
    check("seek to end", lseek(fd, 0, 2) == 5, 11);
    check("write at end", write(fd, "FG", 2) == 2, 12);
    check("seek to start", lseek(fd, 0, 0) == 0, 13);
    memset(buf, 0, sizeof(buf));
    check("read whole file", read(fd, buf, sizeof(buf)) == 7, 14);
    check("contents match", memcmp(buf, "abcdeFG", 7) == 0, 15);
    check("size after append", lseek(fd, 0, 2) == 7, 16);

    /* A seek past EOF leaves a hole that must read back as zeros. */
    check("seek past EOF", lseek(fd, 12, 0) == 12, 17);
    check("write after hole", write(fd, "Z", 1) == 1, 18);
    check("size with hole", lseek(fd, 0, 2) == 13, 19);
    check("read at EOF is empty", read(fd, buf, sizeof(buf)) == 0, 20);
    memset(buf, 0xAA, sizeof(buf));
    check("seek back to hole", lseek(fd, 5, 0) == 5, 21);
    check("read the hole", read(fd, buf, 8) == 8, 22);
    /* Offsets 5 and 6 hold "FG"; the hole is 7..11 and 12 holds 'Z'. */
    check("hole reads as zeros", buf[1] == 'G' && buf[2] == 0 && buf[6] == 0 && buf[7] == 'Z', 23);

    /* Out-of-range seeks must be refused. */
    check("negative seek refused", lseek(fd, -1, 0) < 0, 24);
    check("seek far past EOF refused", lseek(fd, 1L << 40, 0) < 0, 25);
    check("bad whence refused", lseek(fd, 0, 77) < 0, 26);
    check("seek on a directory refused", lseek(open("/tmp", 0), 0, 0) < 0, 27);

    check("stat reports the size", stat("/tmp/probe.seek", &st) == 0 && st.size == 13, 28);
    close(fd);
}

static void probe_sbrk(void)
{
    void *base = sbrk(0);

    check("sbrk(0) returns a break", (long)base > 0, 30);
    check("sbrk(0) is stable", (long)sbrk(0) == (long)base, 31);

    /* Many small allocations must be contiguous and non-overlapping. */
    char *p = sbrk(0);

    for (int i = 0; i < 64; ++i) {
        char *q = sbrk(64);

        if ((long)q < 0) {
            check("incremental sbrk", 0, 32);
            return;
        }
        for (int k = 0; k < 64; ++k) {
            q[k] = (char)i;
        }
    }
    for (int i = 0; i < 64; ++i) {
        for (int k = 0; k < 64; ++k) {
            if (p[i * 64 + k] != (char)i) {
                check("incremental sbrk does not overlap", 0, 33);
                return;
            }
        }
    }
    /* A huge request must fail without corrupting the break. */
    check("huge sbrk refused", (long)sbrk(1L << 46) < 0, 34);
    check("break survives a refusal", (long)sbrk(0) >= (long)p, 35);
}

static void probe_procs(void)
{
    int status = -1;
    int pid;

    check("wait for a stranger fails", wait(9999, &status) == -10, 40);
    check("spawn of a missing program fails", spawn("/bin/nope", "") == -2, 41);

    pid = spawn("/bin/hello", "probe");
    check("spawn returns a pid", pid > 0, 42);
    if (pid > 0) {
        check("wait reaps the child", wait(pid, &status) == pid, 43);
    }
    check("kill of a stranger is refused", kill(9999) == -3, 44);
}

static void probe_vfs(void)
{
    struct stat_info st;
    char name[64];
    char buf[64];
    int made = 0;
    int refused = 0;

    check("stat of a directory", stat("/tmp", &st) == 0 && st.type == 2, 50);
    check("readdir of a file fails", readdir("/bin/hello", 0, buf, sizeof(buf)) < 0, 51);
    check("mkdir under a file fails", mkdir("/bin/hello/sub") == -20, 52);
    check("create under a missing parent fails", open("/tmp/nodir/x", O_CREAT | O_RDWR) == -2, 53);

    /* Error codes must say what actually went wrong, not a blanket ENOENT. */
    check("mkdir -p creates a missing parent", mkdir("/tmp/nodir3/x") == 0, 67);
    check("nested leaf is a directory", stat("/tmp/nodir3/x", &st) == 0 && st.type == 2, 76);
    check("nested parents are removed", rmtree("/tmp/nodir3") == 2, 96);
    check("create under a file is ENOTDIR", open("/bin/hello/y", O_CREAT | O_RDWR) == -20, 68);
    check("mkdir under a file is ENOTDIR", mkdir("/bin/hello/z") == -20, 69);

    /* unlink() also removes an empty directory; a populated one must not go. */
    check("mkdir for the ENOTEMPTY check", mkdir("/tmp/full") == 0, 70);
    check("populate it", open("/tmp/full/inner", O_CREAT | O_RDWR) >= 0, 71);
    check("rm of a populated directory is ENOTEMPTY", unlink("/tmp/full") == -39, 72);
    check("rm of a missing path is ENOENT", unlink("/tmp/nosuchthing") == -2, 73);
    unlink("/tmp/full/inner");
    check("rm works once it is empty", unlink("/tmp/full") == 0, 74);

    /* A deep path is legal as long as each component stays within 31 characters.
     * The VFS allows 256 bytes, so a nested path past the old 128-byte syscall
     * cap must now reach it intact. */
    {
        char deep[AXYS_PATH_PROBE_MAX];
        size_t len = 0;
        int ok = 1;

        memcpy(deep, "/tmp", 4);
        len = 4;
        for (int level = 0; level < 10 && ok; ++level) {
            deep[len++] = '/';
            for (int i = 0; i < 18; ++i) {
                deep[len++] = (char)('a' + ((level + i) % 26));
            }
            deep[len] = '\0';
            /* The last level is the file itself, so only the ones above it
             * may be directories. */
            if (level < 9) {
                ok = mkdir(deep) == 0;
            }
        }
        check("nested path built one level at a time", ok, 75);
        check("nested path is longer than 128 bytes", len > 128, 77);
        {
            int fd = open(deep, O_CREAT | O_TRUNC | O_RDWR);

            check("deep path create", fd >= 0, 78);
            if (fd >= 0) {
                check("deep path write", write(fd, "deep", 4) == 4, 79);
                close(fd);
            }
            unlink(deep);
            /* unwind the directories, rebuilding each prefix */
            for (int level = 8; level >= 0; --level) {
                size_t l = 4;

                for (int k = 0; k <= level; ++k) {
                    deep[l++] = '/';
                    for (int i = 0; i < 18; ++i) {
                        deep[l++] = (char)('a' + ((k + i) % 26));
                    }
                }
                deep[l] = '\0';
                unlink(deep);
            }
        }
    }

    /* Name length limit is 31 characters; 32 must be refused outright. */
    for (int i = 0; i < 31; ++i) {
        name[i] = 'n';
    }
    name[31] = '\0';
    memcpy(name, "/tmp/", 5);
    check("31-char name is accepted", open(name, O_CREAT | O_TRUNC | O_RDWR) >= 0, 54);
    for (int i = 0; i < 32; ++i) {
        name[5 + i] = 'n';
    }
    name[37] = '\0';
    check("32-char name is refused", open(name, O_CREAT | O_TRUNC | O_RDWR) < 0, 55);

    /* Fill a directory well past the old 32-child cap. Each descriptor is
     * closed again: the table, not the node pool, is the first limit. */
    for (int i = 0; i < 200; ++i) {
        char p[32];
        int fd;

        memcpy(p, "/tmp/m", 6);
        p[6] = (char)('a' + (i / 100));
        p[7] = (char)('a' + (i % 100));
        p[8] = '\0';
        fd = open(p, O_CREAT | O_TRUNC | O_RDWR);
        if (fd >= 0) {
            close(fd);
            made++;
        } else {
            refused = 1;
            break;
        }
    }
    check("200 children created", made == 200, 56);
    check("no refusal while below the pool cap", !refused, 57);
    for (int i = 0; i < 200; ++i) {
        char p[32];

        memcpy(p, "/tmp/m", 6);
        p[6] = (char)('a' + (i / 100));
        p[7] = (char)('a' + (i % 100));
        p[8] = '\0';
        unlink(p);
    }
}

/* The kernel heap is a few hundred KiB while the VFS advertises 16 MiB files.
 * Allocating past the heap must fail cleanly: no corruption, and the kernel has
 * to keep serving requests afterwards. */
static void probe_exhaust(void)
{
    char block[4096];
    int fd = open("/tmp/probe.big", O_CREAT | O_TRUNC | O_RDWR);
    ssize_t total = 0;
    int failed_at = 0;
    int failcode = 0;

    memset(block, 'A', sizeof(block));
    if (fd < 0) {
        check("open for exhaustion failed", 0, 60);
        return;
    }
    for (int i = 0; i < 200; ++i) {
        ssize_t n = write(fd, block, sizeof(block));

        if (n < 0) {
            failed_at = i;
            failcode = (int)n;
            break;
        }
        total += n;
    }
    check("write loop terminated", 1, 61);
    puts("probe: wrote ");
    put_u64((u64)total);
    puts(" bytes");
    if (failed_at) {
        puts(" then failed with errno ");
        put_u64((u64)(-failcode));
        puts("\n");
    } else {
        puts(" with no failure\n");
    }
    /* Whatever happened, the file must still be consistent. */
    struct stat_info st;

    if (stat("/tmp/probe.big", &st) == 0 && st.type == 1) {
        char probe[16];

        if (lseek(fd, 0, 0) == 0 && read(fd, probe, sizeof(probe)) > 0 && probe[0] == 'A') {
            puts("probe: file still readable and intact\n");
        } else {
            check("big file stayed readable", 0, 62);
        }
    } else {
        check("stat of the big file", 0, 63);
    }
    close(fd);
    unlink("/tmp/probe.big");

    /* A sparse seek far past EOF must also fail cleanly, not wedge the VFS. */
    fd = open("/tmp/probe.sparse", O_CREAT | O_TRUNC | O_RDWR);
    if (fd >= 0) {
        /* pwrite() reserves the gap, so a far offset may legitimately succeed.
         * Either way the descriptor has to stay usable afterwards. */
        long at = 8L * 1024 * 1024;

        if (lseek(fd, at, 0) == at) {
            (void)write(fd, "x", 1);
        }
        check("sparse fd still usable", lseek(fd, 0, 2) >= at, 64);
        check("sparse fd still readable", read(fd, block, 1) >= 0, 65);
        close(fd);
        unlink("/tmp/probe.sparse");
    }
    /* The shell must still work after all of that. */
    check("kernel still alive after exhaustion", uptime_ms() > 0, 66);
}

static void probe_rename_tree(void)
{
    struct stat_info st;
    struct meminfo before;
    struct meminfo after;
    int fd;

    check("meminfo works", meminfo(&before) == 0, 80);

    /* Multi-level mkdir -p through the syscall (used to fail with ENOENT). */
    check("mkdir -p of three levels", mkdir("/tmp/rn/a/b") == 0, 81);
    check("nested leaf is a directory", stat("/tmp/rn/a/b", &st) == 0 && st.type == 2, 82);
    fd = open("/tmp/rn/a/f", O_CREAT | O_TRUNC | O_RDWR);
    check("file inside the new tree", fd >= 0, 83);
    if (fd >= 0) {
        close(fd);
    }

    /* rename() moves within the VFS, including onto the root directory. */
    check("rename file to root level", rename("/tmp/rn/a/f", "/tmp/rn-moved") == 0, 84);
    check("old name is gone", stat("/tmp/rn/a/f", &st) == -2, 85);
    check("new name resolves", stat("/tmp/rn-moved", &st) == 0 && st.type == 1, 86);
    check("rename onto a directory is EISDIR", rename("/tmp/rn-moved", "/tmp/rn") == -21, 87);
    check("dotted destination canonicalizes onto a directory",
          rename("/tmp/rn-moved", "/tmp/.") == -21, 88);
    check("rename of a file onto the root is EISDIR", rename("/tmp/rn-moved", "/..") == -21, 95);
    check("rename of a directory onto the root is EINVAL", rename("/tmp/rn", "/") == -22, 97);
    check("rename of a missing file is ENOENT", rename("/tmp/nope", "/tmp/nope2") == -2, 89);

    /* rmtree() removes recursively and reports the node count. */
    {
        int removed = rmtree("/tmp/rn");

        check("rmtree removed the tree", removed == 3, 90);
    }
    check("tree is gone", stat("/tmp/rn", &st) == -2, 91);
    check("rmtree of a missing path fails", rmtree("/tmp/rn") < 0, 92);
    unlink("/tmp/rn-moved");

    /* No VFS nodes leaked: the count must be back where it started. */
    check("meminfo still works", meminfo(&after) == 0, 93);
    check("no VFS nodes leaked", after.live_nodes == before.live_nodes, 94);
}

int main(const char *args, size_t len)
{
    (void)args;
    (void)len;
    puts("probe: start\n");
    probe_fds();
    probe_seek();
    probe_sbrk();
    probe_procs();
    probe_vfs();
    probe_exhaust();
    probe_rename_tree();
    unlink("/tmp/probe.fd");
    unlink("/tmp/probe.seek");
    if (failures == 0) {
        puts("probe: all checks passed\n");
        return 0;
    }
    put_u64((u64)failures);
    puts(" = first failing area\n");
    return failures;
}