#include "axys.h"
#include "axys/timeconv.h"
#include "shparse.h"

/* /sbin/init: a small interactive shell in the spirit of busybox.
 *
 * Builtins mirror embedded-linux commands with the same behaviour, minus
 * what the kernel does not provide yet (no pipes, no symlinks, no ps/dmesg:
 * there is no process-list or log-buffer syscall, so `help` says so
 * instead of faking it). Anything showing driver state reads it live --
 * net_stat for `ip`, meminfo for `free`, /proc snapshots for the rest --
 * so no command prints canned text. Paths may be absolute or relative to
 * the shell's working directory (`cd`, `pwd`); the kernel canonicalizes
 * //, /./ and /../ on use, and the shell keeps cwd canonical itself. */

#define LINE_MAX 256
#define PATH_MAX 256
#define IO_BUF 256

/* Errno values the shell compares against (mirrors include/axys/process.h;
 * user space has no errno header, same as the other /bin programs). */
#define E_NOENT 2
#define E_ISDIR 21
#define E_NOSPC 28
#define E_NODEV 19
#define E_INVAL 22

/* Diagnostics go to fd 2 so that `cmd > file` and pipes carry only data. */
static void eputs(const char *s)
{
    write(2, s, strlen(s));
}

static void print_status(const char *what, long value)
{
    char num[24];
    int n = 0;
    u64 v = value < 0 ? (u64)-value : (u64)value;

    eputs(what);
    eputs(": ");
    if (value < 0) {
        eputs("-");
    }
    do {
        num[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        write(2, &num[--n], 1);
    }
    eputs("\n");
}

static int run(const char *path, const char *args)
{
    int status = 0;
    int pid = spawn(path, args);

    if (pid < 0) {
        print_status("spawn failed", pid);
        return pid;
    }
    if (wait(pid, &status) < 0) {
        puts("wait failed\n");
        return -1;
    }
    if (status != 0) {
        if (status >= 128 && status < 128 + 32) {
            puts("[killed by exception ");
            put_u64((u64)(status - 128));
            puts("]\n");
        } else {
            puts("[exit ");
            put_u64((u64)status);
            puts("]\n");
        }
    }
    return status;
}

static void put_mode(u32 type, u32 mode)
{
    char m[11];

    m[0] = type == 2 ? 'd' : '-';
    for (int k = 0; k < 9; ++k) {
        m[1 + k] = (mode & (1u << (8 - k))) ? "rwx"[k % 3] : '-';
    }
    m[10] = '\0';
    puts(m);
}

/* Four octal digits with a leading 0 ("0644"), for `stat`. */
static void put_octal(u32 value)
{
    char buf[6];

    buf[0] = '0';
    for (int i = 0; i < 4; ++i) {
        buf[1 + i] = (char)('0' + ((value >> (9 - 3 * i)) & 7));
    }
    buf[5] = '\0';
    puts(buf);
}

/* One byte as two lowercase hex digits, for MAC addresses. */
static void put_hex2(u8 value)
{
    char buf[3];

    buf[0] = "0123456789abcdef"[value >> 4];
    buf[1] = "0123456789abcdef"[value & 15];
    buf[2] = '\0';
    puts(buf);
}

static void put_ip(const u8 ip[4])
{
    for (int i = 0; i < 4; ++i) {
        if (i) {
            puts(".");
        }
        put_u64(ip[i]);
    }
}

/* Signed decimal (pids can be -1, seq prints 64-bit). The negation avoids
 * -LONG_MIN by going through unsigned arithmetic (gcc wraps, defined). */
static void put_int(long value)
{
    if (value < 0) {
        puts("-");
        put_u64((u64)(-(value + 1)) + 1u);
    } else {
        put_u64((u64)value);
    }
}

/* Bytes as "N", "N KiB", "N MiB" or "N GiB" (truncated, for free/df/du -h). */
static void put_human(u64 bytes)
{
    if (bytes >= (1ull << 30) && (bytes & ((1ull << 30) - 1)) == 0) {
        put_u64(bytes >> 30);
        puts(" GiB");
    } else if (bytes >= (1ull << 20) && (bytes & ((1ull << 20) - 1)) == 0) {
        put_u64(bytes >> 20);
        puts(" MiB");
    } else if (bytes >= 1024 && (bytes & 1023) == 0) {
        put_u64(bytes >> 10);
        puts(" KiB");
    } else {
        put_u64(bytes);
        puts(" B");
    }
}

/* Lowercase hex, exactly `digits` nibbles (offsets for hexdump). */
static void put_hex_n(u64 value, int digits)
{
    char buf[17];
    int i;

    if (digits < 1) {
        digits = 1;
    }
    if (digits > 16) {
        digits = 16;
    }
    for (i = 0; i < digits; ++i) {
        buf[i] = "0123456789abcdef"[(value >> (4 * (digits - 1 - i))) & 15];
    }
    buf[digits] = '\0';
    puts(buf);
}

/* ---- working directory ---------------------------------------------- */

static char cwd[PATH_MAX] = "/";

/* Collapse an absolute path in place of `out`: //, /./ and /../ (clamped at
 * /), no trailing slash except root. `in` must start with '/'. */
static void path_normalize(const char *in, char *out)
{
    size_t mark[64];
    size_t depth = 0;
    size_t top = 1;
    size_t i = 0;

    out[0] = '/';
    out[1] = '\0';
    while (in[i] != '\0') {
        size_t start, len;

        while (in[i] == '/') {
            ++i;
        }
        if (in[i] == '\0') {
            break;
        }
        start = i;
        while (in[i] != '\0' && in[i] != '/') {
            ++i;
        }
        len = i - start;
        if (len == 1 && in[start] == '.') {
            continue;
        }
        if (len == 2 && in[start] == '.' && in[start + 1] == '.') {
            if (depth > 0) {
                --depth;
                top = mark[depth];
                out[top] = '\0';
            }
            continue;
        }
        if (depth < 64) {
            mark[depth++] = top;
        } else {
            continue; /* absurdly deep: let the kernel refuse it */
        }
        if (top > 1) {
            out[top++] = '/';
        }
        if (top + len >= PATH_MAX) {
            len = PATH_MAX - 1 - top; /* truncate safely, kernel errors out */
        }
        memcpy(out + top, in + start, len);
        top += len;
        out[top] = '\0';
    }
}

/* Absolute paths pass through (normalized); relative ones join cwd first. */
static void resolve(const char *in, char *out)
{
    if (in[0] == '/') {
        path_normalize(in, out);
        return;
    }
    {
        char tmp[PATH_MAX];
        size_t c = strlen(cwd);
        size_t n = strlen(in);
        size_t t;

        if (c + 1 + n >= sizeof(tmp)) {
            n = sizeof(tmp) - c - 2;
        }
        memcpy(tmp, cwd, c);
        t = c;
        tmp[t++] = '/';
        memcpy(tmp + t, in, n);
        t += n;
        tmp[t] = '\0';
        path_normalize(tmp, out);
    }
}

/* Carve the next whitespace-separated token off *p (NUL-terminates it).
 * Returns 0 when nothing is left. */
static void cmd_cat_proc(const char *name, const char *path);

static void ls_one(const char *path);
static void ls_recursive(const char *path);

static int ls_long; /* -l */

static void ls_print(const char *name, const struct stat_info *st)
{
    if (ls_long) {
        put_mode(st->type, st->mode);
        puts(" ");
        put_u64(st->uid);
        puts(" ");
        put_u64(st->gid);
        puts(" ");
        put_u64(st->size);
        puts("\t");
    }
    puts(name);
    puts(st->type == 2 ? "/\n" : "\n");
}

/* List one path: the entries of a directory, or the file itself. */
static void ls_one(const char *path)
{
    char name[64];
    char child[PATH_MAX];
    struct stat_info st;

    if (stat(path, &st) == 0 && st.type != 2) {
        ls_print(path, &st);
        return;
    }
    for (u64 i = 0;; ++i) {
        int r = readdir(path, i, name, sizeof(name));
        size_t pl = strlen(path);

        if (r < 0) {
            print_status(path, r); /* name the path: `ls a b` must say which one failed */
            return;
        }
        if (r == 0) {
            return;
        }
        st.type = (u32)r;
        st.mode = st.uid = st.gid = st.size = 0;
        if (pl + strlen(name) + 2 < sizeof(child)) {
            memcpy(child, path, pl);
            if (pl == 0 || child[pl - 1] != '/') {
                child[pl++] = '/';
            }
            memcpy(child + pl, name, strlen(name) + 1);
            if (stat(child, &st) != 0) {
                st.type = (u32)r;
            }
        }
        ls_print(name, &st);
    }
}

/* ls [-l] [-R] [PATH...]: a header per path only when listing several. */
static void cmd_ls(char *rest)
{
    char *paths[16];
    int count = 0;
    int recursive = 0;
    char *arg;

    ls_long = 0;
    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (arg[0] == '-' && arg[1] != '\0') {
            for (const char *f = arg + 1; *f; ++f) {
                if (*f == 'l') ls_long = 1;
                else if (*f == 'R') recursive = 1;
                else if (*f == 'a') { /* no hidden files here: accepted for habit */ }
                else {
                    print_status("ls", -E_INVAL);
                    return;
                }
            }
        } else if (count < 16) {
            paths[count++] = arg;
        } else {
            print_status("ls", -E_INVAL);
            return;
        }
    }
    if (count == 0) {
        char path[PATH_MAX];

        memcpy(path, cwd, strlen(cwd) + 1);
        if (recursive) {
            ls_recursive(path);
        } else {
            ls_one(path);
        }
        return;
    }
    for (int i = 0; i < count; ++i) {
        char path[PATH_MAX];

        resolve(paths[i], path);
        if (count > 1 && !recursive) {
            if (i > 0) {
                puts("\n");
            }
            puts(paths[i]);
            puts(":\n");
        }
        if (recursive) {
            ls_recursive(path);
        } else {
            ls_one(path);
        }
    }
}

/* ls -R: this directory, then each subdirectory in turn. */
static void ls_recursive(const char *path)
{
    char name[64];
    char child[PATH_MAX];
    struct stat_info st;

    puts(path);
    puts(":\n");
    ls_one(path);
    for (u64 i = 0;; ++i) {
        int r = readdir(path, i, name, sizeof(name));
        size_t pl;

        if (r <= 0) {
            return;
        }
        if (r != 2) {
            continue;
        }
        pl = strlen(path);
        if (pl + strlen(name) + 2 >= sizeof(child)) {
            continue;
        }
        memcpy(child, path, pl);
        if (pl == 0 || child[pl - 1] != '/') {
            child[pl++] = '/';
        }
        memcpy(child + pl, name, strlen(name) + 1);
        if (stat(child, &st) == 0 && st.type == 2) {
            puts("\n");
            ls_recursive(child);
        }
    }
}

/* Parse an unsigned number. Returns 0 and stores the value, or -1 when there
 * is no digit or the value overflows 32 bits. Silently wrapping or returning 0
 * turned typos like "su abc" or "chown 4294967297 f" into uid 0 / uid 1. */
static int parse_num(const char **s, u32 base, u32 *out)
{
    u64 v = 0;
    int digits = 0;

    while (**s >= '0' && **s < (char)('0' + (base > 10 ? 10 : base))) {
        v = v * base + (u64)(**s - '0');
        if (v > 0xffffffffull) {
            return -1;
        }
        ++*s;
        ++digits;
    }
    if (digits == 0 || (**s != '\0' && **s != ' ')) {
        return -1;
    }
    *out = (u32)v;
    return 0;
}

/* cat [-n] FILE...: concatenation keeps one numbering/begin state, like GNU. */
static void cat_stream(int fd, int numbered, u64 *lineno, int *bol)
{
    char buf[128];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        size_t i = 0;

        if (!numbered) {
            write(1, buf, (size_t)n);
            continue;
        }
        while (i < (size_t)n) {
            size_t j = i;

            if (*bol) {
                put_u64((*lineno)++);
                puts("\t");
                *bol = 0;
            }
            while (j < (size_t)n && buf[j] != '\n') {
                ++j;
            }
            if (j < (size_t)n) {
                ++j; /* include the newline */
                *bol = 1;
            }
            write(1, buf + i, j - i);
            i = j;
        }
    }
}

static void cmd_cat(char *rest)
{
    char path[PATH_MAX];
    int fd;
    int numbered = 0;
    u64 lineno = 1;
    int bol = 1;
    char *file;

    if (rest[0] == '-' && rest[1] == 'n' && (rest[2] == ' ' || rest[2] == '\0')) {
        numbered = 1;
        rest += 2;
        while (*rest == ' ') {
            ++rest;
        }
    }
    file = next_arg(&rest);
    if (!file) {
        cat_stream(0, numbered, &lineno, &bol); /* no FILE: copy stdin */
        return;
    }
    for (; file; file = next_arg(&rest)) {
        resolve(file, path);
        fd = open(path, 0);
        if (fd < 0) {
            print_status("cat", fd);
            return;
        }
        cat_stream(fd, numbered, &lineno, &bol);
        close(fd);
    }
}

/* echo [-n] TEXT [> FILE | >> FILE] */
static void cmd_echo(char *rest)
{
    int nonl = 0;
    int first = 1;
    int printed = 0;
    char *arg;

    /* Redirection (`echo hi > file`) is handled for every command by the
     * pipeline layer; echo just prints its (unquoted) words. */
    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (first && strcmp(arg, "-n") == 0) {
            nonl = 1;
            first = 0;
            continue;
        }
        first = 0;
        if (printed) {
            puts(" ");
        }
        puts(arg);
        printed = 1;
    }
    if (!nonl) {
        puts("\n");
    }
}
static void cmd_touch(char *rest)
{
    char *arg = next_arg(&rest);
    char path[PATH_MAX];

    if (!arg) {
        print_status("touch", -E_INVAL);
        return;
    }
    for (; arg; arg = next_arg(&rest)) {
        int fd;

        resolve(arg, path);
        fd = open(path, O_CREAT | O_WRONLY);
        if (fd < 0) {
            print_status("touch", fd);
            return;
        }
        close(fd);
    }
}

/* Copy one regular file. Returns 0, or a negative errno. */
static int cp_file(const char *spath, const char *dpath)
{
    struct stat_info st;
    char buf[IO_BUF];
    int in, out;
    ssize_t n;

    if (stat(spath, &st) != 0 || st.type != 1) {
        return -E_NOENT;
    }
    in = open(spath, 0);
    if (in < 0) {
        return in;
    }
    out = open(dpath, O_CREAT | O_TRUNC | O_WRONLY);
    if (out < 0) {
        close(in);
        return out;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            close(in);
            close(out);
            return -12;
        }
    }
    close(in);
    close(out);
    if (n < 0) {
        return (int)n;
    }
    (void)chmod(dpath, st.mode); /* keep the mode, best effort */
    return 0;
}

/* Recursive copy of the tree at spath onto dpath. */
static int cp_tree(const char *spath, const char *dpath, int depth)
{
    struct stat_info st;
    char name[64];
    char sp[PATH_MAX], dp[PATH_MAX];

    if (stat(spath, &st) != 0) {
        return -E_NOENT;
    }
    if (st.type != 2) {
        return cp_file(spath, dpath);
    }
    if (depth > 16) {
        return -E_INVAL;
    }
    {
        int r = mkdir(dpath);

        if (r != 0 && r != -17) { /* exists: reuse it */
            return r;
        }
    }
    for (u64 i = 0;; ++i) {
        int r = readdir(spath, i, name, sizeof(name));
        size_t sl = strlen(spath);
        size_t dl = strlen(dpath);

        if (r < 0) {
            return r;
        }
        if (r == 0) {
            return 0;
        }
        if (sl + strlen(name) + 2 >= sizeof(sp) || dl + strlen(name) + 2 >= sizeof(dp)) {
            return -E_INVAL;
        }
        memcpy(sp, spath, sl);
        sp[sl++] = '/';
        memcpy(sp + sl, name, strlen(name) + 1);
        memcpy(dp, dpath, dl);
        dp[dl++] = '/';
        memcpy(dp + dl, name, strlen(name) + 1);
        r = cp_tree(sp, dp, depth + 1);
        if (r != 0) {
            return r;
        }
    }
}

static void cmd_cp(char *rest)
{
    char *src = next_arg(&rest);
    char *dst;
    char spath[PATH_MAX], dpath[PATH_MAX];
    int recursive = 0;
    int r;

    if (src && strcmp(src, "-r") == 0) {
        recursive = 1;
        src = next_arg(&rest);
    }
    dst = next_arg(&rest);
    if (!src || !dst || next_arg(&rest)) {
        print_status("cp", -E_INVAL);
        return;
    }
    resolve(src, spath);
    resolve(dst, dpath);
    if (recursive) {
        r = cp_tree(spath, dpath, 0);
    } else {
        r = cp_file(spath, dpath);
    }
    if (r != 0) {
        print_status("cp", r);
    }
}

/* head [-N | -n N] FILE, tail [-N | -n N] FILE. tail rewinds and skips:
 * two streaming passes, no heap, works on any file size. */
static void cmd_head_tail(char *rest, int is_tail)
{
    const char *name = is_tail ? "tail" : "head";
    char *tok = next_arg(&rest);
    char *file = tok;
    u32 n = 10;
    char path[PATH_MAX];
    char buf[IO_BUF];
    int fd;
    ssize_t r;

    if (tok && tok[0] == '-') {
        if (tok[1] == 'n' && tok[2] == '\0') {
            const char *a = next_arg(&rest);
            u32 v = 0;

            if (!a || parse_num(&a, 10, &v) != 0) {
                print_status(name, -E_INVAL);
                return;
            }
            n = v;
        } else {
            const char *a = tok + 1;
            u32 v = 0;

            if (parse_num(&a, 10, &v) != 0) {
                print_status(name, -E_INVAL);
                return;
            }
            n = v;
        }
        file = next_arg(&rest);
    }
    if (file && next_arg(&rest)) {
        print_status(name, -E_INVAL);
        return;
    }
    if (file) {
        resolve(file, path);
        fd = open(path, 0);
        if (fd < 0) {
            print_status(name, fd);
            return;
        }
    } else {
        fd = 0; /* no FILE: read stdin */
    }
    if (!is_tail) {
        u32 lines = 0;

        while (lines < n && (r = read(fd, buf, sizeof(buf))) > 0) {
            size_t stop = (size_t)r;

            for (size_t i = 0; i < (size_t)r && lines < n; ++i) {
                if (buf[i] == '\n') {
                    ++lines;
                    stop = i + 1;
                }
            }
            write(1, buf, stop);
        }
        close(fd);
        return;
    }
    {
        /* Pass 1: count lines (a trailing partial line counts). */
        u64 total = 0;
        int partial = 0;

        while ((r = read(fd, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < r; ++i) {
                if (buf[i] == '\n') {
                    ++total;
                    partial = 0;
                } else {
                    partial = 1;
                }
            }
        }
        if (partial) {
            ++total;
        }
        if (lseek(fd, 0, 0) < 0) {
            print_status(name, -5);
            close(fd);
            return;
        }
        /* Pass 2: print from line (total - n). */
        {
            u64 start = total > n ? total - n : 0;
            u64 line = 0;
            int skipping = start > 0;

            while ((r = read(fd, buf, sizeof(buf))) > 0) {
                size_t from = 0;

                if (skipping) {
                    for (ssize_t i = 0; i < r; ++i) {
                        if (buf[i] == '\n' && ++line >= start) {
                            from = (size_t)i + 1;
                            skipping = 0;
                            break;
                        }
                    }
                    if (skipping) {
                        continue;
                    }
                }
                write(1, buf + from, (size_t)r - from);
            }
        }
        close(fd);
    }
}

static void put_u64_pad(u64 v, int width)
{
    char num[24];
    int n = 0;

    do {
        num[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    for (int pad = width - n; pad > 0; --pad) {
        puts(" ");
    }
    while (n > 0) {
        write(1, &num[--n], 1);
    }
}

/* Count lines, words and bytes of one stream. Returns 0 or a negative errno. */
static int wc_count(int fd, u64 *lines, u64 *words, u64 *bytes)
{
    char buf[IO_BUF];
    ssize_t r;
    int in_word = 0;

    *lines = *words = *bytes = 0;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        *bytes += (u64)r;
        for (ssize_t i = 0; i < r; ++i) {
            int space = buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r';

            if (buf[i] == '\n') {
                ++*lines;
            }
            if (!space && !in_word) {
                in_word = 1;
                ++*words;
            } else if (space) {
                in_word = 0;
            }
        }
    }
    return r < 0 ? (int)r : 0;
}

/* wc [-l] [-w] [-c] [FILE...]: no FILE reads stdin; several files add a total. */
static void cmd_wc(char *rest)
{
    int want_l = 0, want_w = 0, want_c = 0;
    char *arg;
    char *files[16];
    int nfiles = 0;
    u64 tl = 0, tw = 0, tc = 0;
    int fields;

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (arg[0] == '-' && arg[1] != '\0') {
            for (const char *f = arg + 1; *f; ++f) {
                if (*f == 'l') want_l = 1;
                else if (*f == 'w') want_w = 1;
                else if (*f == 'c') want_c = 1;
                else {
                    print_status("wc", -E_INVAL);
                    return;
                }
            }
        } else if (nfiles < 16) {
            files[nfiles++] = arg;
        } else {
            print_status("wc", -E_INVAL);
            return;
        }
    }
    if (!want_l && !want_w && !want_c) {
        want_l = want_w = want_c = 1;
    }
    fields = want_l + want_w + want_c;
    for (int i = 0; i < (nfiles ? nfiles : 1); ++i) {
        u64 l, w, c;
        int fd = 0;
        int r;
        char path[PATH_MAX];

        if (nfiles) {
            resolve(files[i], path);
            fd = open(path, 0);
            if (fd < 0) {
                print_status(files[i], fd);
                continue;
            }
        }
        r = wc_count(fd, &l, &w, &c);
        if (nfiles) {
            close(fd);
        }
        if (r < 0) {
            print_status(nfiles ? files[i] : "wc", r);
            continue;
        }
        tl += l;
        tw += w;
        tc += c;
        if (want_l) put_u64_pad(l, fields > 1 ? 7 : 0);
        if (want_w) { if (want_l) puts(" "); put_u64_pad(w, fields > 1 ? 7 : 0); }
        if (want_c) { if (want_l || want_w) puts(" "); put_u64_pad(c, fields > 1 ? 7 : 0); }
        if (nfiles) {
            puts(" ");
            puts(files[i]);
        }
        puts("\n");
    }
    if (nfiles > 1) {
        if (want_l) put_u64_pad(tl, fields > 1 ? 7 : 0);
        if (want_w) { if (want_l) puts(" "); put_u64_pad(tw, fields > 1 ? 7 : 0); }
        if (want_c) { if (want_l || want_w) puts(" "); put_u64_pad(tc, fields > 1 ? 7 : 0); }
        puts(" total\n");
    }
}

/* grep: substring match (no regex). Options -i -n -v -c -r; with no FILE the
 * input is stdin, so `cmd | grep text` works. Lines longer than LINE_MAX-1 are
 * matched on their first LINE_MAX-1 bytes. */
struct grep_opts {
    int icase, number, invert, count, show_name;
};

static char lower_c(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int line_matches(const char *line, size_t len, const char *pat, size_t plen, int icase)
{
    if (plen == 0) {
        return 1;
    }
    for (size_t i = 0; i + plen <= len; ++i) {
        size_t k = 0;

        for (; k < plen; ++k) {
            char x = line[i + k];
            char y = pat[k];

            if (icase) {
                x = lower_c(x);
                y = lower_c(y);
            }
            if (x != y) {
                break;
            }
        }
        if (k == plen) {
            return 1;
        }
    }
    return 0;
}

static void grep_emit(const struct grep_opts *o, const char *name, u64 lineno, const char *line, size_t len)
{
    if (o->show_name) {
        puts(name);
        puts(":");
    }
    if (o->number) {
        put_u64(lineno);
        puts(":");
    }
    write(1, line, len);
    puts("\n");
}

static int grep_stream(int fd, const char *name, const char *pat, size_t plen, const struct grep_opts *o)
{
    char buf[IO_BUF];
    char line[LINE_MAX];
    size_t llen = 0;
    u64 lineno = 0;
    int hits = 0;
    int partial = 0;
    ssize_t r;

    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < r; ++i) {
            if (buf[i] != '\n') {
                if (llen + 1 < sizeof(line)) {
                    line[llen++] = buf[i];
                }
                partial = 1;
                continue;
            }
            ++lineno;
            if (line_matches(line, llen, pat, plen, o->icase) != o->invert) {
                ++hits;
                if (!o->count) {
                    grep_emit(o, name, lineno, line, llen);
                }
            }
            llen = 0;
            partial = 0;
        }
    }
    if (r == 0 && partial) { /* last line without a newline */
        ++lineno;
        if (line_matches(line, llen, pat, plen, o->icase) != o->invert) {
            ++hits;
            if (!o->count) {
                grep_emit(o, name, lineno, line, llen);
            }
        }
    }
    if (r < 0) {
        return (int)r;
    }
    if (o->count) {
        if (o->show_name) {
            puts(name);
            puts(":");
        }
        put_u64((u64)hits);
        puts("\n");
    }
    return hits;
}

static int grep_path(const char *path, const char *pat, size_t plen, const struct grep_opts *o, int recursive,
                     int depth)
{
    struct stat_info st;
    int total = 0;

    if (stat(path, &st) != 0) {
        return -E_NOENT;
    }
    if (st.type != 2) {
        int fd = open(path, 0);
        int r;

        if (fd < 0) {
            return fd;
        }
        r = grep_stream(fd, path, pat, plen, o);
        close(fd);
        return r;
    }
    if (!recursive) {
        return -E_ISDIR;
    }
    if (depth > 16) {
        return 0;
    }
    for (u64 i = 0;; ++i) {
        char name[64];
        char child[PATH_MAX];
        size_t pl = strlen(path);
        int e = readdir(path, i, name, sizeof(name));

        if (e <= 0) {
            return total;
        }
        if (pl + strlen(name) + 2 >= PATH_MAX) {
            continue;
        }
        memcpy(child, path, pl);
        if (pl == 0 || child[pl - 1] != '/') {
            child[pl++] = '/';
        }
        memcpy(child + pl, name, strlen(name) + 1);
        {
            int r = grep_path(child, pat, plen, o, recursive, depth + 1);

            if (r > 0) {
                total += r;
            }
        }
    }
}

static void cmd_grep(char *rest)
{
    struct grep_opts o = {0, 0, 0, 0, 0};
    int recursive = 0;
    char *arg;
    char *pat = 0;
    int nfiles = 0;
    char *files[16];

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (!pat && arg[0] == '-' && arg[1] != '\0') {
            for (const char *f = arg + 1; *f; ++f) {
                if (*f == 'i') o.icase = 1;
                else if (*f == 'n') o.number = 1;
                else if (*f == 'v') o.invert = 1;
                else if (*f == 'c') o.count = 1;
                else if (*f == 'r') recursive = 1;
                else {
                    print_status("grep", -E_INVAL);
                    return;
                }
            }
        } else if (!pat) {
            pat = arg;
        } else if (nfiles < 16) {
            files[nfiles++] = arg;
        } else {
            print_status("grep", -E_INVAL);
            return;
        }
    }
    if (!pat) {
        print_status("grep", -E_INVAL);
        return;
    }
    o.show_name = nfiles > 1 || recursive;
    if (nfiles == 0) {
        int r = grep_stream(0, "(standard input)", pat, strlen(pat), &o);

        if (r < 0) {
            print_status("grep", r);
        }
        return;
    }
    for (int i = 0; i < nfiles; ++i) {
        char path[PATH_MAX];
        int r;

        resolve(files[i], path);
        r = grep_path(path, pat, strlen(pat), &o, recursive, 0);
        if (r < 0) {
            print_status(files[i], r);
        }
    }
}


static void cmd_stat(const char *rest)
{
    char path[PATH_MAX];
    struct stat_info st;

    if (rest[0] == '\0') {
        print_status("stat", -E_INVAL);
        return;
    }
    resolve(rest, path);
    if (stat(path, &st) != 0) {
        print_status("stat", -E_NOENT);
        return;
    }
    puts("  file: ");
    puts(path);
    puts("\n  type: ");
    puts(st.type == 2 ? "directory\n" : "file\n");
    puts("  size: ");
    put_u64(st.size);
    puts(" bytes\n  mode: ");
    put_octal(st.mode);
    puts("\n  uid: ");
    put_u64(st.uid);
    puts(" gid: ");
    put_u64(st.gid);
    puts("\n");
}

/* Recursive VFS walk shared by du (sum) and find (print). depth stops
 * runaway recursion; readdir gives 1 = file, 2 = directory, 0 = end. */
static int walk(const char *path, int depth, u64 *total, int print)
{
    struct stat_info st;
    char name[64];
    char child[PATH_MAX];

    if (stat(path, &st) != 0) {
        return -1;
    }
    if (st.type != 2) {
        if (total) {
            *total += st.size;
        }
        if (print) {
            puts(path);
            puts("\n");
        }
        return 0;
    }
    if (depth > 16) {
        return 0;
    }
    if (print) {
        puts(path);
        puts("/\n");
    }
    for (u64 i = 0;; ++i) {
        int r = readdir(path, i, name, sizeof(name));
        size_t pl = strlen(path);

        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;
        }
        if (pl + strlen(name) + 2 >= sizeof(child)) {
            continue;
        }
        memcpy(child, path, pl);
        if (pl == 0 || child[pl - 1] != '/') {
            child[pl++] = '/';
        }
        memcpy(child + pl, name, strlen(name) + 1);
        if (walk(child, depth + 1, total, print) != 0) {
            return -1;
        }
    }
}

/* Apply chmod/chown over a tree. Continues past per-entry errors the way
 * GNU does, reporting the first one. No symlinks exist in this VFS, so the
 * depth cap only guards pathological nesting. */
static int chmod_tree(const char *path, u32 mode, u32 uid, u32 gid, int do_chown, int depth)
{
    struct stat_info st;
    int first = 0;
    int r;

    if (stat(path, &st) != 0) {
        return -E_NOENT;
    }
    r = do_chown ? chown(path, uid, gid) : chmod(path, mode);
    if (r != 0) {
        first = r;
    }
    if (st.type != 2 || depth > 16) {
        return first;
    }
    for (u64 i = 0;; ++i) {
        char name[64];
        char child[PATH_MAX];
        size_t pl = strlen(path);
        int e = readdir(path, i, name, sizeof(name));

        if (e < 0) {
            return first ? first : e;
        }
        if (e == 0) {
            return first;
        }
        if (pl + strlen(name) + 2 >= sizeof(child)) {
            if (!first) {
                first = -E_INVAL;
            }
            continue;
        }
        memcpy(child, path, pl);
        if (pl == 0 || child[pl - 1] != '/') {
            child[pl++] = '/';
        }
        memcpy(child + pl, name, strlen(name) + 1);
        r = chmod_tree(child, mode, uid, gid, do_chown, depth + 1);
        if (r != 0 && !first) {
            first = r;
        }
    }
}

static void cmd_du(char *rest)
{
    char path[PATH_MAX];
    char *arg = next_arg(&rest);
    char *file = arg;
    int human = 0;
    u64 total = 0;

    if (arg && strcmp(arg, "-h") == 0) {
        human = 1;
        file = next_arg(&rest);
    }
    if (file && next_arg(&rest)) {
        print_status("du", -E_INVAL);
        return;
    }
    if (!file) {
        memcpy(path, cwd, strlen(cwd) + 1);
    } else {
        resolve(file, path);
    }
    if (walk(path, 0, &total, 0) != 0) {
        print_status("du", -E_NOENT);
        return;
    }
    if (human) {
        put_human(total);
    } else {
        put_u64(total);
    }
    puts("\t");
    puts(path);
    puts("\n");
}

static int glob_star(const char *pat, const char *s);
static int find_name(const char *path, const char *pat, int depth);

static void cmd_find(char *rest)
{
    char path[PATH_MAX];
    char *arg = next_arg(&rest);
    char *pat = 0;
    char *dir = arg;

    if (arg && strcmp(arg, "-name") == 0) {
        pat = next_arg(&rest);
        dir = next_arg(&rest);
        if (!pat) {
            print_status("find", -E_INVAL);
            return;
        }
    }
    if (dir && next_arg(&rest)) {
        print_status("find", -E_INVAL);
        return;
    }
    if (!dir) {
        memcpy(path, cwd, strlen(cwd) + 1);
    } else {
        resolve(dir, path);
    }
    if (!pat) {
        if (walk(path, 0, 0, 1) != 0) {
            print_status("find", -E_NOENT);
        }
        return;
    }
    if (find_name(path, pat, 0) != 0) {
        print_status("find", -E_NOENT);
    }
}

/* Glob with '*' only (no '?', no classes): enough for find -name, honest
 * about it in the usage line. */
static int glob_star(const char *pat, const char *s)
{
    const char *star = 0;
    const char *ss = s;

    while (*s) {
        if (*pat == '*') {
            star = pat++;
            ss = s;
        } else if (*pat == *s) {
            ++pat;
            ++s;
        } else if (star) {
            pat = star + 1;
            s = ++ss;
        } else {
            return 0;
        }
    }
    while (*pat == '*') {
        ++pat;
    }
    return *pat == '\0';
}

static int find_name(const char *path, const char *pat, int depth)
{
    struct stat_info st;
    char name[64];
    char child[PATH_MAX];

    if (stat(path, &st) != 0) {
        return -1;
    }
    if (st.type != 2) {
        const char *base = path;
        const char *slash = path;

        while (*slash) {
            if (*slash++ == '/') {
                base = slash;
            }
        }
        if (glob_star(pat, base)) {
            puts(path);
            puts("\n");
        }
        return 0;
    }
    if (depth > 16) {
        return 0;
    }
    for (u64 i = 0;; ++i) {
        int r = readdir(path, i, name, sizeof(name));
        size_t pl = strlen(path);

        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return 0;
        }
        if (pl + strlen(name) + 2 >= sizeof(child)) {
            continue;
        }
        memcpy(child, path, pl);
        if (pl == 0 || child[pl - 1] != '/') {
            child[pl++] = '/';
        }
        memcpy(child + pl, name, strlen(name) + 1);
        if (r == 2) {
            if (find_name(child, pat, depth + 1) != 0) {
                return -1;
            }
        } else if (glob_star(pat, name)) {
            puts(child);
            puts("\n");
        }
    }
}

/* cut -d DELIM -f LIST [FILE]: delimiter-split fields, streamed. LIST is
 * N,N-M,N-, -M comma items (1-based); -d defaults to TAB. */
static void cut_line(char *line, char delim, const int *lo, const int *hi, int n)
{
    int field = 1;
    char *p = line;
    char *start = line;
    int first = 1;

    for (;;) {
        int end = *p == '\0' || *p == delim;
        char save = *p;

        if (end) {
            *p = '\0';
            for (int i = 0; i < n; ++i) {
                if (field >= lo[i] && (hi[i] < 0 || field <= hi[i])) {
                    if (!first) {
                        write(1, &delim, 1);
                    }
                    first = 0;
                    puts(start);
                    break;
                }
            }
            *p = save;
            if (save == '\0') {
                break;
            }
            ++field;
            start = p + 1;
        }
        ++p;
    }
    puts("\n");
}

static void cmd_cut(char *rest)
{
    char *tok = next_arg(&rest);
    char delim = '\t';
    int lo[16], hi[16];
    int n = 0;
    char *file;
    char path[PATH_MAX];
    char buf[IO_BUF];
    char line[LINE_MAX];
    size_t llen = 0;
    int fd;
    ssize_t r;

    while (tok && tok[0] == '-') {
        if (tok[1] == 'd' && tok[2] != '\0' && tok[3] == '\0') {
            delim = tok[2];
        } else if (tok[1] == 'd' && tok[2] == '\0') {
            char *darg = next_arg(&rest);

            if (!darg || darg[0] == '\0' || darg[1] != '\0') {
                print_status("cut", -E_INVAL);
                return;
            }
            delim = darg[0];
        } else if (tok[1] == 'f') {
            char *list = tok[2] != '\0' ? tok + 2 : next_arg(&rest);

            if (!list) {
                print_status("cut", -E_INVAL);
                return;
            }
            /* Parse N,N-M,N-,-M items. */
            for (char *it = list;;) {
                char *comma = it;
                int a = 0, b = -1;
                int have_a = 0, have_b = 0;

                while (*comma && *comma != ',') {
                    ++comma;
                }
                {
                    char save = *comma;

                    *comma = '\0';
                    /* Forms: N | N- | N-M | -M */
                    {
                        const char *q = it;
                        u32 v = 0;

                        if (*q == '-') {
                            ++q;
                        } else {
                            while (*q >= '0' && *q <= '9') {
                                v = v * 10 + (u32)(*q - '0');
                                ++q;
                                have_a = 1;
                            }
                            a = (int)v;
                        }
                        if (*q == '-') {
                            ++q;
                            if (*q == '\0') {
                                have_b = 0; /* open end */
                            } else {
                                v = 0;
                                have_b = 0;
                                while (*q >= '0' && *q <= '9') {
                                    v = v * 10 + (u32)(*q - '0');
                                    ++q;
                                    have_b = 1;
                                }
                                b = (int)v;
                            }
                        } else if (*q == '\0') {
                            b = a;
                            have_b = 1;
                        } else {
                            *comma = save;
                            print_status("cut", -E_INVAL);
                            return;
                        }
                        if (!have_a && !have_b) {
                            *comma = save;
                            print_status("cut", -E_INVAL);
                            return;
                        }
                        if (!have_a) {
                            a = 1; /* -M means 1-M */
                        }
                    }
                    *comma = save;
                }
                if (n >= 16) {
                    print_status("cut", -E_INVAL);
                    return;
                }
                lo[n] = a < 1 ? 1 : a;
                hi[n] = have_b ? b : -1;
                if (hi[n] != -1 && hi[n] < lo[n]) {
                    print_status("cut", -E_INVAL);
                    return;
                }
                ++n;
                if (*comma == '\0') {
                    break;
                }
                it = comma + 1;
                if (*it == '\0') {
                    print_status("cut", -E_INVAL);
                    return;
                }
            }
        } else {
            print_status("cut", -E_INVAL);
            return;
        }
        tok = next_arg(&rest);
    }
    file = tok;
    if (n == 0 || (file && next_arg(&rest))) {
        print_status("cut", -E_INVAL);
        return;
    }
    if (!file) {
        fd = 0; /* stdin */
    } else {
        resolve(file, path);
        fd = open(path, 0);
        if (fd < 0) {
            print_status("cut", fd);
            return;
        }
    }
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < r; ++i) {
            if (buf[i] == '\n') {
                line[llen] = '\0';
                cut_line(line, delim, lo, hi, n);
                llen = 0;
            } else if (llen + 1 < sizeof(line)) {
                line[llen++] = buf[i];
            }
        }
    }
    if (llen > 0) {
        line[llen] = '\0';
        cut_line(line, delim, lo, hi, n);
    }
    if (file) {
        close(fd);
    }
    if (r < 0) {
        print_status("cut", (int)r);
    }
}

/* uniq [-c] [FILE]: collapse adjacent duplicate lines (stdin by default). */
static void cmd_uniq(char *rest)
{
    char *tok = next_arg(&rest);
    char *file = tok;
    int count = 0;
    char path[PATH_MAX];
    char buf[IO_BUF];
    char prev[LINE_MAX];
    char cur[LINE_MAX];
    size_t clen = 0;
    u64 run = 0;
    int fd;
    ssize_t r;

    if (tok && strcmp(tok, "-c") == 0) {
        count = 1;
        file = next_arg(&rest);
    }
    if (file && next_arg(&rest)) {
        print_status("uniq", -E_INVAL);
        return;
    }
    if (!file) {
        fd = 0;
    } else {
        resolve(file, path);
        fd = open(path, 0);
        if (fd < 0) {
            print_status("uniq", fd);
            return;
        }
    }
    {
        int have = 0;

        while ((r = read(fd, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < r; ++i) {
                if (buf[i] == '\n') {
                    cur[clen] = '\0';
                    if (!have || strcmp(prev, cur) != 0) {
                        if (have) {
                            if (count) {
                                put_u64(run);
                                puts(" ");
                            }
                            puts(prev);
                            puts("\n");
                        }
                        memcpy(prev, cur, clen + 1);
                        run = 1;
                        have = 1;
                    } else {
                        ++run;
                    }
                    clen = 0;
                } else if (clen + 1 < sizeof(cur)) {
                    cur[clen++] = buf[i];
                }
            }
        }
        if (clen > 0) {
            cur[clen] = '\0';
            if (!have || strcmp(prev, cur) != 0) {
                if (have) {
                    if (count) {
                        put_u64(run);
                        puts(" ");
                    }
                    puts(prev);
                    puts("\n");
                }
                memcpy(prev, cur, clen + 1);
                run = 1;
                have = 1;
            } else {
                ++run;
            }
        }
        if (have) {
            if (count) {
                put_u64(run);
                puts(" ");
            }
            puts(prev);
            puts("\n");
        }
    }
    if (file) {
        close(fd);
    }
    if (r < 0) {
        print_status("uniq", (int)r);
    }
}

/* tr SET1 SET2 [FILE]: byte translation, streamed (stdin by default). */
/* Expand a tr set: 'a-z' ranges plus literals ('-' is literal first/last).
 * Returns the expanded length, or -1 when nothing fits. */
static int tr_expand(const char *s, unsigned char *out, size_t cap)
{
    size_t n = 0;

    while (*s && n < cap) {
        if (s[0] != '-' && s[1] == '-' && s[2] != '\0' &&
            (unsigned char)s[0] <= (unsigned char)s[2]) {
            unsigned char lo = (unsigned char)s[0];
            unsigned char hi = (unsigned char)s[2];

            for (unsigned c = lo; c <= hi && n < cap; ++c) {
                out[n++] = (unsigned char)c;
            }
            s += 3;
        } else {
            out[n++] = (unsigned char)*s++;
        }
    }
    if (*s) {
        return -1;
    }
    return (int)n;
}

static void cmd_tr(char *rest)
{
    char *s1 = next_arg(&rest);
    char *s2 = next_arg(&rest);
    char *file = next_arg(&rest);
    char path[PATH_MAX];
    char buf[IO_BUF];
    unsigned char map[256];
    unsigned char e1[256], e2[256];
    int n1, n2;
    int fd;
    ssize_t r;

    if (!s1 || !s2 || next_arg(&rest)) {
        print_status("tr", -E_INVAL);
        return;
    }
    n1 = tr_expand(s1, e1, sizeof(e1));
    n2 = tr_expand(s2, e2, sizeof(e2));
    if (n1 < 0 || n2 < 0 || n1 != n2) {
        print_status("tr", -E_INVAL);
        return;
    }
    for (int i = 0; i < 256; ++i) {
        map[i] = (unsigned char)i;
    }
    for (int i = 0; i < n1; ++i) {
        map[e1[i]] = e2[i];
    }
    if (!file) {
        fd = 0;
    } else {
        resolve(file, path);
        fd = open(path, 0);
        if (fd < 0) {
            print_status("tr", fd);
            return;
        }
    }
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < r; ++i) {
            buf[i] = (char)map[(unsigned char)buf[i]];
        }
        write(1, buf, (size_t)r);
    }
    if (file) {
        close(fd);
    }
    if (r < 0) {
        print_status("tr", (int)r);
    }
}

/* strings [FILE]: runs of 4+ printable bytes (stdin by default). */
static void cmd_strings(char *rest)
{
    char *file = next_arg(&rest);
    char path[PATH_MAX];
    char buf[IO_BUF];
    char run[128];
    size_t rlen = 0;
    int fd;
    ssize_t r;

    if (file && next_arg(&rest)) {
        print_status("strings", -E_INVAL);
        return;
    }
    if (!file) {
        fd = 0;
    } else {
        resolve(file, path);
        fd = open(path, 0);
        if (fd < 0) {
            print_status("strings", fd);
            return;
        }
    }
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < r; ++i) {
            char c = buf[i];

            if (c >= 0x20 && c < 0x7f) {
                if (rlen + 1 < sizeof(run)) {
                    run[rlen++] = c;
                }
            } else {
                if (rlen >= 4) {
                    run[rlen] = '\0';
                    puts(run);
                    puts("\n");
                }
                rlen = 0;
            }
        }
    }
    if (rlen >= 4) {
        run[rlen] = '\0';
        puts(run);
        puts("\n");
    }
    if (file) {
        close(fd);
    }
    if (r < 0) {
        print_status("strings", (int)r);
    }
}

static void cmd_pwd(void)
{
    puts(cwd);
    puts("\n");
}

static void cmd_cd(const char *rest)
{
    char path[PATH_MAX];
    struct stat_info st;

    if (rest[0] == '\0') {
        memcpy(cwd, "/", 2);
        return;
    }
    resolve(rest, path);
    if (stat(path, &st) != 0 || st.type != 2) {
        print_status("cd", -E_NOENT);
        return;
    }
    memcpy(cwd, path, strlen(path) + 1);
}

/* tee [-a] FILE: stdin to file and stdout. No -i/--help subset beyond -a. */
static void cmd_tee(char *rest)
{
    char *tok = next_arg(&rest);
    char *file;
    int append = 0;
    char path[PATH_MAX];
    char buf[IO_BUF];
    int fd;
    ssize_t n;

    if (tok && strcmp(tok, "-a") == 0) {
        append = 1;
        tok = next_arg(&rest);
    }
    file = tok;
    if (!file || next_arg(&rest)) {
        print_status("tee", -E_INVAL);
        return;
    }
    resolve(file, path);
    fd = open(path, O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC));
    if (fd < 0) {
        print_status("tee", fd);
        return;
    }
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
        if (write(fd, buf, (size_t)n) != n) {
            print_status("tee", -12);
            close(fd);
            return;
        }
    }
    close(fd);
}

static void cmd_clear(void)
{
    /* The kernel clears VGA and sends the ANSI sequence to the serial terminal. */
    if (console_clear() < 0) {
        puts("\n");
    }
}

static void cmd_free(const char *rest)
{
    struct meminfo mi;
    int human = 0;

    if (rest[0] == '-') {
        if (strcmp(rest, "-h") != 0) {
            print_status("free", -E_INVAL);
            return;
        }
        human = 1;
    } else if (rest[0] != '\0') {
        print_status("free", -E_INVAL);
        return;
    }
    if (meminfo(&mi) != 0) {
        print_status("free", -5);
        return;
    }
    puts("MemFree: ");
    if (human) {
        put_human(mi.free_frames * 4 * 1024);
    } else {
        put_u64(mi.free_frames * 4);
        puts(" KiB");
    }
    puts("\nHeap: ");
    put_u64(mi.heap_used);
    puts(" used / ");
    put_u64(mi.heap_free);
    puts(" free bytes\nVfsNodes: ");
    put_u64(mi.live_nodes);
    puts(" live\n");
}

/* df [-h], lsblk: capacity and backend from /proc/disk (published live by
 * the disk driver). The parse fills the caller buffers; -h prints human
 * sizes instead of exact bytes. */
static int proc_disk(char *backend, size_t bcap, u64 *sectors, u64 *bytes)
{
    char buf[256];
    int fd;
    ssize_t n, total = 0;

    fd = open("/proc/disk", 0);
    if (fd < 0) {
        return fd;
    }
    while (total < (ssize_t)sizeof(buf) - 1 && (n = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total)) > 0) {
        total += n;
    }
    close(fd);
    if (total <= 0) {
        return -5;
    }
    buf[total] = '\0';
    backend[0] = '\0';
    *sectors = 0;
    *bytes = 0;
    for (char *l = buf; *l;) {
        char *e = l;

        while (*e && *e != '\n') {
            ++e;
        }
        if (*e) {
            *e++ = '\0';
        }
        if (l[0] == 'd' && memcmp(l, "disk: ", 6) == 0) {
            size_t k = strlen(l + 6);

            if (k >= bcap) {
                k = bcap - 1;
            }
            memcpy(backend, l + 6, k);
            backend[k] = '\0';
        } else if (memcmp(l, "sectors: ", 9) == 0) {
            for (char *d = l + 9; *d >= '0' && *d <= '9'; ++d) {
                *sectors = *sectors * 10 + (u64)(*d - '0');
            }
        } else if (memcmp(l, "capacity_bytes: ", 16) == 0) {
            for (char *d = l + 16; *d >= '0' && *d <= '9'; ++d) {
                *bytes = *bytes * 10 + (u64)(*d - '0');
            }
        }
        l = e;
    }
    return 0;
}

static void cmd_df(char *rest)
{
    char backend[128];
    char *arg = next_arg(&rest);
    char *path = arg;
    u64 sectors, bytes;
    int human = 0;
    int r;

    if (arg && strcmp(arg, "-h") == 0) {
        human = 1;
        path = next_arg(&rest);
    }
    if (path && next_arg(&rest)) {
        print_status("df", -E_INVAL);
        return;
    }
    if (path) {
        /* One disk in this kernel: the path only has to exist. */
        char p[PATH_MAX];
        struct stat_info st;

        resolve(path, p);
        if (stat(p, &st) != 0) {
            print_status("df", -E_NOENT);
            return;
        }
    }
    r = proc_disk(backend, sizeof(backend), &sectors, &bytes);
    if (r != 0) {
        print_status("df", r);
        return;
    }
    puts("backend: ");
    puts(backend[0] ? backend : "?");
    puts("\ncapacity: ");
    if (human) {
        put_human(bytes);
    } else {
        put_u64(bytes);
        puts(" bytes");
    }
    puts(" (");
    put_u64(sectors);
    puts(" sectors)\n");
}

static void cmd_lsblk(void)
{
    char backend[128];
    u64 sectors, bytes;
    int r = proc_disk(backend, sizeof(backend), &sectors, &bytes);

    if (r != 0) {
        print_status("lsblk", r);
        return;
    }
    puts("NAME\tSECTORS\tCAPACITY\tBACKEND\n");
    puts("disk0\t");
    put_u64(sectors);
    puts("\t");
    put_human(bytes);
    puts("\t");
    puts(backend[0] ? backend : "?");
    puts("\n");
}

/* ps: live process snapshot (SYS_PS). Zombies show as Z: reaped by wait. */
/* ---- text filters that need the whole input: sort, tac, nl ---------------- */

#define SLURP_MAX 65536u
#define MAX_LINES 4096u

static char slurp_buf[SLURP_MAX + 1u];
static char *line_ptr[MAX_LINES];

/* Read every FILE (or stdin when there are none) into slurp_buf, split it into
 * lines in place and return the line count. Returns a negative errno when an
 * input cannot be read or does not fit (64 KiB / 4096 lines). */
static int slurp_lines(char **files, int nfiles)
{
    size_t len = 0;
    int lines = 0;
    int truncated = 0;

    for (int i = 0; i < (nfiles ? nfiles : 1); ++i) {
        int fd = 0;
        ssize_t r;

        if (nfiles) {
            char path[PATH_MAX];

            resolve(files[i], path);
            fd = open(path, 0);
            if (fd < 0) {
                print_status(files[i], fd);
                return fd;
            }
        }
        while ((r = read(fd, slurp_buf + len, SLURP_MAX - len)) > 0) {
            len += (size_t)r;
            if (len == SLURP_MAX) {
                char probe;

                if (read(fd, &probe, 1) > 0) {
                    truncated = 1;
                }
                break;
            }
        }
        if (nfiles) {
            close(fd);
        }
        if (r < 0) {
            return (int)r;
        }
        if (truncated) {
            return -E_NOSPC;
        }
        if (len > 0 && slurp_buf[len - 1] != '\n' && len < SLURP_MAX) {
            slurp_buf[len++] = '\n'; /* a file's last line may lack its newline */
        }
    }
    slurp_buf[len] = '\0';
    for (size_t i = 0; i < len;) {
        size_t start = i;

        while (i < len && slurp_buf[i] != '\n') {
            ++i;
        }
        slurp_buf[i] = '\0';
        if (lines >= (int)MAX_LINES) {
            return -E_NOSPC;
        }
        line_ptr[lines++] = slurp_buf + start;
        ++i;
    }
    return lines;
}

/* Leading integer of a line for `sort -n` (blanks skipped; no digits sorts as 0). */
static long leading_number(const char *t)
{
    long v = 0;
    int neg = 0;

    while (*t == ' ' || *t == '\t') {
        ++t;
    }
    if (*t == '-') {
        neg = 1;
        ++t;
    }
    while (*t >= '0' && *t <= '9') {
        v = v * 10 + (*t - '0');
        ++t;
        if (v > 1000000000000000L) {
            break;
        }
    }
    return neg ? -v : v;
}

static int sort_less(const char *a, const char *b, int numeric)
{
    if (numeric) {
        long x = leading_number(a);
        long y = leading_number(b);

        if (x != y) {
            return x < y;
        }
    }
    return strcmp(a, b) < 0;
}

static void cmd_sort(char *rest)
{
    int reverse = 0, numeric = 0, unique = 0;
    char *files[16];
    int nfiles = 0;
    int n;
    char *arg;

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (arg[0] == '-' && arg[1] != '\0') {
            for (const char *f = arg + 1; *f; ++f) {
                if (*f == 'r') reverse = 1;
                else if (*f == 'n') numeric = 1;
                else if (*f == 'u') unique = 1;
                else {
                    print_status("sort", -E_INVAL);
                    return;
                }
            }
        } else if (nfiles < 16) {
            files[nfiles++] = arg;
        } else {
            print_status("sort", -E_INVAL);
            return;
        }
    }
    n = slurp_lines(files, nfiles);
    if (n < 0) {
        print_status("sort", n);
        return;
    }
    /* Shell sort: simple, in place, and O(n^1.3) is plenty for 4096 lines. */
    for (int gap = n / 2; gap > 0; gap /= 2) {
        for (int i = gap; i < n; ++i) {
            char *tmp = line_ptr[i];
            int j = i;

            while (j >= gap && sort_less(tmp, line_ptr[j - gap], numeric)) {
                line_ptr[j] = line_ptr[j - gap];
                j -= gap;
            }
            line_ptr[j] = tmp;
        }
    }
    for (int k = 0; k < n; ++k) {
        int i = reverse ? n - 1 - k : k;
        int prev = reverse ? i + 1 : i - 1;

        if (unique && k > 0 && strcmp(line_ptr[i], line_ptr[prev]) == 0) {
            continue;
        }
        puts(line_ptr[i]);
        puts("\n");
    }
}

static void cmd_tac(char *rest)
{
    char *files[16];
    int nfiles = 0;
    int n;
    char *arg;

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (nfiles < 16) {
            files[nfiles++] = arg;
        }
    }
    n = slurp_lines(files, nfiles);
    if (n < 0) {
        print_status("tac", n);
        return;
    }
    for (int i = n - 1; i >= 0; --i) {
        puts(line_ptr[i]);
        puts("\n");
    }
}

static void cmd_nl(char *rest)
{
    char *files[16];
    int nfiles = 0;
    int n;
    char *arg;

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (nfiles < 16) {
            files[nfiles++] = arg;
        }
    }
    n = slurp_lines(files, nfiles);
    if (n < 0) {
        print_status("nl", n);
        return;
    }
    for (int i = 0; i < n; ++i) {
        put_u64_pad((u64)i + 1u, 6);
        puts("\t");
        puts(line_ptr[i]);
        puts("\n");
    }
}

static void put_int_pad(long v, int width)
{
    char num[24];
    int n = 0;
    u64 m = v < 0 ? (u64)-v : (u64)v;

    do {
        num[n++] = (char)('0' + m % 10);
        m /= 10;
    } while (m != 0);
    if (v < 0) {
        num[n++] = '-';
    }
    for (int pad = width - n; pad > 0; --pad) {
        puts(" ");
    }
    while (n > 0) {
        write(1, &num[--n], 1);
    }
}

static void cmd_ps(void)
{
    struct ps_entry list[32];
    int n = ps_list(list, 32);
    int i;

    if (n < 0) {
        print_status("ps", n);
        return;
    }
    puts("  PID  PPID   UID S NAME\n");
    for (i = 0; i < n; ++i) {
        put_int_pad(list[i].pid, 5);
        puts(" ");
        put_int_pad(list[i].parent, 5);
        puts(" ");
        put_int_pad((long)list[i].uid, 5);
        puts(list[i].state ? " Z " : " R ");
        puts(list[i].name);
        puts("\n");
    }
}

/* dmesg: retained console log (SYS_DMESG), oldest first, paged by skip. */
static void cmd_dmesg(void)
{
    char buf[512];
    u64 skip = 0;

    for (;;) {
        long n = dmesg(buf, sizeof(buf), skip);

        if (n < 0) {
            print_status("dmesg", (int)n);
            return;
        }
        if (n == 0) {
            return;
        }
        write(1, buf, (size_t)n);
        skip += (u64)n;
        if (n < (long)sizeof(buf) || skip >= 65536u) {
            return;
        }
    }
}

static void cmd_lscpu(void)
{
    cmd_cat_proc("lscpu", "/proc/cpu");
}

/* hexdump [-C] FILE: offset, hex bytes and ASCII (like od -A x -t x1z). */
static void cmd_hexdump(char *rest)
{
    char *tok = next_arg(&rest);
    char *file = tok;
    char path[PATH_MAX];
    char buf[16];
    u64 off = 0;
    int fd;
    ssize_t n;

    if (tok && strcmp(tok, "-C") == 0) {
        file = next_arg(&rest);
    }
    if (!file || next_arg(&rest)) {
        print_status("hexdump", -E_INVAL);
        return;
    }
    resolve(file, path);
    fd = open(path, 0);
    if (fd < 0) {
        print_status("hexdump", fd);
        return;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        put_hex_n(off, 8);
        for (ssize_t i = 0; i < 16; ++i) {
            puts(i == 8 ? "  " : " ");
            if (i < n) {
                put_hex2((u8)buf[i]);
            } else {
                puts("  ");
            }
        }
        puts("  |");
        for (ssize_t i = 0; i < n; ++i) {
            char c = buf[i];

            write(1, (c >= 0x20 && c < 0x7f) ? &c : ".", 1);
        }
        puts("|\n");
        off += (u64)n;
    }
    close(fd);
    if (n < 0) {
        print_status("hexdump", (int)n);
    }
}

/* cmp FILE1 FILE2: first differing byte, or silence plus "identical". */
static void cmd_cmp(char *rest)
{
    char *f1 = next_arg(&rest);
    char *f2 = next_arg(&rest);
    char p1[PATH_MAX], p2[PATH_MAX];
    char b1[IO_BUF], b2[IO_BUF];
    int d1, d2;
    u64 off = 0;

    if (!f1 || !f2 || next_arg(&rest)) {
        print_status("cmp", -E_INVAL);
        return;
    }
    resolve(f1, p1);
    resolve(f2, p2);
    d1 = open(p1, 0);
    if (d1 < 0) {
        print_status("cmp", d1);
        return;
    }
    d2 = open(p2, 0);
    if (d2 < 0) {
        print_status("cmp", d2);
        close(d1);
        return;
    }
    for (;;) {
        ssize_t n1 = read(d1, b1, sizeof(b1));
        ssize_t n2 = read(d2, b2, sizeof(b2));

        if (n1 < 0 || n2 < 0) {
            print_status("cmp", (int)(n1 < 0 ? n1 : n2));
            break;
        }
        {
            ssize_t m = n1 < n2 ? n1 : n2;
            ssize_t i;

            for (i = 0; i < m; ++i) {
                if (b1[i] != b2[i]) {
                    puts("differ at byte ");
                    put_u64(off + (u64)i);
                    puts("\n");
                    close(d1);
                    close(d2);
                    return;
                }
            }
            off += (u64)m;
            if (n1 != n2) {
                puts("different lengths at byte ");
                put_u64(off);
                puts("\n");
                break;
            }
            if (n1 == 0) {
                puts("identical\n");
                break;
            }
        }
    }
    close(d1);
    close(d2);
}

static void cmd_basename(const char *rest, int want_dir)
{
    const char *p;
    size_t n;

    if (rest[0] == '\0') {
        print_status(want_dir ? "dirname" : "basename", -E_INVAL);
        return;
    }
    /* Strip trailing slashes (but keep root). */
    n = strlen(rest);
    while (n > 1 && rest[n - 1] == '/') {
        --n;
    }
    if (!want_dir) {
        p = rest + n;
        while (p > rest && p[-1] != '/') {
            --p;
        }
        while (p < rest + n) {
            char c = *p++;

            write(1, &c, 1);
        }
        puts("\n");
        return;
    }
    p = rest + n;
    while (p > rest && p[-1] != '/') {
        --p;
    }
    while (p > rest + 1 && p[-1] == '/') {
        --p;
    }
    if (p == rest) {
        puts(".\n");
        return;
    }
    if (p == rest + 1 && rest[0] == '/') {
        puts("/\n");
        return;
    }
    {
        size_t k = (size_t)(p - rest);

        if (k > 0 && rest[k - 1] == '/' && k > 1) {
            --k;
        }
        for (size_t i = 0; i < k; ++i) {
            write(1, rest + i, 1);
        }
        puts("\n");
    }
}

/* seq LAST | FIRST LAST | FIRST STEP LAST (integers, negatives allowed). */
static long parse_long(const char *s, int *ok)
{
    long v = 0;
    int neg = 0;
    int digits = 0;

    if (*s == '-') {
        neg = 1;
        ++s;
    }
    while (*s >= '0' && *s <= '9') {
        /* Saturate instead of wrapping: a wrapped bound prints forever. */
        if (v > 922337203685477580L) {
            v = 9223372036854775807L;
        } else {
            v = v * 10 + (*s - '0');
        }
        ++s;
        ++digits;
    }
    if (digits == 0 || *s != '\0') {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return neg ? -v : v;
}

static void cmd_seq(char *rest)
{
    char *a1 = next_arg(&rest);
    char *a2 = next_arg(&rest);
    char *a3 = next_arg(&rest);
    long first = 1, step = 1, last = 0;
    int ok = 1;
    long v;

    if (!a1 || next_arg(&rest)) {
        print_status("seq", -E_INVAL);
        return;
    }
    last = parse_long(a1, &ok);
    if (a2) {
        first = last;
        last = parse_long(a2, &ok);
        if (a3) {
            step = last;
            last = parse_long(a3, &ok);
        }
    }
    if (!ok || step == 0) {
        print_status("seq", -E_INVAL);
        return;
    }
    /* The wrap check is done in unsigned arithmetic (mod 2^64, defined):
     * a wrapped step would otherwise print until heat death. */
    if (step > 0) {
        for (v = first; v <= last;) {
            put_int(v);
            puts("\n");
            {
                unsigned long unext = (unsigned long)v + (unsigned long)step;

                if (unext < (unsigned long)v) {
                    break;
                }
                v = (long)unext;
            }
        }
    } else {
        for (v = first; v >= last;) {
            put_int(v);
            puts("\n");
            {
                unsigned long unext = (unsigned long)v + (unsigned long)step;

                if (unext > (unsigned long)v) {
                    break;
                }
                v = (long)unext;
            }
        }
    }
}

/* which PROG: where the shell would run it from. */
static void cmd_which(const char *rest)
{
    char path[64];
    struct stat_info st;
    size_t cl;

    if (rest[0] == '\0') {
        print_status("which", -E_INVAL);
        return;
    }
    for (const char *c = rest; *c; ++c) {
        if (*c == ' ') {
            print_status("which", -E_INVAL);
            return;
        }
    }
    cl = strlen(rest);
    if (cl + 6 >= sizeof(path)) {
        print_status("which", -E_INVAL);
        return;
    }
    memcpy(path, "/bin/", 5);
    memcpy(path + 5, rest, cl + 1);
    if (stat(path, &st) == 0) {
        puts(path);
        puts("\n");
    } else {
        puts("which: no ");
        puts(rest);
        puts(" in /bin\n");
    }
}

/* time PROG [ARGS]: run and report wall milliseconds. */
static void cmd_time(char *rest)
{
    char *prog = next_arg(&rest);
    char path[PATH_MAX];
    u64 before, after;

    if (!prog) {
        print_status("time", -E_INVAL);
        return;
    }
    if (prog[0] == '/') {
        resolve(prog, path);
    } else {
        size_t cl = strlen(prog);

        if (cl + 6 >= 64) {
            print_status("time", -E_INVAL);
            return;
        }
        memcpy(path, "/bin/", 5);
        memcpy(path + 5, prog, cl + 1);
    }
    before = uptime_ms();
    (void)run(path, rest);
    after = uptime_ms();
    puts("real ");
    put_u64(after - before);
    puts(" ms\n");
}

/* whoami: login name for our uid from /etc/passwd, or the bare uid. */
static void cmd_whoami(void)
{
    char buf[256];
    int fd;
    ssize_t n, total = 0;
    u32 me = (u32)getuid();

    fd = open("/etc/passwd", 0);
    if (fd < 0) {
        put_u64(me);
        puts("\n");
        return;
    }
    while (total < (ssize_t)sizeof(buf) - 1 && (n = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total)) > 0) {
        total += n;
    }
    close(fd);
    if (total <= 0) {
        put_u64(me);
        puts("\n");
        return;
    }
    buf[total] = '\0';
    for (char *l = buf; *l;) {
        char *e = l;
        char *f1;
        u32 uid = 0;
        int ok = 0;

        while (*e && *e != '\n') {
            ++e;
        }
        if (*e) {
            *e++ = '\0';
        }
        /* name:passwd:uid:gid:... */
        f1 = l;
        while (*l && *l != ':') {
            ++l;
        }
        if (*l) {
            *l++ = '\0';
        }
        while (*l && *l != ':') {
            ++l;
        }
        if (*l) {
            *l++ = '\0';
        }
        while (*l >= '0' && *l <= '9') {
            uid = uid * 10 + (u32)(*l - '0');
            ++l;
            ok = 1;
        }
        if (ok && uid == me) {
            puts(f1);
            puts("\n");
            return;
        }
        l = e;
    }
    put_u64(me);
    puts("\n");
}

/* ip [addr|link], ifconfig: live NIC state from the e1000 driver. */
static void cmd_ip(const char *rest)
{
    struct net_stat st;
    int r = net_stat(&st);

    if (r == -E_NODEV) {
        puts("ip: no network device\n");
        return;
    }
    if (r != 0) {
        print_status("ip", r);
        return;
    }
    if (rest[0] != '\0' && strcmp(rest, "addr") != 0 && strcmp(rest, "link") != 0) {
        print_status("ip", -E_INVAL);
        return;
    }
    if (rest[0] == '\0' || strcmp(rest, "link") == 0) {
        puts("link: ");
        puts(st.link ? "up " : "down ");
        if (st.link) {
            put_u64(st.speed);
            puts("Mb/s");
        }
        puts("\nmac: ");
        for (int i = 0; i < 6; ++i) {
            if (i) {
                puts(":");
            }
            put_hex2(st.mac[i]);
        }
        puts("\n");
    }
    if (rest[0] == '\0' || strcmp(rest, "addr") == 0) {
        puts("ip: ");
        put_ip(st.ip);
        puts("\ntx: ");
        put_u64(st.tx_packets);
        puts(" rx: ");
        put_u64(st.rx_packets);
        puts(" dropped: ");
        put_u64(st.rx_dropped);
        puts("\n");
    }
}

static void cmd_cat_proc(const char *name, const char *path)
{
    char buf[IO_BUF];
    int fd = open(path, 0);
    ssize_t n;

    if (fd < 0) {
        print_status(name, fd);
        return;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
    }
    close(fd);
}

/* sysinfo: one section per driver snapshot published under /proc. */
static void cmd_sysinfo(void)
{
    static const char *const files[] = {
        "/proc/version", "/proc/acpi", "/proc/cpu", "/proc/pci",
        "/proc/usb", "/proc/disk", "/proc/net",
    };
    static const char *const titles[] = {
        "version", "acpi", "cpu", "pci", "usb", "disk", "net",
    };
    char buf[IO_BUF];
    ssize_t n;

    puts("== axys sysinfo ==\n");
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        int fd = open(files[i], 0);

        puts("-- ");
        puts(titles[i]);
        puts(" --\n");
        if (fd < 0) {
            puts("(unavailable)\n");
            continue;
        }
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, (size_t)n);
        }
        close(fd);
    }
}

static void cmd_uname(const char *rest)
{
    if (rest[0] == '\0') {
        puts("axysOS\n");
    } else if (strcmp(rest, "-a") == 0) {
        cmd_cat_proc("uname", "/proc/version");
    } else {
        print_status("uname", -E_INVAL);
    }
}

static void cmd_hostname(char *rest)
{
    char *name = next_arg(&rest);

    if (!name) {
        cmd_cat_proc("hostname", "/etc/hostname");
        return;
    }
    if (next_arg(&rest)) {
        print_status("hostname", -E_INVAL);
        return;
    }
    {
        int fd = open("/etc/hostname", O_TRUNC | O_WRONLY);

        if (fd < 0) {
            print_status("hostname", fd);
            return;
        }
        write(fd, name, strlen(name));
        write(fd, "\n", 1);
        close(fd);
    }
}

static const char *const weekday_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void put_2digits(u32 v)
{
    char d[2] = {(char)('0' + (v / 10) % 10), (char)('0' + v % 10)};

    write(1, d, 2);
}

/* Parse "YYYY-MM-DD HH:MM:SS" (or with 'T'). Returns 0 and fills `c`, or -1. */
static int parse_datetime(const char *t, struct axys_civil *c)
{
    u32 v[6];
    int idx = 0;

    for (int field = 0; field < 6; ++field) {
        u32 n = 0;
        int digits = 0;

        while (*t >= '0' && *t <= '9' && digits < 5) {
            n = n * 10 + (u32)(*t - '0');
            ++t;
            ++digits;
        }
        if (digits == 0) {
            return -1;
        }
        v[idx++] = n;
        if (field < 5) {
            char sep = field == 0 || field == 1 ? '-' : (field == 2 ? ' ' : ':');

            if (*t == sep || (field == 2 && *t == 'T')) {
                ++t;
            } else {
                return -1;
            }
        }
    }
    if (*t != '\0') {
        return -1;
    }
    c->year = v[0];
    c->month = v[1];
    c->day = v[2];
    c->hour = v[3];
    c->minute = v[4];
    c->second = v[5];
    c->weekday = 0;
    return 0;
}

/* date [+FORMAT | -s "YYYY-MM-DD HH:MM:SS"]. UTC. FORMAT understands
 * %Y %m %d %H %M %S %s %a %b %F %T %% (anything else is printed as is). */
static void cmd_date(char *rest)
{
    char *arg = next_arg(&rest);
    long now;
    struct axys_civil c;

    if (arg && strcmp(arg, "-s") == 0) {
        char *when = next_arg(&rest);
        struct axys_civil want;
        long secs;
        int r;

        if (!when || parse_datetime(when, &want) != 0 || (secs = (long)axys_unix_from_civil(&want)) < 0) {
            print_status("date", -E_INVAL);
            return;
        }
        r = settime((u64)secs);
        if (r < 0) {
            print_status("date", r);
        }
        return;
    }
    now = time_unix();
    if (now < 0) {
        print_status("date", (int)now);
        return;
    }
    axys_civil_from_unix(now, &c);
    if (arg && arg[0] == '+') {
        for (const char *f = arg + 1; *f; ++f) {
            if (*f != '%' || f[1] == '\0') {
                write(1, f, 1);
                continue;
            }
            ++f;
            switch (*f) {
            case 'Y': put_u64(c.year); break;
            case 'm': put_2digits(c.month); break;
            case 'd': put_2digits(c.day); break;
            case 'H': put_2digits(c.hour); break;
            case 'M': put_2digits(c.minute); break;
            case 'S': put_2digits(c.second); break;
            case 's': put_u64((u64)now); break;
            case 'a': puts(weekday_names[c.weekday % 7]); break;
            case 'b': puts(month_names[(c.month - 1) % 12]); break;
            case 'F': put_u64(c.year); puts("-"); put_2digits(c.month); puts("-"); put_2digits(c.day); break;
            case 'T': put_2digits(c.hour); puts(":"); put_2digits(c.minute); puts(":"); put_2digits(c.second); break;
            case '%': puts("%"); break;
            default: write(1, f - 1, 2); break;
            }
        }
        puts("\n");
        return;
    }
    if (arg) {
        print_status("date", -E_INVAL);
        return;
    }
    puts(weekday_names[c.weekday % 7]);
    puts(" ");
    puts(month_names[(c.month - 1) % 12]);
    puts(" ");
    put_2digits(c.day);
    puts(" ");
    put_2digits(c.hour);
    puts(":");
    put_2digits(c.minute);
    puts(":");
    put_2digits(c.second);
    puts(" UTC ");
    put_u64(c.year);
    puts("\n");
}

static void cmd_kill(char *rest)
{
    char *arg = next_arg(&rest);
    const char *a;
    u32 pid = 0;
    int r;

    if (!arg || next_arg(&rest)) {
        print_status("kill", -E_INVAL);
        return;
    }
    a = arg;
    r = parse_num(&a, 10, &pid);
    if (r != 0) {
        print_status("kill", -E_INVAL);
        return;
    }
    r = kill((int)pid);
    if (r < 0) {
        print_status("kill", r);
    }
}

static void help_cmd(const char *c)
{
    if (strcmp(c, "ls") == 0) {
        puts("ls [-l] [-R] [PATH...]: names of a directory (-l: mode, uid, gid, size; -R: recurse);\n"
             "  a file argument lists that file; several paths get a header each\n");
    } else if (strcmp(c, "cat") == 0) {
        puts("cat [-n] [FILE...]: print files (-n numbers lines); no FILE copies standard input\n");
    } else if (strcmp(c, "echo") == 0) {
        puts("echo [-n] [WORD...]: print the words separated by spaces (-n: no newline).\n"
             "  Redirect with > or >> like any command; quote to keep spaces: echo \"a  b\"\n");
    } else if (strcmp(c, "mkdir") == 0) {
        puts("mkdir PATH...: create directories, parents as needed\n");
    } else if (strcmp(c, "rm") == 0) {
        puts("rm PATH...: remove files or empty directories\n");
    } else if (strcmp(c, "mv") == 0) {
        puts("mv SRC DST: rename or move within the VFS\n");
    } else if (strcmp(c, "rmtree") == 0) {
        puts("rmtree PATH: remove a tree recursively (rm -rf)\n");
    } else if (strcmp(c, "touch") == 0) {
        puts("touch FILE...: create empty files if missing\n");
    } else if (strcmp(c, "cp") == 0) {
        puts("cp [-r] SRC DST: copy a file, or a tree with -r\n");
    } else if (strcmp(c, "head") == 0) {
        puts("head [-N | -n N] [FILE]: first N lines (default 10); no FILE reads standard input\n");
    } else if (strcmp(c, "tail") == 0) {
        puts("tail [-N | -n N] [FILE]: last N lines (default 10); no FILE reads standard input\n");
    } else if (strcmp(c, "wc") == 0) {
        puts("wc [-l] [-w] [-c] [FILE...]: lines, words and bytes; no FILE reads standard input;\n"
             "  several files add a total line\n");
    } else if (strcmp(c, "grep") == 0) {
        puts("grep [-i] [-n] [-v] [-c] [-r] PATTERN [FILE...]: lines containing PATTERN (a plain\n"
             "  substring, not a regex). -i ignore case, -n line numbers, -v invert, -c count,\n"
             "  -r walk directories. No FILE reads standard input.\n");
    } else if (strcmp(c, "stat") == 0) {
        puts("stat PATH: type, size, mode, owner\n");
    } else if (strcmp(c, "du") == 0) {
        puts("du [-h] [PATH]: bytes under a tree (default: .)\n");
    } else if (strcmp(c, "find") == 0) {
        puts("find [PATH]: list a tree recursively (default: .)\n");
    } else if (strcmp(c, "hexdump") == 0) {
        puts("hexdump [-C] FILE: offset, hex bytes and ASCII\n");
    } else if (strcmp(c, "cmp") == 0) {
        puts("cmp FILE1 FILE2: first differing byte, or 'identical'\n");
    } else if (strcmp(c, "basename") == 0) {
        puts("basename PATH: last component\n");
    } else if (strcmp(c, "dirname") == 0) {
        puts("dirname PATH: all but the last component\n");
    } else if (strcmp(c, "seq") == 0) {
        puts("seq LAST | FIRST LAST | FIRST STEP LAST: print numbers\n");
    } else if (strcmp(c, "which") == 0) {
        puts("which PROG: where the shell would run it from\n");
    } else if (strcmp(c, "time") == 0) {
        puts("time PROG [ARGS]: run and report wall milliseconds\n");
    } else if (strcmp(c, "whoami") == 0) {
        puts("whoami: login name for our uid from /etc/passwd\n");
    } else if (strcmp(c, "cut") == 0) {
        puts("cut [-d DELIM] -f LIST [FILE]: delimiter fields (stdin default)\n");
    } else if (strcmp(c, "uniq") == 0) {
        puts("uniq [-c] [FILE]: collapse adjacent duplicates (stdin default)\n");
    } else if (strcmp(c, "tr") == 0) {
        puts("tr SET1 SET2 [FILE]: translate bytes, ranges like a-z\n");
    } else if (strcmp(c, "strings") == 0) {
        puts("strings [FILE]: runs of 4+ printable bytes (stdin default)\n");
    } else if (strcmp(c, "pwd") == 0) {
        puts("pwd: print the working directory\n");
    } else if (strcmp(c, "cd") == 0) {
        puts("cd [PATH]: change directory (default: /)\n");
    } else if (strcmp(c, "tee") == 0) {
        puts("tee [-a] FILE: stdin to file and stdout (-a appends)\n");
    } else if (strcmp(c, "clear") == 0) {
        puts("clear: clear the screen (VGA and serial terminal)\n");
    } else if (strcmp(c, "meminfo") == 0) {
        puts("meminfo: raw kernel allocator counters\n");
    } else if (strcmp(c, "free") == 0) {
        puts("free [-h]: free RAM, heap use, live VFS nodes\n");
    } else if (strcmp(c, "ps") == 0) {
        puts("ps: live processes from the kernel (pid, ppid, uid, state)\n");
    } else if (strcmp(c, "dmesg") == 0) {
        puts("dmesg: retained console log, oldest first\n");
    } else if (strcmp(c, "lscpu") == 0) {
        puts("lscpu: CPU vendor, brand and features from CPUID (/proc/cpu)\n");
    } else if (strcmp(c, "lsblk") == 0) {
        puts("lsblk: system disk sectors and capacity (/proc/disk)\n");
    } else if (strcmp(c, "uptime") == 0) {
        puts("uptime: seconds since boot\n");
    } else if (strcmp(c, "date") == 0) {
        puts("date [+FORMAT | -s \"YYYY-MM-DD HH:MM:SS\"]: current UTC date and time from the RTC.\n"
             "  FORMAT: %Y %m %d %H %M %S %s %a %b %F %T %%. -s (root) sets a software offset\n"
             "  that lasts until reboot; the hardware clock is not written.\n");
    } else if (strcmp(c, "random") == 0) {
        puts("random: 64 bits from the kernel RNG\n");
    } else if (strcmp(c, "pid") == 0) {
        puts("pid: print our process id\n");
    } else if (strcmp(c, "id") == 0) {
        puts("id: print uid and gid\n");
    } else if (strcmp(c, "su") == 0) {
        puts("su UID: root may switch down; going back to 0 is refused\n");
    } else if (strcmp(c, "chmod") == 0) {
        puts("chmod [-R] MODE PATH: octal mode, owner or root\n");
    } else if (strcmp(c, "chown") == 0) {
        puts("chown [-R] UID GID PATH: root only\n");
    } else if (strcmp(c, "sync") == 0) {
        puts("sync: flush the file system to disk\n");
    } else if (strcmp(c, "sleep") == 0) {
        puts("sleep SECS: wait\n");
    } else if (strcmp(c, "kill") == 0) {
        puts("kill PID: terminate a process\n");
    } else if (strcmp(c, "hostname") == 0) {
        puts("hostname [NAME]: show or set /etc/hostname\n");
    } else if (strcmp(c, "uname") == 0) {
        puts("uname [-a]: system name, or the full /proc/version line\n");
    } else if (strcmp(c, "run") == 0) {
        puts("run PROG [ARGS]: spawn /bin or absolute program and wait\n");
    } else if (strcmp(c, "poweroff") == 0 || strcmp(c, "halt") == 0 ||
               strcmp(c, "shutdown") == 0) {
        puts("poweroff|halt|shutdown: halt (root only)\n");
    } else if (strcmp(c, "reboot") == 0) {
        puts("reboot: reset (root only)\n");
    } else if (strcmp(c, "exit") == 0) {
        puts("exit: leave the shell (init restarts it as root)\n");
    } else if (strcmp(c, "lspci") == 0) {
        puts("lspci: PCI functions, BARs and capabilities (/proc/pci)\n");
    } else if (strcmp(c, "lsusb") == 0) {
        puts("lsusb: xHCI summary, keyboards, storage slot (/proc/usb)\n");
    } else if (strcmp(c, "sysinfo") == 0) {
        puts("sysinfo: every /proc driver snapshot in one place\n");
    } else if (strcmp(c, "df") == 0) {
        puts("df [-h] [PATH]: system disk backend and capacity (/proc/disk)\n");
    } else if (strcmp(c, "ip") == 0 || strcmp(c, "ifconfig") == 0) {
        puts("ip [addr|link]: live NIC state from the e1000 driver\n");
    } else if (strcmp(c, "sort") == 0) {
        puts("usage: sort [-r] [-n] [-u] [FILE...]   (no FILE: standard input; up to 64 KiB / 4096 lines)\n");
    } else if (strcmp(c, "tac") == 0) {
        puts("usage: tac [FILE...]   (lines in reverse order)\n");
    } else if (strcmp(c, "nl") == 0) {
        puts("usage: nl [FILE...]   (numbers every line)\n");
    } else if (strcmp(c, "help") == 0) {
        puts("help [CMD]: this list, or usage for CMD\n");
    } else {
        puts("no help for: ");
        puts(c);
        puts("\n");
    }
}

static void help_cmd(const char *c);

struct help_entry {
    const char *name;
    const char *summary;
    int category;
};

static const char *const help_categories[] = {"Files and directories", "Text and data", "System and users",
                                              "Processes and hardware", "Programs in /bin"};

static const struct help_entry help_table[] = {
    {"ls", "list a directory (-l long format, -R recursive)", 0},
    {"cd", "change the working directory", 0},
    {"pwd", "print the working directory", 0},
    {"mkdir", "create a directory", 0},
    {"touch", "create an empty file", 0},
    {"cp", "copy a file", 0},
    {"mv", "move or rename", 0},
    {"rm", "remove a file or an empty directory", 0},
    {"rmtree", "remove a directory and everything in it", 0},
    {"stat", "show type, mode, owner and size", 0},
    {"du", "disk usage of a tree", 0},
    {"find", "search a tree for names", 0},
    {"basename", "strip the directory from a path", 0},
    {"dirname", "strip the last path component", 0},
    {"tee", "copy input to a file and to the screen", 0},
    {"cat", "print files, or standard input (-n numbers lines)", 1},
    {"echo", "print arguments (-n: no newline)", 1},
    {"head", "first lines of a file or input", 1},
    {"tail", "last lines of a file or input", 1},
    {"wc", "count lines, words and bytes (-l -w -c)", 1},
    {"grep", "lines containing text (-i -n -v -c -r)", 1},
    {"cut", "select fields or characters from lines", 1},
    {"sort", "sort lines (-r reverse, -n numeric, -u unique)", 1},
    {"uniq", "collapse repeated adjacent lines", 1},
    {"tac", "print lines in reverse order", 1},
    {"nl", "number lines", 1},
    {"tr", "translate or delete characters", 1},
    {"strings", "printable strings found in a file", 1},
    {"hexdump", "bytes of a file in hex", 1},
    {"cmp", "compare two files byte by byte", 1},
    {"seq", "print a sequence of numbers", 1},
    {"clear", "clear the screen", 2},
    {"date", "show or set the date and time (UTC)", 2},
    {"uptime", "time since boot", 2},
    {"uname", "kernel name and version", 2},
    {"hostname", "show the host name", 2},
    {"whoami", "current user name", 2},
    {"id", "user and group ids", 2},
    {"su", "switch to another user id (root only)", 2},
    {"chmod", "change file permissions", 2},
    {"chown", "change file owner (root only)", 2},
    {"sync", "write the file system to disk", 2},
    {"sleep", "wait for some seconds", 2},
    {"time", "run a command and report how long it took", 2},
    {"which", "locate a command", 2},
    {"random", "random numbers from the kernel generator", 2},
    {"run", "run a program by path", 2},
    {"poweroff", "power the machine off", 2},
    {"halt", "stop the machine", 2},
    {"reboot", "restart the machine", 2},
    {"exit", "leave the shell", 2},
    {"help", "this list; `help CMD` for details", 2},
    {"ps", "list processes", 3},
    {"kill", "terminate a process by pid", 3},
    {"pid", "pid of this shell", 3},
    {"meminfo", "physical memory, kernel heap and file-system nodes", 3},
    {"free", "memory in use and free", 3},
    {"df", "file-system and disk usage", 3},
    {"dmesg", "kernel messages since boot", 3},
    {"lscpu", "processor information", 3},
    {"lsblk", "block devices", 3},
    {"lspci", "PCI devices", 3},
    {"lsusb", "USB controller and devices", 3},
    {"sysinfo", "summary of the whole system", 3},
    {"ip", "network address, link and counters", 3},
    {"ifconfig", "same as ip", 3},
    {"hello", "demo program in ring 3", 4},
    {"crash", "deliberate faults (tests exception handling)", 4},
    {"heap", "user heap test", 4},
    {"fileio", "file API self-test", 4},
    {"perms", "permission checks self-test", 4},
    {"probe", "system call self-test", 4},
    {"fuzz", "in-guest syscall fuzzer", 4},
    {"ping", "ICMP echo to an IPv4 address (root)", 4},
    {"dhcp", "obtain an IPv4 lease (root)", 4},
};

#define HELP_COUNT ((int)(sizeof(help_table) / sizeof(help_table[0])))

static void help(void)
{
    puts("axys shell. Type `help CMD` for the usage of one command.\n"
         "Quote words with '...' or \"...\"; join commands with ';' or '|' (a pipe);\n"
         "redirect with '<', '>' and '>>'. Output of any command can be redirected.\n");
    for (int cat = 0; cat < 5; ++cat) {
        puts("\n");
        puts(help_categories[cat]);
        puts("\n");
        for (int i = 0; i < HELP_COUNT; ++i) {
            size_t n;

            if (help_table[i].category != cat) {
                continue;
            }
            puts("  ");
            puts(help_table[i].name);
            for (n = strlen(help_table[i].name); n < 10; ++n) {
                puts(" ");
            }
            puts(help_table[i].summary);
            puts("\n");
        }
    }
}

/* One command: its summary line, then the detailed usage. */
static void help_one(const char *name)
{
    for (int i = 0; i < HELP_COUNT; ++i) {
        if (strcmp(help_table[i].name, name) == 0) {
            puts(name);
            puts(" - ");
            puts(help_table[i].summary);
            puts("\n");
            break;
        }
    }
    help_cmd(name);
}

/* Run one command line; several may ride separated by ';' (there is no
 * quoting in this shell: a ';' always separates). Returns 1 when the shell
 * must exit (`exit`). */
static int run_line(char *line)
{
    char *cmd;
    char *rest;

    cmd = line;
    while (*cmd == ' ') {
        ++cmd;
    }
    if (*cmd == '\0') {
        return 0;
    }
        rest = cmd;
        while (*rest && *rest != ' ') {
            ++rest;
        }
        if (*rest) {
            *rest++ = '\0';
        }
        while (*rest == ' ') {
            ++rest;
        }

        if (strcmp(cmd, "help") == 0) {
            if (rest[0] == '\0') {
                help();
            } else {
                char *what = next_arg(&rest);

                help_one(what ? what : "");
            }
        } else if (strcmp(cmd, "ls") == 0) {
            cmd_ls(rest);
        } else if (strcmp(cmd, "sort") == 0) {
            cmd_sort(rest);
        } else if (strcmp(cmd, "tac") == 0) {
            cmd_tac(rest);
        } else if (strcmp(cmd, "nl") == 0) {
            cmd_nl(rest);
        } else if (strcmp(cmd, "cat") == 0) {
            cmd_cat(rest);
        } else if (strcmp(cmd, "echo") == 0) {
            cmd_echo(rest);
        } else if (strcmp(cmd, "mkdir") == 0) {
            char *arg = next_arg(&rest);
            char path[PATH_MAX];

            if (!arg) {
                print_status("mkdir", -E_INVAL);
            } else {
                for (; arg; arg = next_arg(&rest)) {
                    int r;

                    resolve(arg, path);
                    r = mkdir(path);
                    if (r < 0) {
                        print_status("mkdir", r);
                    }
                }
            }
        } else if (strcmp(cmd, "rm") == 0) {
            char *arg = next_arg(&rest);
            char path[PATH_MAX];

            if (!arg) {
                print_status("rm", -E_INVAL);
            } else {
                for (; arg; arg = next_arg(&rest)) {
                    int r;

                    resolve(arg, path);
                    r = unlink(path);
                    if (r < 0) {
                        print_status("rm", r);
                    }
                }
            }
        } else if (strcmp(cmd, "mv") == 0) {
            char *src = next_arg(&rest);
            char *dst = next_arg(&rest);
            char spath[PATH_MAX], dpath[PATH_MAX];

            if (!src || !dst || next_arg(&rest)) {
                print_status("mv", -E_INVAL);
            } else {
                int r;

                resolve(src, spath);
                resolve(dst, dpath);
                r = rename(spath, dpath);
                if (r < 0) {
                    print_status("mv", r);
                }
            }
        } else if (strcmp(cmd, "rmtree") == 0) {
            char path[PATH_MAX];

            if (rest[0] == '\0') {
                print_status("rmtree", -E_INVAL);
            } else {
                resolve(rest, path);
                print_status("rmtree", rmtree(path));
            }
        } else if (strcmp(cmd, "touch") == 0) {
            cmd_touch(rest);
        } else if (strcmp(cmd, "cp") == 0) {
            cmd_cp(rest);
        } else if (strcmp(cmd, "head") == 0) {
            cmd_head_tail(rest, 0);
        } else if (strcmp(cmd, "tail") == 0) {
            cmd_head_tail(rest, 1);
        } else if (strcmp(cmd, "wc") == 0) {
            cmd_wc(rest);
        } else if (strcmp(cmd, "grep") == 0) {
            cmd_grep(rest);
        } else if (strcmp(cmd, "stat") == 0) {
            cmd_stat(rest);
        } else if (strcmp(cmd, "du") == 0) {
            cmd_du(rest);
        } else if (strcmp(cmd, "find") == 0) {
            cmd_find(rest);
        } else if (strcmp(cmd, "cut") == 0) {
            cmd_cut(rest);
        } else if (strcmp(cmd, "uniq") == 0) {
            cmd_uniq(rest);
        } else if (strcmp(cmd, "tr") == 0) {
            cmd_tr(rest);
        } else if (strcmp(cmd, "strings") == 0) {
            cmd_strings(rest);
        } else if (strcmp(cmd, "pwd") == 0) {
            cmd_pwd();
        } else if (strcmp(cmd, "cd") == 0) {
            cmd_cd(rest);
        } else if (strcmp(cmd, "tee") == 0) {
            cmd_tee(rest);
        } else if (strcmp(cmd, "clear") == 0) {
            cmd_clear();
        } else if (strcmp(cmd, "meminfo") == 0) {
            struct meminfo mi;
            int r = meminfo(&mi);

            if (r < 0) {
                print_status("meminfo", r);
            } else {
                puts("frames_free=");
                put_u64(mi.free_frames);
                puts(" heap_used=");
                put_u64(mi.heap_used);
                puts(" heap_free=");
                put_u64(mi.heap_free);
                puts(" live_nodes=");
                put_u64(mi.live_nodes);
                puts("\n");
            }
        } else if (strcmp(cmd, "free") == 0) {
            cmd_free(rest);
        } else if (strcmp(cmd, "uptime") == 0) {
            u64 ms = uptime_ms();
            put_u64(ms / 1000);
            puts(".");
            put_u64((ms % 1000) / 100);
            puts(" s\n");
        } else if (strcmp(cmd, "date") == 0) {
            cmd_date(rest);
        } else if (strcmp(cmd, "random") == 0) {
            u8 r[8];
            u64 v = 0;

            if (getrandom(r, 8) == 8) {
                for (int i = 0; i < 8; ++i) {
                    v = (v << 8) | r[i];
                }
                put_hex(v);
                puts("\n");
            }
        } else if (strcmp(cmd, "pid") == 0) {
            put_u64((u64)getpid());
            puts("\n");
        } else if (strcmp(cmd, "id") == 0) {
            puts("uid=");
            put_u64((u64)getuid());
            puts(" gid=");
            put_u64((u64)getgid());
            puts("\n");
        } else if (strcmp(cmd, "su") == 0) {
            const char *a = rest;
            u32 id = 0;
            int r = parse_num(&a, 10, &id) == 0 ? 0 : -22;

            if (r == 0) {
                r = setgid(id);
            }
            if (r == 0) {
                r = setuid(id);
            }
            if (r < 0) {
                print_status("su", r);
            }
        } else if (strcmp(cmd, "chmod") == 0) {
            const char *a = rest;
            u32 mode = 0;
            int rec = 0;
            int r;

            if (a[0] == '-' && a[1] == 'R' && (a[2] == ' ' || a[2] == '\0')) {
                rec = 1;
                a += 2;
                while (*a == ' ') {
                    ++a;
                }
            }
            r = parse_num(&a, 8, &mode) == 0 ? 0 : -22;

            while (*a == ' ') {
                ++a;
            }
            if (r == 0) {
                char path[PATH_MAX];

                if (*a == '\0') {
                    r = -E_INVAL;
                } else {
                    resolve(a, path);
                    r = rec ? chmod_tree(path, mode, 0, 0, 0, 0) : chmod(path, mode);
                }
            }
            if (r < 0) {
                print_status("chmod", r);
            }
        } else if (strcmp(cmd, "chown") == 0) {
            const char *a = rest;
            u32 uid = 0;
            u32 gid = 0;
            int rec = 0;
            int r;

            if (a[0] == '-' && a[1] == 'R' && (a[2] == ' ' || a[2] == '\0')) {
                rec = 1;
                a += 2;
                while (*a == ' ') {
                    ++a;
                }
            }
            r = parse_num(&a, 10, &uid) == 0 ? 0 : -22;

            while (*a == ' ') {
                ++a;
            }
            if (r == 0 && parse_num(&a, 10, &gid) != 0) {
                r = -22;
            }
            while (*a == ' ') {
                ++a;
            }
            if (r == 0) {
                char path[PATH_MAX];

                if (*a == '\0') {
                    r = -E_INVAL;
                } else {
                    resolve(a, path);
                    r = rec ? chmod_tree(path, 0, uid, gid, 1, 0) : chown(path, uid, gid);
                }
            }
            if (r < 0) {
                print_status("chown", r);
            }
        } else if (strcmp(cmd, "sync") == 0) {
            int r = sync();
            if (r < 0) {
                print_status("sync", r);
            }
        } else if (strcmp(cmd, "sleep") == 0) {
            const char *a = rest;
            u32 s = 0;
            int r = parse_num(&a, 10, &s);

            while (*a == ' ') {
                ++a;
            }
            if (r != 0 || *a != '\0') {
                print_status("sleep", -22);
            } else {
                sleep_ms((u64)s * 1000);
            }
        } else if (strcmp(cmd, "kill") == 0) {
            cmd_kill(rest);
        } else if (strcmp(cmd, "hostname") == 0) {
            cmd_hostname(rest);
        } else if (strcmp(cmd, "uname") == 0) {
            cmd_uname(rest);
        } else if (strcmp(cmd, "whoami") == 0) {
            cmd_whoami();
        } else if (strcmp(cmd, "which") == 0) {
            cmd_which(rest);
        } else if (strcmp(cmd, "seq") == 0) {
            cmd_seq(rest);
        } else if (strcmp(cmd, "time") == 0) {
            cmd_time(rest);
        } else if (strcmp(cmd, "basename") == 0) {
            cmd_basename(rest, 0);
        } else if (strcmp(cmd, "dirname") == 0) {
            cmd_basename(rest, 1);
        } else if (strcmp(cmd, "hexdump") == 0) {
            cmd_hexdump(rest);
        } else if (strcmp(cmd, "cmp") == 0) {
            cmd_cmp(rest);
        } else if (strcmp(cmd, "ps") == 0) {
            cmd_ps();
        } else if (strcmp(cmd, "dmesg") == 0) {
            cmd_dmesg();
        } else if (strcmp(cmd, "lscpu") == 0) {
            cmd_lscpu();
        } else if (strcmp(cmd, "lsblk") == 0) {
            cmd_lsblk();
        } else if (strcmp(cmd, "lspci") == 0) {
            cmd_cat_proc("lspci", "/proc/pci");
        } else if (strcmp(cmd, "lsusb") == 0) {
            cmd_cat_proc("lsusb", "/proc/usb");
        } else if (strcmp(cmd, "sysinfo") == 0) {
            cmd_sysinfo();
        } else if (strcmp(cmd, "df") == 0) {
            cmd_df(rest);
        } else if (strcmp(cmd, "ip") == 0 || strcmp(cmd, "ifconfig") == 0) {
            cmd_ip(rest);
        } else if (strcmp(cmd, "run") == 0) {
            char *a = rest;

            while (*a && *a != ' ') {
                ++a;
            }
            if (*a) {
                *a++ = '\0';
            }
            {
                char path[PATH_MAX];

                resolve(rest, path);
                run(path, a);
            }
        } else if (strcmp(cmd, "poweroff") == 0 || strcmp(cmd, "halt") == 0 ||
                   strcmp(cmd, "shutdown") == 0) {
            print_status("poweroff", power(0));
        } else if (strcmp(cmd, "reboot") == 0) {
            print_status("reboot", power(1));
        } else if (strcmp(cmd, "exit") == 0) {
            return 1;
        } else {
            char path[64];
            size_t cl = strlen(cmd);

            if (cmd[0] == '/') {
                char apath[PATH_MAX];

                resolve(cmd, apath);
                run(apath, rest);
            } else if (cl + 6 < sizeof(path)) {
                memcpy(path, "/bin/", 5);
                memcpy(path + 5, cmd, cl + 1);
                struct stat_info st;

                if (stat(path, &st) == 0) {
                    run(path, rest);
                } else {
                    puts("unknown command: ");
                    puts(cmd);
                    puts("\n");
                }
            } else {
                /* Too long for /bin/<cmd>: say so instead of swallowing it. */
                puts("unknown command: ");
                puts(cmd);
                puts("\n");
            }
        }
    return 0;
}

/* ---- command lines: quoting, `;`, `|`, `<`, `>`, `>>` ---------------------- */

#define MAX_STAGES 8

/* One command line without `;`: stages joined by `|`. A pipe is a temporary
 * file in /tmp: stage i writes it, stage i+1 reads it, stages run one after
 * another. Redirection is dup2() on this shell's own fd 0/1, so builtins and
 * the programs they spawn (which inherit fd 0-2) behave the same. */
static int run_pipeline(char *seg)
{
    char *stage[MAX_STAGES];
    char pipe_path[2][40];
    int pipe_used[2] = {0, 0};
    int count = 0;
    int exit_shell = 0;
    char *cur = seg;
    int pid = getpid();

    while (cur != 0 && count < MAX_STAGES) {
        stage[count++] = cur;
        cur = split_at(cur, '|');
    }
    if (cur != 0) {
        eputs("syntax error: too many pipeline stages\n");
        return 0;
    }
    for (int i = 0; i < 2; ++i) {
        char *w = pipe_path[i];
        const char *base = "/tmp/.pipe";
        u64 v = (u64)pid;
        char digits[20];
        int nd = 0;

        while (*base) {
            *w++ = *base++;
        }
        do {
            digits[nd++] = (char)('0' + v % 10);
            v /= 10;
        } while (v != 0);
        while (nd > 0) {
            *w++ = digits[--nd];
        }
        *w++ = '_';
        *w++ = (char)('0' + i);
        *w = '\0';
    }
    for (int i = 0; i < count && !exit_shell; ++i) {
        char in_name[PATH_MAX];
        char out_name[PATH_MAX];
        char path[PATH_MAX];
        int append = 0;
        int in_fd = -1;
        int out_fd = -1;
        int saved_in = -1;
        int saved_out = -1;
        char *c;

        if (extract_redirs(stage[i], in_name, out_name, sizeof(in_name), &append) != 0) {
            eputs("syntax error near redirection\n");
            break;
        }
        for (c = stage[i]; *c != '\0'; ++c) {
        }
        while (c > stage[i] && c[-1] == ' ') { /* the blanks left where a redirection was */
            *--c = '\0';
        }
        for (c = stage[i]; *c == ' '; ++c) {
        }
        if (*c == '\0' && count > 1) {
            eputs("syntax error: empty pipeline stage\n");
            break;
        }
        if (in_name[0]) {
            resolve(in_name, path);
            in_fd = open(path, 0);
            if (in_fd < 0) {
                print_status(in_name, in_fd);
                break;
            }
        } else if (i > 0) {
            in_fd = open(pipe_path[(i - 1) & 1], 0);
        }
        if (out_name[0]) {
            resolve(out_name, path);
            out_fd = open(path, O_CREAT | O_WRONLY | O_APPEND | (append ? 0 : O_TRUNC));
            if (out_fd < 0) {
                print_status(out_name, out_fd);
                if (in_fd >= 0) {
                    close(in_fd);
                }
                break;
            }
        } else if (i < count - 1) {
            out_fd = open(pipe_path[i & 1], O_CREAT | O_WRONLY | O_APPEND | O_TRUNC);
            if (out_fd >= 0) {
                pipe_used[i & 1] = 1;
            } else {
                print_status("pipe", out_fd);
                if (in_fd >= 0) {
                    close(in_fd);
                }
                break;
            }
        }
        if (in_fd >= 0) {
            saved_in = dup(0);
            dup2(in_fd, 0);
            close(in_fd);
        }
        if (out_fd >= 0) {
            saved_out = dup(1);
            dup2(out_fd, 1);
            close(out_fd);
        }
        exit_shell = run_line(stage[i]);
        if (saved_in >= 0) {
            dup2(saved_in, 0);
            close(saved_in);
        }
        if (saved_out >= 0) {
            dup2(saved_out, 1);
            close(saved_out);
        }
        if (i > 0 && pipe_used[(i - 1) & 1]) {
            unlink(pipe_path[(i - 1) & 1]); /* consumed */
            pipe_used[(i - 1) & 1] = 0;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (pipe_used[i]) {
            unlink(pipe_path[i]);
        }
    }
    return exit_shell;
}

int main(const char *args, size_t len)
{
    char line[LINE_MAX];

    (void)args;
    (void)len;
    puts("axys shell ready (type 'help')\n");
    for (;;) {
        ssize_t n;
        char *seg;
        int done = 0;

        puts(getuid() == 0 ? "axys# " : "axys$ ");
        n = read(0, line, sizeof(line) - 1);
        if (n <= 0) {
            puts("\n");
            break; /* EOF */
        }
        line[n] = '\0';
        if (n > 0 && line[n - 1] == '\n') {
            line[n - 1] = '\0';
        }
        seg = line;
        for (;;) {
            char *next = split_at(seg, ';');

            if (run_pipeline(seg)) {
                done = 1;
                break;
            }
            if (!next) {
                break;
            }
            seg = next;
        }
        if (done) {
            break;
        }
    }
    return 0;
}
