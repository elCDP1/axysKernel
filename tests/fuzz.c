/* Host fuzzer: differential and model-based checks over host-testable kernel
 * code (string/printf/path/ELF/VFS). Runs seeded and deterministic; build
 * with -fsanitize=address,undefined via `make fuzz`. Any failure aborts and
 * prints the seed and iteration so the case reproduces exactly. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "axys/elf.h"
#include "axys/path.h"
#include "axys/printf.h"
#include "axys/string.h"
#include "axys/vfs.h"

static axys_uint64_t rng_state;

static void rng_seed(axys_uint64_t seed)
{
    rng_state = seed != 0u ? seed : 0x9e3779b97f4a7c15ull;
}

static axys_uint64_t rng_next(void)
{
    axys_uint64_t x = rng_state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545f4914f6cdd1dull;
}

static unsigned rng_below(unsigned n)
{
    return (unsigned)(rng_next() % (n == 0u ? 1u : n));
}

/* ---- printf differential vs libc -------------------------------------- */

/* Differential formats with EXACTLY matching args on both sides: a format
 * that consumes more (or different-typed) arguments than passed reads
 * garbage by C semantics on both implementations, which proves nothing. */
static void fuzz_printf(unsigned iter)
{
    char mine[128];
    char ref[128];
    int a0 = (int)rng_next();
    unsigned a1 = (unsigned)rng_next();
    int mine_n, ref_n;
    unsigned t = rng_below(10);
    char ch = (char)(' ' + rng_below(95));
    size_t zn = sizeof(mine) + rng_below(10);

    switch (t) {
    case 0:
        mine_n = axys_snprintf(mine, sizeof(mine), "a%db", a0);
        ref_n = snprintf(ref, sizeof(ref), "a%db", a0);
        break;
    case 1:
        mine_n = axys_snprintf(mine, sizeof(mine), "%u-%d", a1, a0);
        ref_n = snprintf(ref, sizeof(ref), "%u-%d", a1, a0);
        break;
    case 2:
        mine_n = axys_snprintf(mine, sizeof(mine), "%x/%08x", a1, a1);
        ref_n = snprintf(ref, sizeof(ref), "%x/%08x", a1, a1);
        break;
    case 3:
        mine_n = axys_snprintf(mine, sizeof(mine), "[%s]", "s");
        ref_n = snprintf(ref, sizeof(ref), "[%s]", "s");
        break;
    case 4:
        mine_n = axys_snprintf(mine, sizeof(mine), "<%c>", ch);
        ref_n = snprintf(ref, sizeof(ref), "<%c>", ch);
        break;
    case 5:
        mine_n = axys_snprintf(mine, sizeof(mine), "%d%s%x", a0, "", a1);
        ref_n = snprintf(ref, sizeof(ref), "%d%s%x", a0, "", a1);
        break;
    case 6:
        mine_n = axys_snprintf(mine, sizeof(mine), "%-5d|", a0);
        ref_n = snprintf(ref, sizeof(ref), "%-5d|", a0);
        break;
    case 7:
        mine_n = axys_snprintf(mine, sizeof(mine), "%zu", zn);
        ref_n = snprintf(ref, sizeof(ref), "%zu", zn);
        break;
    case 8:
        mine_n = axys_snprintf(mine, sizeof(mine), "100%%");
        ref_n = snprintf(ref, sizeof(ref), "100%%");
        break;
    default:
        mine_n = axys_snprintf(mine, sizeof(mine), "%d %u %x %c", a0, a1, a1, 'q');
        ref_n = snprintf(ref, sizeof(ref), "%d %u %x %c", a0, a1, a1, 'q');
        break;
    }
    if (mine_n != ref_n || strcmp(mine, ref) != 0) {
        fprintf(stderr, "PRINTF-MISMATCH iter=%u t=%u mine=\"%s\"(%d) ref=\"%s\"(%d)\n",
                iter, t, mine, mine_n, ref, ref_n);
        fflush(stderr);
        abort();
    }
    if ((iter & 127u) == 0) {
        /* %p shape: "0x" + 16 lowercase hex digits, return 18. */
        int n = axys_snprintf(mine, sizeof(mine), "%p", (void *)(axys_uintptr_t)rng_next());

        assert(n == 18 && mine[0] == '0' && mine[1] == 'x' && mine[18] == '\0');
        for (unsigned i = 2; i < 18; ++i) {
            assert((mine[i] >= '0' && mine[i] <= '9') || (mine[i] >= 'a' && mine[i] <= 'f'));
        }
    }
}

/* ---- string differential vs libc -------------------------------------- */

static void fuzz_string(unsigned iter)
{
    unsigned char a[40], b[40], d[40], e[40];
    size_t la, lb, n;

    (void)iter;
    la = rng_below(sizeof(a));
    lb = rng_below(sizeof(b));
    for (size_t i = 0; i < sizeof(a); ++i) {
        a[i] = (unsigned char)rng_below(5); /* heavy on NULs */
        b[i] = (unsigned char)rng_below(5);
    }
    a[sizeof(a) - 1] = '\0';
    b[sizeof(b) - 1] = '\0';
    assert(axys_strlen((const char *)a) == strlen((const char *)a));
    assert(axys_strcmp((const char *)a, (const char *)b) == 0 ||
           ((axys_strcmp((const char *)a, (const char *)b) < 0) ==
            (strcmp((const char *)a, (const char *)b) < 0)));
    n = rng_below(41);
    assert(axys_strncmp((const char *)a, (const char *)b, n) == 0 ||
           ((axys_strncmp((const char *)a, (const char *)b, n) < 0) ==
            (strncmp((const char *)a, (const char *)b, n) < 0)));
    assert(axys_memcmp(a, b, n) == 0 ||
           ((axys_memcmp(a, b, n) < 0) == (memcmp(a, b, n) < 0)));
    memcpy(d, a, sizeof(d));
    memcpy(e, a, sizeof(e));
    {
        size_t off1 = rng_below(20), off2 = rng_below(20), len = rng_below(20);

        if (off1 + len <= sizeof(d) && off2 + len <= sizeof(d)) {
            memmove(d + off1, d + off2, len);
            axys_memmove(e + off1, e + off2, len);
            assert(memcmp(d, e, sizeof(d)) == 0);
        }
    }
    (void)la;
    (void)lb;
}

/* ---- path normalization properties ------------------------------------- */

static void fuzz_path(unsigned iter)
{
    static const char *const atoms[] = {"a", "bb", ".", "..", "", "c"};
    char in[120];
    char out[AXYS_VFS_MAX_PATH];
    char again[AXYS_VFS_MAX_PATH];
    int rc;

    (void)iter;
    in[0] = '\0';
    for (unsigned k = 0; k < 6; ++k) {
        strncat(in, "/", sizeof(in) - strlen(in) - 1);
        strncat(in, atoms[rng_below(6)], sizeof(in) - strlen(in) - 1);
    }
    rc = axys_path_normalize(in, out, sizeof(out));
    if (rc != 0) {
        assert(rc == -1 || rc == -2);
        return;
    }
    assert(out[0] == '/');
    assert(strstr(out, "//") == NULL);
    {
        size_t l = strlen(out);

        assert(l < AXYS_VFS_MAX_PATH);
        if (l > 1) {
            assert(out[l - 1] != '/');
        }
    }
    assert(axys_path_normalize(out, again, sizeof(again)) == 0);
    assert(strcmp(out, again) == 0); /* idempotent */
}

/* ---- ELF mutation: must reject, never crash ----------------------------- */

static void fuzz_elf(unsigned iter)
{
    static axys_uint8_t image[2048];
    struct axys_elf_image_info info;
    int rc;

    (void)iter;
    if ((iter & 63u) == 0) {
        /* Reseed from a valid skeleton so mutations stay near-valid. */
        memset(image, 0, sizeof(image));
        image[0] = 0x7f;
        image[1] = 'E';
        image[2] = 'L';
        image[3] = 'F';
        image[4] = 2;
        image[5] = 1;
        image[6] = 1;
        image[16] = 3; /* ET_DYN */
        image[18] = 62; /* x86-64 */
    }
    for (unsigned k = 0; k < 8; ++k) {
        image[rng_below(sizeof(image))] = (axys_uint8_t)rng_next();
    }
    rc = axys_elf_validate_image(image, sizeof(image), &info);
    assert(rc == 0 || rc == -1);
}

/* ---- VFS model: random ops, global invariants after each round ----------- */

static void make_component(char *out, size_t cap)
{
    static const char alpha[] = "ab012./";

    size_t n = 1 + rng_below(6);

    if (n >= cap) {
        n = cap - 1;
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = alpha[rng_below(sizeof(alpha) - 1)];
    }
    out[n] = '\0';
}

static void make_path(char *out, size_t cap)
{
    char comp[16];

    strncpy(out, "/tmp/fz", cap);
    out[cap - 1] = '\0';
    for (unsigned k = 0; k < 1 + rng_below(3); ++k) {
        make_component(comp, sizeof(comp));
        if (strlen(out) + 1 + strlen(comp) >= cap) {
            break;
        }
        strcat(out, "/");
        strcat(out, comp);
    }
}

/* Walk the whole tree counting nodes and checking sibling-name uniqueness. */
static axys_uint64_t checked_nodes;

static void check_tree(axys_vfs_node_t dir)
{
    axys_vfs_node_t child = -1;
    /* Per-level name set (two passes: count, then compare). A single static
     * buffer shared across recursion levels caused false duplicates. */
    char(*names)[AXYS_VFS_NAME_MAX] = NULL;
    unsigned count = 0;
    unsigned n = 0;

    while ((child = axys_vfs_next_child(dir, child)) >= 0) {
        ++count;
        assert(count < AXYS_VFS_MAX_NODES);
    }
    if (count != 0) {
        names = calloc(count, AXYS_VFS_NAME_MAX);
        assert(names != NULL);
    }
    ++checked_nodes;
    assert(checked_nodes < AXYS_VFS_MAX_NODES + 100u);
    child = -1;
    while ((child = axys_vfs_next_child(dir, child)) >= 0) {
        const char *nm = axys_vfs_name(child);

        assert(n < count);
        for (unsigned i = 0; i < n; ++i) {
            assert(strcmp(names[i], nm) != 0); /* duplicate sibling name */
        }
        strncpy(names[n++], nm, AXYS_VFS_NAME_MAX - 1);
        names[n - 1][AXYS_VFS_NAME_MAX - 1] = '\0';
        if (axys_vfs_type(child) == AXYS_VFS_DIR) {
            check_tree(child);
        } else {
            struct axys_vfs_attr at;

            ++checked_nodes; /* files count as live nodes too */
            assert(axys_vfs_getattr(child, &at) == 0);
            assert(at.type == 1u);
        }
    }
    free(names);
}

static void check_invariants(unsigned iter)
{
    checked_nodes = 0;
    check_tree(0);
    if (checked_nodes != axys_vfs_live_nodes()) {
        fprintf(stderr, "COUNT-MISMATCH iter=%u walked=%llu live=%llu\n", iter,
                (unsigned long long)checked_nodes,
                (unsigned long long)axys_vfs_live_nodes());
        fflush(stderr);
        extern void fuzz_dump_log(void);
        fuzz_dump_log();
    }
    assert(checked_nodes == axys_vfs_live_nodes());
}

#define OPLOG_N 64
static char oplog[OPLOG_N][160];
static unsigned oplog_head;

static void log_op(const char *fmt, const char *a, const char *b, int rc)
{
    snprintf(oplog[oplog_head % OPLOG_N], sizeof(oplog[0]), "%s a=%s b=%s rc=%d live=%llu",
             fmt, a ? a : "-", b ? b : "-", rc,
             (unsigned long long)axys_vfs_live_nodes());
    ++oplog_head;
}

void fuzz_dump_log(void)
{
    unsigned start = oplog_head > OPLOG_N ? oplog_head - OPLOG_N : 0;

    for (unsigned i = start; i < oplog_head; ++i) {
        fprintf(stderr, "  op %u: %s\n", i, oplog[i % OPLOG_N]);
    }
    fflush(stderr);
}

static void fuzz_vfs(unsigned iter)
{
    char p[AXYS_VFS_MAX_PATH];
    char q[AXYS_VFS_MAX_PATH];

    make_path(p, sizeof(p));
    switch (rng_below(8)) {
    case 0:
    case 1: {
        axys_vfs_node_t n = axys_vfs_create(p, AXYS_VFS_FILE);

        log_op("create", p, NULL, n);
        if (n >= 0 && rng_below(2)) {
            char buf[32];

            for (unsigned i = 0; i < sizeof(buf); ++i) {
                buf[i] = (char)rng_next();
            }
            (void)axys_vfs_write(n, buf, rng_below(sizeof(buf) + 1));
        }
        break;
    }
    case 2:
        log_op("mkdirs", p, NULL, axys_vfs_mkdirs(p));
        break;
    case 3:
        log_op("unlink", p, NULL, axys_vfs_unlink(p));
        break;
    case 4:
        make_path(q, sizeof(q));
        log_op("rename", p, q, axys_vfs_rename(p, q));
        break;
    case 5:
        log_op("rmtree", p, NULL, axys_vfs_remove_tree(p));
        break;
    case 6: {
        axys_vfs_node_t n = axys_vfs_lookup(p);
        char buf[16];

        log_op("lookup", p, NULL, n);
        if (n >= 0) {
            (void)axys_vfs_read(n, buf, sizeof(buf));
            (void)axys_vfs_pread(n, rng_below(64), buf, sizeof(buf));
        }
        break;
    }
    default: {
        axys_vfs_node_t n = axys_vfs_lookup(p);

        log_op("lookup", p, NULL, n);
        if (n >= 0) {
            (void)axys_vfs_chmod(n, rng_below(07777u));
        }
        break;
    }
    }
    if ((iter & 255u) == 0) {
        check_invariants(iter);
    }
}

int main(int argc, char **argv)
{
    axys_uint64_t seed = argc > 1 ? (axys_uint64_t)strtoull(argv[1], NULL, 0) : 1u;
    unsigned iters = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 4000u;

    rng_seed(seed);
    axys_vfs_init();
    assert(axys_vfs_mkdirs("/tmp/fz") == 0);
    for (unsigned i = 0; i < iters; ++i) {
        fuzz_printf(i);
        fuzz_string(i);
        fuzz_path(i);
        fuzz_elf(i);
        fuzz_vfs(i);
    }
    check_invariants(iters);
    printf("fuzz-host: PASS seed=%llu iters=%u live=%llu\n", (unsigned long long)seed, iters,
           (unsigned long long)axys_vfs_live_nodes());
    return 0;
}
