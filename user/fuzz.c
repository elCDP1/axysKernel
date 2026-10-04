#include "axys.h"

/* In-guest syscall fuzzer. Exercises hostile pointers, descriptor/node
 * exhaustion, VFS model operations and kernel-heap/node accounting, printing
 * heartbeats so the host orchestrator can tell PASS / FAIL / HANG apart.
 * Usage: fuzz [seed] [iters]. Ends with "fuzz: PASS" (exit 0) or the first
 * "fuzz: FAIL: ..." (exit 1). A kernel panic or exception instead means the
 * kernel fell over: that is the bug this program hunts. */

static u64 rng_state;

static void rng_seed(u64 seed)
{
    rng_state = seed ? seed : (u64)0x9e3779b97f4a7c15UL;
}

static u64 rng_next(void)
{
    u64 x = rng_state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * (u64)0x2545f4914f6cdd1dUL;
}

static u64 rng_below(u64 n)
{
    return rng_next() % (n ? n : (u64)1);
}

static int failures;

static void fail(const char *what)
{
    puts("fuzz: FAIL: ");
    puts(what);
    puts("\n");
    failures = 1;
}

static void heartbeat(const char *phase, u64 i)
{
    puts("fuzz: heartbeat ");
    puts(phase);
    puts(" ");
    put_u64(i);
    puts("\n");
}

static void put_path(char *dst, u64 k, int sub)
{
    /* /tmp/fz[/d<N>]/f<N> */
    const char *pre = "/tmp/fz";
    u64 i = 0;

    while (*pre) {
        dst[i++] = *pre++;
    }
    if (sub >= 0) {
        dst[i++] = '/';
        dst[i++] = 'd';
        dst[i++] = (char)('0' + (sub % 10));
    }
    dst[i++] = '/';
    dst[i++] = 'f';
    dst[i++] = (char)('0' + (k % 10));
    dst[i] = '\0';
}

static void phase_hostile(void)
{
    static const char *const bad[] = {
        (const char *)0, (const char *)1, (const char *)0xfff,
        (const char *)0x100000UL, (const char *)0xffffffff80000000UL,
        (const char *)0xffffffffffffffffUL,
    };
    char name[64];
    struct stat_info st;
    struct meminfo mi;

    heartbeat("hostile", 0);
    for (u64 i = 0; i < 6; ++i) {
        /* Every one of these must be refused, never honored and never fatal. */
        if (open(bad[i], 0) >= 0) {
            fail("open accepted a hostile pointer");
            return;
        }
        if (stat(bad[i], &st) >= 0) {
            fail("stat accepted a hostile pointer");
            return;
        }
        if (mkdir(bad[i]) >= 0) {
            fail("mkdir accepted a hostile pointer");
            return;
        }
        if (unlink(bad[i]) >= 0) {
            fail("unlink accepted a hostile pointer");
            return;
        }
        if (rename(bad[i], "/tmp/fz/x") >= 0) {
            fail("rename accepted a hostile source");
            return;
        }
        if (rename("/tmp/fz/x", bad[i]) >= 0) {
            fail("rename accepted a hostile destination");
            return;
        }
        if (chmod(bad[i], 0644) >= 0) {
            fail("chmod accepted a hostile pointer");
            return;
        }
        if (chown(bad[i], 0, 0) >= 0) {
            fail("chown accepted a hostile pointer");
            return;
        }
        if (readdir(bad[i], 0, name, sizeof(name)) >= 0) {
            fail("readdir accepted a hostile pointer");
            return;
        }
        if (spawn(bad[i], "") >= 0) {
            fail("spawn accepted a hostile pointer");
            return;
        }
    }
    if (write(1, (const void *)0x100000UL, 16) != -14) {
        fail("write accepted a kernel buffer");
        return;
    }
    if (read(0, (void *)0x100000UL, 16) != -14) {
        fail("read accepted a kernel buffer");
        return;
    }
    if (getrandom((void *)0xffffffff80000000UL, 16) != -14) {
        fail("getrandom accepted a kernel buffer");
        return;
    }
    if (meminfo(&mi) != 0) {
        fail("meminfo failed");
        return;
    }
    heartbeat("hostile", 1);
}

static void phase_model(u64 iters)
{
    char a[32];
    char b[32];
    char name[64];
    struct stat_info st;

    if (mkdir("/tmp/fz") != 0 && stat("/tmp/fz", &st) != 0) {
        fail("cannot create /tmp/fz");
        return;
    }
    for (u64 i = 0; i < iters; ++i) {
        if ((i & 255) == 0) {
            heartbeat("model", i);
        }
        put_path(a, rng_below(64), (int)rng_below(3) - 1);
        switch (rng_below(8)) {
        case 0:
        case 1: {
            int fd = open(a, O_CREAT | O_TRUNC | O_RDWR);

            if (fd >= 0) {
                char w[24];

                for (u64 k = 0; k < sizeof(w); ++k) {
                    w[k] = (char)rng_next();
                }
                write(fd, w, (size_t)rng_below(sizeof(w) + 1));
                close(fd);
            }
            break;
        }
        case 2:
            mkdir(a);
            break;
        case 3:
            unlink(a);
            break;
        case 4:
            put_path(b, rng_below(64), (int)rng_below(3) - 1);
            rename(a, b);
            break;
        case 5:
            rmtree(a);
            break;
        case 6:
            readdir("/tmp/fz", rng_below(20), name, sizeof(name));
            stat(a, &st);
            break;
        default:
            if (stat(a, &st) == 0) {
                chmod(a, (u32)rng_below(0777));
            }
            break;
        }
    }
    {
        int r = rmtree("/tmp/fz");

        if (r < 0) {
            fail("final rmtree failed");
            return;
        }
    }
    if (stat("/tmp/fz", &st) == 0) {
        fail("/tmp/fz survived rmtree");
        return;
    }
    heartbeat("model", iters);
}

static void phase_exhaust(void)
{
    int fds[80];
    int n = 0;

    heartbeat("exhaust", 0);
    if (mkdir("/tmp/fz2") != 0) {
        fail("cannot create /tmp/fz2");
        return;
    }
    for (int i = 0; i < 80; ++i) {
        fds[i] = open("/tmp/fz2/f", O_CREAT | O_TRUNC | O_RDWR);
        if (fds[i] < 0) {
            break;
        }
        ++n;
    }
    if (n < 8) {
        fail("fd table did not fill");
        return;
    }
    if (open("/tmp/fz2/f", 0) >= 0) {
        fail("open succeeded with a full table");
        return;
    }
    for (int i = 0; i < n; ++i) {
        close(fds[i]);
    }
    if (rmtree("/tmp/fz2") < 0) {
        fail("cannot remove /tmp/fz2");
        return;
    }
    if (spawn("/bin/nope", "") != -2) {
        fail("spawn of a missing program did not fail ENOENT");
        return;
    }
    {
        int status = 0;

        if (wait(9999, &status) != -10) {
            fail("wait for a stranger did not fail");
            return;
        }
    }
    if (kill(9999) != -3) {
        fail("kill of a stranger did not fail");
        return;
    }
    heartbeat("exhaust", 1);
}

static void phase_accounting(void)
{
    struct meminfo before;
    struct meminfo after;

    heartbeat("accounting", 0);
    if (meminfo(&before) != 0) {
        fail("meminfo failed");
        return;
    }
    if (mkdir("/tmp/fza") != 0) {
        fail("cannot create /tmp/fza");
        return;
    }
    for (int i = 0; i < 100; ++i) {
        char p[24] = "/tmp/fza/f";
        int fd;

        p[10] = (char)('0' + (i / 10));
        p[11] = (char)('0' + (i % 10));
        p[12] = '\0';
        fd = open(p, O_CREAT | O_TRUNC | O_RDWR);
        if (fd < 0) {
            fail("accounting create failed");
            return;
        }
        write(fd, "0123456789abcdef", 16);
        close(fd);
        if (unlink(p) != 0) {
            fail("accounting unlink failed");
            return;
        }
    }
    if (rmtree("/tmp/fza") < 0) {
        fail("cannot remove /tmp/fza");
        return;
    }
    if (meminfo(&after) != 0) {
        fail("meminfo failed");
        return;
    }
    if (after.live_nodes != before.live_nodes) {
        puts("fuzz: live_nodes before=");
        put_u64(before.live_nodes);
        puts(" after=");
        put_u64(after.live_nodes);
        puts("\n");
        fail("VFS nodes leaked");
        return;
    }
    if (after.heap_used != before.heap_used) {
        puts("fuzz: heap_used before=");
        put_u64(before.heap_used);
        puts(" after=");
        put_u64(after.heap_used);
        puts("\n");
        fail("kernel heap leaked");
        return;
    }
    heartbeat("accounting", 1);
}

static u64 parse_u64(const char *s)
{
    u64 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u64)(*s - '0');
        ++s;
    }
    return v;
}

int main(const char *args, size_t len)
{
    u64 seed = 1;
    u64 iters = 2000;

    (void)len;
    if (args && *args) {
        seed = parse_u64(args);
        while (*args >= '0' && *args <= '9') {
            ++args;
        }
        while (*args == ' ') {
            ++args;
        }
        if (*args) {
            iters = parse_u64(args);
            if (iters > 20000) {
                iters = 20000;
            }
        }
    }
    puts("fuzz: start seed=");
    put_u64(seed);
    puts(" iters=");
    put_u64(iters);
    puts("\n");
    rng_seed(seed);
    phase_hostile();
    if (!failures) {
        phase_model(iters);
    }
    if (!failures) {
        phase_exhaust();
    }
    if (!failures) {
        phase_accounting();
    }
    if (!failures) {
        puts("fuzz: PASS\n");
        return 0;
    }
    return 1;
}
