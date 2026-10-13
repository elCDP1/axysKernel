#include "axys.h"
#include "axys/timeconv.h"
#include "shparse.h"

/* /sbin/init: a small interactive shell in the spirit of busybox.
 *
 * Builtins mirror embedded-linux commands with the same behaviour, minus
 * what the kernel does not provide yet (no symlinks, no signals, no fork
 * or job control: `help` says so instead of faking it). Anything showing
 * driver state reads it live -- net_stat for `ip`, meminfo for `free`,
 * /proc snapshots for the rest -- so no command prints canned text. Paths
 * may be absolute or relative to the shell's working directory (`cd`,
 * `pwd`); the kernel canonicalizes //, /./ and /../ on use, and the shell
 * keeps cwd canonical itself. Command lines support quoting, `;` chaining,
 * `|` pipes (temporary files) and < > >> redirection, all parsed by
 * user/shparse.h with no quoting surprises. */

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
#define E_EXIST 17
#define E_NOTDIR 20
#define E_NOTEMPTY 39
#define E_PERM 1
#define E_ACCES 13

/* ---- interface language: `lang [en|es]`, system-wide in /etc/lang ----
 * Spanish strings stay ASCII-only on purpose (no accents): the VGA text
 * console drops bytes above 0x7E, so "contrasena" reads everywhere while
 * "contrase\xc3\xb1a" would print "contrasea". Per-command `help CMD`
 * texts stay English for now (documented in `help lang`). */
static int lang_es; /* 0 English, 1 Spanish */

struct lang_str {
    const char *key;
    const char *en;
    const char *es;
};

static const struct lang_str lang_tab[] = {
    {"banner2", "'help' lists commands, 'help CMD' explains one.",
                "'help' lista los comandos, 'help CMD' explica uno."},
    {"banner3", "';' chains, '|' pipes, < > >> redirect.",
                "';' encadena, '|' entuba, < > >> redirige."},
    {"unknown", "unknown command: ", "comando desconocido: "},
    {"overwrite", "overwrite ", "sobrescribir "},
    {"newpw", "New password: ", "Nueva contrasena: "},
    {"retype", "Retype new password: ", "Repite la nueva contrasena: "},
    {"pwupdated", "Password updated.\n", "Contrasena actualizada.\n"},
    {"loginname", "login: ", "usuario: "},
    {"password", "Password: ", "Contrasena: "},
    {"help0", "axys shell commands. `help CMD` shows one command in detail.\n",
              "comandos del shell axys. `help CMD` muestra uno en detalle.\n"},
    {"help1", "Words split on spaces unless 'quoted' or \"quoted\" (\\ escapes a byte).\n",
              "Las palabras se separan por espacios salvo 'citadas' o \"citadas\" (\\ escapa un byte).\n"},
    {"help2", "Chain lines with ';', pipe stages with '|', redirect with < > >>.\n",
              "Encadena con ';', entuba con '|', redirige con < > >>.\n"},
    {"help3", "Try: ls -l /bin | head -n 5     programs through a filter\n",
              "Prueba: ls -l /bin | head -n 5     programas por un filtro\n"},
    {"help4", "Try: grep -ri main /bin | wc -l count hits across programs\n",
              "Prueba: grep -ri main /bin | wc -l cuenta lineas en programas\n"},
};

static const char *T(const char *key)
{
    size_t n = sizeof(lang_tab) / sizeof(lang_tab[0]);

    for (size_t i = 0; i < n; ++i) {
        if (strcmp(lang_tab[i].key, key) == 0) {
            return lang_es ? lang_tab[i].es : lang_tab[i].en;
        }
    }
    return key;
}

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

/* Decimal, zero-padded on the left to `width` (`seq -w`). */
static void put_dec_pad(u64 value, int width)
{
    char buf[24];
    int n = 0;

    do {
        buf[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 && n < (int)sizeof(buf));
    while (n < width) {
        puts("0");
        ++n;
    }
    while (n > 0) {
        char c = buf[--n];

        write(1, &c, 1);
    }
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
static int glob_star(const char *pat, const char *s);
static int parse_symbolic(const char *spec, u32 old, int is_dir, u32 *out);
static int passwd_scan(u32 want_uid, const char *want_name, u32 *uid, u32 *gid, char *name,
                       size_t cap);
static int passwd_resolve(const char *arg, u32 *uid, u32 *gid, char *name, size_t cap);
static char *split_pw(char *line, u32 *uid, u32 *gid, int *ok);
static char *split_group(char *line, u32 *gid, char **members, int *ok);
static int group_name(u32 gid, char *out, size_t cap);
static int group_gid(const char *name, u32 *gid);
static int passwd_uid(const char *name, u32 *uid);
static int passwd_name(u32 uid, char *out, size_t cap);
static int passwd_home(u32 uid, char *out, size_t cap);
static int read_line(int fd, char *line, size_t cap);
static ssize_t write_all(int fd, const char *buf, size_t len);
static int confirm(const char *path);
static int is_builtin(const char *name);
static int confirm(const char *path);
static int confirm(const char *path);

static void ls_one(const char *path);
static void ls_recursive(const char *path);

static int ls_long; /* -l */
static int ls_classify; /* -F: '/' for dirs, '*' for executables */
static int ls_dirs_only; /* -d: list directories themselves, not contents */
static int ls_human; /* -h: human sizes in -l */

static void ls_print(const char *name, const struct stat_info *st)
{
    if (ls_long) {
        put_mode(st->type, st->mode);
        puts(" ");
        put_u64(st->uid);
        puts(" ");
        put_u64(st->gid);
        puts(" ");
        if (ls_human) {
            put_human(st->size);
        } else {
            put_u64(st->size);
        }
        puts("\t");
    }
    puts(name);
    if (st->type == 2) {
        puts("/\n");
    } else if (ls_classify && (st->mode & 0111u) != 0) {
        puts("*\n");
    } else {
        puts("\n");
    }
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

/* ls [-lRFdh] [PATH...]: a header per path only when listing several. */
static void cmd_ls(char *rest)
{
    char *paths[16];
    int count = 0;
    int recursive = 0;
    char *arg;

    ls_long = 0;
    ls_classify = 0;
    ls_dirs_only = 0;
    ls_human = 0;
    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (arg[0] == '-' && arg[1] != '\0') {
            for (const char *f = arg + 1; *f; ++f) {
                if (*f == 'l') ls_long = 1;
                else if (*f == 'R') recursive = 1;
                else if (*f == 'a') { /* no hidden files here: accepted for habit */ }
                else if (*f == 'F') ls_classify = 1;
                else if (*f == 'd') ls_dirs_only = 1;
                else if (*f == 'h') ls_human = 1;
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
        if (ls_dirs_only) {
            struct stat_info st;

            if (stat(path, &st) != 0) {
                print_status("ls", -E_NOENT);
                return;
            }
            ls_print(path, &st);
        } else if (recursive) {
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
        if (ls_dirs_only) {
            struct stat_info st;

            if (stat(path, &st) != 0) {
                print_status(paths[i], -E_NOENT);
                continue;
            }
            ls_print(path, &st);
        } else if (recursive) {
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

/* cat [-nET] [FILE...|-]: concatenation keeps one numbering/begin state,
 * like GNU. -E marks line ends with $, -T shows tabs as ^I, lone - is stdin. */
static void cat_stream(int fd, int mode, u64 *lineno, int *bol)
{
    char buf[128];
    ssize_t n;
    int numbered = mode & 1, ends = mode & 2, tabs = mode & 4;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        size_t i = 0;

        if (!numbered && !ends && !tabs) {
            write(1, buf, (size_t)n);
            continue;
        }
        while (i < (size_t)n) {
            size_t j = i;
            size_t k;

            if (*bol && numbered) {
                put_u64((*lineno)++);
                puts("\t");
                *bol = 0;
            }
            while (j < (size_t)n && buf[j] != '\n') {
                ++j;
            }
            for (k = i; k < j;) {
                size_t m = k;

                while (m < j && (!tabs || buf[m] != '\t')) {
                    ++m;
                }
                if (m > k) {
                    write(1, buf + k, m - k);
                }
                if (m < j) {
                    puts("^I");
                    k = m + 1;
                } else {
                    k = m;
                }
            }
            if (j < (size_t)n) {
                if (ends) {
                    puts("$");
                }
                write(1, "\n", 1);
                *bol = 1;
                ++j;
            }
            i = j;
        }
    }
}

static void cmd_cat(char *rest)
{
    char path[PATH_MAX];
    int fd;
    int mode = 0;
    u64 lineno = 1;
    int bol = 1;
    char *file;

    for (file = next_arg(&rest); file && file[0] == '-' && file[1] != '\0';
         file = next_arg(&rest)) {
        for (const char *f = file + 1; *f; ++f) {
            if (*f == 'n') {
                mode |= 1;
            } else if (*f == 'E') {
                mode |= 2;
            } else if (*f == 'T') {
                mode |= 4;
            } else {
                print_status("cat", -E_INVAL);
                return;
            }
        }
    }
    if (!file) {
        cat_stream(0, mode, &lineno, &bol); /* no FILE: copy stdin */
        return;
    }
    for (; file; file = next_arg(&rest)) {
        if (strcmp(file, "-") == 0) {
            cat_stream(0, mode, &lineno, &bol);
            continue;
        }
        resolve(file, path);
        fd = open(path, 0);
        if (fd < 0) {
            print_status("cat", fd);
            return;
        }
        cat_stream(fd, mode, &lineno, &bol);
        close(fd);
    }
}

/* echo [-n] [-e] TEXT [> FILE | >> FILE] */
static void echo_word(const char *s, int esc, int *stop)
{
    for (; *s; ++s) {
        char c = *s;

        if (esc && c == '\\') {
            char d = *++s;

            if (d == '\0') {
                write(1, "\\", 1);
                return;
            }
            switch (d) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'a': c = '\x07'; break;
            case 'b': c = '\b'; break;
            case 'e': c = '\x1b'; break;
            case 'f': c = '\x0c'; break;
            case 'v': c = '\x0b'; break;
            case 'c': *stop = 1; return; /* stop output, no newline */
            case '\\': c = '\\'; break;
            default: write(1, "\\", 1); c = d; break;
            }
        }
        write(1, &c, 1);
    }
}

static void cmd_echo(char *rest)
{
    int nonl = 0;
    int esc = 0;
    int first = 1;
    int printed = 0;
    int stop = 0;
    char *arg;

    /* Redirection (`echo hi > file`) is handled for every command by the
     * pipeline layer; echo just prints its (unquoted) words. */
    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        /* Single-letter flags, alone or clustered (-n, -ne). Anything else
         * starting with '-' is an ordinary word, like GNU. */
        if (first && arg[0] == '-' && arg[1] != '\0') {
            int ok = 1;

            for (const char *f = arg + 1; *f; ++f) {
                if (*f == 'n') {
                    nonl = 1;
                } else if (*f == 'e') {
                    esc = 1;
                } else if (*f == 'E') {
                    esc = 0;
                } else {
                    ok = 0;
                    break;
                }
            }
            if (ok) {
                first = 0;
                continue;
            }
        }
        first = 0;
        if (printed) {
            puts(" ");
        }
        echo_word(arg, esc, &stop);
        if (stop) {
            return;
        }
        printed = 1;
    }
    if (!nonl && !stop) {
        puts("\n");
    }
}
static void cmd_touch(char *rest)
{
    char *arg = next_arg(&rest);
    char path[PATH_MAX];
    int nocreate = 0;

    if (arg && strcmp(arg, "-c") == 0) {
        nocreate = 1;
        arg = next_arg(&rest);
    }
    if (!arg) {
        print_status("touch", -E_INVAL);
        return;
    }
    for (; arg; arg = next_arg(&rest)) {
        int fd;
        struct stat_info st;

        resolve(arg, path);
        if (nocreate) {
            /* -c: never create; existing entries are already "touched"
             * (this VFS keeps no timestamps). */
            if (stat(path, &st) != 0) {
                continue;
            }
            if (st.type != 1) {
                continue;
            }
        }
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
    int interactive = 0;
    int r;
    struct stat_info st;

    while (src && src[0] == '-' && src[1] != '\0') {
        int ok = 1;

        for (const char *f = src + 1; *f; ++f) {
            if (*f == 'r' || *f == 'R') {
                recursive = 1;
            } else if (*f == 'i') {
                interactive = 1;
            } else {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            break;
        }
        src = next_arg(&rest);
    }
    dst = next_arg(&rest);
    if (!src || !dst || next_arg(&rest)) {
        print_status("cp", -E_INVAL);
        return;
    }
    resolve(src, spath);
    resolve(dst, dpath);
    if (interactive && stat(dpath, &st) == 0 && !confirm(dpath)) {
        return;
    }
    if (recursive) {
        r = cp_tree(spath, dpath, 0);
    } else {
        r = cp_file(spath, dpath);
    }
    if (r != 0) {
        print_status("cp", r);
    }
}

/* head/tail [-N | -n N | -c N | -q | -v] [FILE...]: lines by default,
 * bytes with -c. -q never prints headers, -v always does; otherwise several
 * files get "==> file <==" headers. tail rewinds and skips: two streaming
 * passes, no heap, works on any file size. Console stdin cannot rewind, so
 * tail spills it to a temp file first (same trick as pipelines); anything
 * else streams straight off fd 0. */
static void head_tail_fd(int fd, int is_stdin, const char *name, int is_tail, int bytes, u64 n)
{
    char buf[IO_BUF];
    ssize_t r;
    int temp = 0;
    char tpath[40];

    if (bytes && !is_tail) {
        u64 left = n;

        while (left > 0 && (r = read(fd, buf, sizeof(buf))) > 0) {
            size_t take = (u64)r < left ? (size_t)r : (size_t)left;

            write(1, buf, take);
            left -= take;
        }
        close(fd);
        return;
    }
    if (!is_tail) {
        u64 lines = 0;

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
        /* Console stdin cannot rewind: spill it to a temp file first.
         * Probe with a no-op seek (harmless on files, EBADF on consoles);
         * pipelines already hand us rewindable files. */
        if (is_stdin && lseek(fd, 0, 0) != 0) {
            int tf;
            ssize_t k;
            char *w = tpath;
            const char *base = "/tmp/.tail";
            u64 v = (u64)getpid();
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
            *w = '\0';
            tf = open(tpath, O_CREAT | O_TRUNC | O_WRONLY);
            if (tf < 0) {
                print_status(name, tf);
                return;
            }
            while ((k = read(0, buf, sizeof(buf))) > 0) {
                if (write(tf, buf, (size_t)k) != k) {
                    print_status(name, -12);
                    close(tf);
                    unlink(tpath);
                    return;
                }
            }
            close(tf);
            fd = open(tpath, 0);
            if (fd < 0) {
                print_status(name, fd);
                unlink(tpath);
                return;
            }
            temp = 1;
        }
        if (bytes) {
            /* Pass 1: total bytes; pass 2: skip total-n, print the rest. */
            u64 total = 0;
            u64 skip;

            while ((r = read(fd, buf, sizeof(buf))) > 0) {
                total += (u64)r;
            }
            if (lseek(fd, 0, 0) < 0) {
                print_status(name, -5);
                goto tail_done;
            }
            skip = total > n ? total - n : 0;
            while ((r = read(fd, buf, sizeof(buf))) > 0) {
                size_t from = 0;

                if (skip > 0) {
                    size_t drop = (u64)r < skip ? (size_t)r : (size_t)skip;

                    skip -= drop;
                    from = drop;
                }
                if (from < (size_t)r) {
                    write(1, buf + from, (size_t)r - from);
                }
            }
        } else {
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
            goto tail_done;
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
        }
    tail_done:
        if (temp) {
            close(fd);
            unlink(tpath);
        } else {
            close(fd);
        }
    }
}

static void cmd_head_tail(char *rest, int is_tail)
{
    const char *name = is_tail ? "tail" : "head";
    char *files[16];
    int nfiles = 0;
    char *arg;
    u64 n = 10;
    int bytes = 0;
    int quiet = 0, verbose = 0;

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (arg[0] == '-' && arg[1] != '\0') {
            /* -n N, -c N, -nN, -cN, bare -N, -q, -v, clustered (-qv). */
            if ((arg[1] == 'n' || arg[1] == 'c') && arg[2] == '\0') {
                const char *a = next_arg(&rest);
                u32 v = 0;

                if (!a || parse_num(&a, 10, &v) != 0) {
                    print_status(name, -E_INVAL);
                    return;
                }
                n = v;
                bytes = arg[1] == 'c';
            } else if ((arg[1] == 'n' || arg[1] == 'c') && arg[2] >= '0' && arg[2] <= '9') {
                const char *a = arg + 2;
                u32 v = 0;

                if (parse_num(&a, 10, &v) != 0) {
                    print_status(name, -E_INVAL);
                    return;
                }
                n = v;
                bytes = arg[1] == 'c';
            } else {
                int flags_only = 1;

                for (const char *f = arg + 1; *f; ++f) {
                    if (*f == 'q') {
                        quiet = 1;
                    } else if (*f == 'v') {
                        verbose = 1;
                    } else if (*f >= '0' && *f <= '9') {
                        const char *a = f;
                        u32 v = 0;

                        if (parse_num(&a, 10, &v) != 0) {
                            print_status(name, -E_INVAL);
                            return;
                        }
                        n = v;
                        f = a - 1; /* the for-step advances past the digits */
                    } else {
                        flags_only = 0;
                        break;
                    }
                }
                if (!flags_only) {
                    print_status(name, -E_INVAL);
                    return;
                }
            }
        } else if (nfiles < 16) {
            files[nfiles++] = arg;
        } else {
            print_status(name, -E_INVAL);
            return;
        }
    }
    {
        int header = verbose || (!quiet && nfiles > 1);

        if (nfiles == 0) {
            if (header) {
                puts("==> standard input <==\n");
            }
            head_tail_fd(0, 1, name, is_tail, bytes, n);
            return;
        }
        for (int i = 0; i < nfiles; ++i) {
            char path[PATH_MAX];
            int fd;

            resolve(files[i], path);
            fd = open(path, 0);
            if (fd < 0) {
                print_status(files[i], fd);
                continue;
            }
            if (header) {
                if (i > 0) {
                    puts("\n");
                }
                puts("==> ");
                puts(files[i]);
                puts(" <==\n");
            }
            head_tail_fd(fd, 0, name, is_tail, bytes, n);
        }
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

/* Same digits to fd 2, for diagnostics that must not pollute pipes. */
static void put_u64_e(u64 v)
{
    char num[24];
    int n = 0;

    do {
        num[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        write(2, &num[--n], 1);
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
    u64 max; /* -m: stop after this many matches (0 = no limit) */
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
        for (ssize_t i = 0; i < r && !(o->max && (u64)hits >= o->max); ++i) {
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
    if (r == 0 && partial && !(o->max && (u64)hits >= o->max)) { /* last line without a newline */
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
    struct grep_opts o = {0, 0, 0, 0, 0, 0};
    int recursive = 0;
    char *arg;
    char *pat = 0;
    int nfiles = 0;
    char *files[16];

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (!pat && arg[0] == '-' && arg[1] != '\0') {
            /* -m takes a number: -m N or -mN, never clustered. */
            if (arg[1] == 'm' && (arg[2] == '\0' || (arg[2] >= '0' && arg[2] <= '9'))) {
                const char *a = arg[2] ? arg + 2 : next_arg(&rest);
                u32 v = 0;

                if (!a || parse_num(&a, 10, &v) != 0 || v == 0) {
                    print_status("grep", -E_INVAL);
                    return;
                }
                o.max = v;
                continue;
            }
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
 * depth cap only guards pathological nesting. With `sym` set, each node gets
 * its own symbolic evaluation (X and copies depend on the node). */
static int chmod_tree(const char *path, u32 mode, u32 uid, u32 gid, int do_chown, int depth,
                      const char *sym)
{
    struct stat_info st;
    int first = 0;
    int r;

    if (stat(path, &st) != 0) {
        return -E_NOENT;
    }
    if (sym) {
        u32 want = 0;

        if (parse_symbolic(sym, st.mode, st.type == 2, &want) != 0) {
            return -E_INVAL;
        }
        r = chmod(path, want);
    } else {
        r = do_chown ? chown(path, uid, gid) : chmod(path, mode);
    }
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
        r = chmod_tree(child, mode, uid, gid, do_chown, depth + 1, sym);
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
static int find_filtered(const char *path, const char *pat, int ftype, int maxdepth, int del,
                         int have_exec, char (*exec_argv)[64], int exec_argc, int show, int depth);
static int parse_symbolic(const char *spec, u32 old, int is_dir, u32 *out);
static int run_line(char *line);

static void cmd_find(char *rest)
{
    char path[PATH_MAX];
    char *arg;
    char *pat = 0;
    char *dir = 0;
    int ftype = 0;   /* 0 any, 1 files only, 2 dirs only */
    int maxdepth = -1; /* -1 unlimited */
    int del = 0;
    int show = 0;    /* explicit -print */
    char exec_argv[8][64];
    int exec_argc = 0;
    int have_exec = 0;

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (strcmp(arg, "-name") == 0) {
            pat = next_arg(&rest);
            if (!pat) {
                print_status("find", -E_INVAL);
                return;
            }
        } else if (strcmp(arg, "-type") == 0) {
            char *t = next_arg(&rest);

            if (!t || (t[0] != 'f' && t[0] != 'd') || t[1] != '\0') {
                print_status("find", -E_INVAL);
                return;
            }
            ftype = t[0] == 'f' ? 1 : 2;
        } else if (strcmp(arg, "-maxdepth") == 0) {
            const char *a = next_arg(&rest);
            u32 v = 0;

            if (!a || parse_num(&a, 10, &v) != 0) {
                print_status("find", -E_INVAL);
                return;
            }
            maxdepth = v > 16 ? 16 : (int)v;
        } else if (strcmp(arg, "-delete") == 0) {
            del = 1;
        } else if (strcmp(arg, "-print") == 0) {
            show = 1;
        } else if (strcmp(arg, "-exec") == 0) {
            char *a = next_arg(&rest);

            if (!a) {
                print_status("find", -E_INVAL);
                return;
            }
            for (; a && strcmp(a, ";") != 0; a = next_arg(&rest)) {
                size_t k;

                if (exec_argc >= 8) {
                    print_status("find", -E_INVAL);
                    return;
                }
                k = strlen(a);
                if (k >= sizeof(exec_argv[0])) {
                    print_status("find", -E_INVAL);
                    return;
                }
                memcpy(exec_argv[exec_argc], a, k + 1);
                ++exec_argc;
            }
            if (!a || exec_argc == 0) {
                print_status("find", -E_INVAL);
                return;
            }
            have_exec = 1;
        } else if (!dir) {
            dir = arg;
        } else {
            print_status("find", -E_INVAL);
            return;
        }
    }
    if (del && have_exec) {
        print_status("find", -E_INVAL);
        return;
    }
    if (!dir) {
        memcpy(path, cwd, strlen(cwd) + 1);
    } else {
        resolve(dir, path);
    }
    if (!pat && ftype == 0 && maxdepth < 0 && !del && !have_exec) {
        if (walk(path, 0, 0, 1) != 0) {
            print_status("find", -E_NOENT);
        }
        return;
    }
    if (find_filtered(path, pat, ftype, maxdepth, del, have_exec, exec_argv, exec_argc,
                       show || (!del && !have_exec), 0) != 0) {
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


/* Set when a nested -exec runs `exit`: run_pipeline() turns it into a real
 * shell exit, since the void cmd_find cannot return one itself. */
static int find_nested_exit;

/* Filtered walk for -name/-type/-maxdepth with -print/-delete/-exec
 * actions. Returns -1 only when the top path is missing/unreadable (the
 * caller reports it); per-entry action errors print inline and continue,
 * like GNU. `pat` NULL matches everything. */
static int find_filtered(const char *path, const char *pat, int ftype, int maxdepth, int del,
                         int have_exec, char (*exec_argv)[64], int exec_argc, int show,
                         int depth)
{
    struct stat_info st;
    char name[64];
    char child[PATH_MAX];

    if (stat(path, &st) != 0) {
        return -1;
    }
    {
        const char *base = path;
        const char *slash = path;
        int is_dir = st.type == 2;
        int match;
        int descend;

        while (*slash) {
            if (*slash++ == '/') {
                base = slash;
            }
        }
        match = (!pat || glob_star(pat, base)) &&
                (ftype == 0 || (ftype == 1 && !is_dir) || (ftype == 2 && is_dir));
        descend = is_dir && depth <= 16 && (maxdepth < 0 || depth < maxdepth);
        if (match && del && !(is_dir && descend)) {
            int r = unlink(path);

            if (r < 0) {
                print_status("find", r);
            }
        } else if (match && have_exec) {
            char cmd[LINE_MAX];
            size_t off = 0;
            int i;

            for (i = 0; i < exec_argc; ++i) {
                const char *a = exec_argv[i];
                size_t k = 0;

                if (i > 0) {
                    if (off + 1 >= sizeof(cmd)) {
                        print_status("find", -E_INVAL);
                        return 0;
                    }
                    cmd[off++] = ' ';
                }
                for (;;) {
                    const char *brace = a + k;
                    size_t blen = 0;

                    while (brace[blen] && (brace[blen] != '{' || brace[blen + 1] != '}')) {
                        ++blen;
                    }
                    if (off + blen >= sizeof(cmd)) {
                        print_status("find", -E_INVAL);
                        return 0;
                    }
                    memcpy(cmd + off, a + k, blen);
                    off += blen;
                    k += blen;
                    if (a[k] == '\0') {
                        break;
                    }
                    /* Found "{}": substitute the path. */
                    blen = strlen(path);
                    if (off + blen >= sizeof(cmd)) {
                        print_status("find", -E_INVAL);
                        return 0;
                    }
                    memcpy(cmd + off, path, blen);
                    off += blen;
                    k += 2;
                }
            }
            cmd[off] = '\0';
            if (run_line(cmd)) {
                find_nested_exit = 1;
                return 0;
            }
        } else if (match && show) {
            puts(path);
            puts(is_dir ? "/\n" : "\n");
        }
        if (!is_dir || depth > 16 || (maxdepth >= 0 && depth >= maxdepth)) {
            return 0;
        }
        for (u64 i = 0;; ++i) {
            int r = readdir(path, i, name, sizeof(name));
            size_t pl = strlen(path);

            if (r < 0) {
                return 0; /* entries already shown stay shown */
            }
            if (r == 0) {
                break;
            }
            if (pl + strlen(name) + 2 >= sizeof(child)) {
                continue;
            }
            memcpy(child, path, pl);
            if (pl == 0 || child[pl - 1] != '/') {
                child[pl++] = '/';
            }
            memcpy(child + pl, name, strlen(name) + 1);
            if (find_filtered(child, pat, ftype, maxdepth, del, have_exec, exec_argv,
                               exec_argc, show, depth + 1) != 0) {
                return -1;
            }
            if (find_nested_exit) {
                return 0;
            }
        }
        /* -delete unlinks a matched directory after its children
         * (bottom-up), so removing a whole tree actually empties it. */
        if (del && match) {
            int r = unlink(path);

            if (r < 0) {
                print_status("find", r);
            }
        }
        return 0;
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

/* xargs [-n MAX] [-0] CMD [ARGS...]: append stdin lines to CMD and run it,
 * batching to MAX args (default: everything fitting one command line).
 * -0 splits on NUL instead of newlines. Empty input still runs once. */
#define XARGS_MAX_BYTES 65536u
#define XARGS_MAX_ITEMS 1024u
#define XARGS_ARG_CAP 112u /* spawn truncates past AXYS_ARGS_MAX */

static char xargs_buf[XARGS_MAX_BYTES + 1u];
static char *xargs_item[XARGS_MAX_ITEMS];

static void cmd_xargs(char *rest)
{
    char *arg;
    u64 maxargs = 0;
    int zero = 0;
    char *cmd_argv[16];
    int ncmd = 0;
    char prog[PATH_MAX];
    int nitems = 0;
    int r;

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (arg[0] == '-' && arg[1] != '\0' && ncmd == 0) {
            if (strcmp(arg, "-0") == 0) {
                zero = 1;
            } else if (strcmp(arg, "-n") == 0) {
                const char *a = next_arg(&rest);
                u32 v = 0;

                if (!a || parse_num(&a, 10, &v) != 0 || v == 0) {
                    print_status("xargs", -E_INVAL);
                    return;
                }
                maxargs = v;
            } else {
                print_status("xargs", -E_INVAL);
                return;
            }
        } else if (ncmd < 16) {
            cmd_argv[ncmd++] = arg;
        } else {
            print_status("xargs", -E_INVAL);
            return;
        }
    }
    if (ncmd == 0) {
        print_status("xargs", -E_INVAL);
        return;
    }
    /* Slurp stdin raw (NULs kept for -0), then split in place. */
    {
        char buf[IO_BUF];
        ssize_t k;
        size_t len = 0;
        int truncated = 0;

        while ((k = read(0, buf, sizeof(buf))) > 0) {
            size_t take = (size_t)k;

            if (len + take > XARGS_MAX_BYTES) {
                take = XARGS_MAX_BYTES - len;
                truncated = 1;
            }
            memcpy(xargs_buf + len, buf, take);
            len += take;
            if (truncated || len == XARGS_MAX_BYTES) {
                char probe;

                if (!truncated && read(0, &probe, 1) > 0) {
                    truncated = 1;
                }
                break;
            }
        }
        if (k < 0) {
            print_status("xargs", (int)k);
            return;
        }
        for (size_t i = 0; i < len && nitems < (int)XARGS_MAX_ITEMS;) {
            size_t j = i;

            while (j < len && xargs_buf[j] != (zero ? '\0' : '\n')) {
                ++j;
            }
            xargs_buf[j] = '\0'; /* terminator or overwrites the delimiter */
            if (j > i) {
                xargs_item[nitems++] = xargs_buf + i;
            }
            i = j + 1;
        }
        if (nitems == XARGS_MAX_ITEMS) {
            /* Extra input beyond the cap is dropped, loudly. */
            eputs("xargs: input truncated\n");
        }
    }
    if (cmd_argv[0][0] == '/') {
        resolve(cmd_argv[0], prog);
    } else {
        size_t cl = strlen(cmd_argv[0]);

        if (cl + 6 >= 64) {
            print_status("xargs", -E_INVAL);
            return;
        }
        memcpy(prog, "/bin/", 5);
        memcpy(prog + 5, cmd_argv[0], cl + 1);
    }
    /* Fail once up front (not once per batch): missing programs and shell
     * builtins (echo, cat, ...) cannot run here, only /bin programs. */
    {
        struct stat_info pst;

        if (stat(prog, &pst) != 0 || pst.type != 1) {
            print_status("xargs", -E_NOENT);
            return;
        }
    }
    /* Batch: at most maxargs items and XARGS_ARG_CAP bytes per run. */
    {
        int at = 0;

        do {
            char args[128];
            size_t off = 0;
            u64 batch = 0;

            for (int i = 1; i < ncmd; ++i) {
                size_t k = strlen(cmd_argv[i]);

                if (off + k + 2 >= sizeof(args)) {
                    print_status("xargs", -E_INVAL);
                    return;
                }
                if (off > 0) {
                    args[off++] = ' ';
                }
                memcpy(args + off, cmd_argv[i], k);
                off += k;
            }
            while (at < nitems && (maxargs == 0 || batch < maxargs)) {
                size_t k = strlen(xargs_item[at]);

                if (off + k + 2 >= XARGS_ARG_CAP) {
                    break;
                }
                if (off > 0) {
                    args[off++] = ' ';
                }
                memcpy(args + off, xargs_item[at], k);
                off += k;
                ++at;
                ++batch;
            }
            args[off] = '\0';
            r = run(prog, off > 0 ? args : "");
            (void)r;
        } while (at < nitems);
    }
}

/* file FILE...: hand-rolled magic (no magic DB here): ELF details from the
 * header, #! scripts, empty/text/data, plus executable bit when set. */
#define FILE_SAMPLE 512u
static void cmd_file(char *rest)
{
    char *arg = next_arg(&rest);

    if (!arg) {
        print_status("file", -E_INVAL);
        return;
    }
    for (; arg; arg = next_arg(&rest)) {
        char path[PATH_MAX];
        struct stat_info st;
        int fd;
        int is_elf = 0;
        unsigned char head[FILE_SAMPLE];
        ssize_t n;
        ssize_t total = 0;

        resolve(arg, path);
        if (stat(path, &st) != 0) {
            print_status(arg, -E_NOENT);
            continue;
        }
        if (st.type == 2) {
            puts(path);
            puts(": directory\n");
            continue;
        }
        fd = open(path, 0);
        if (fd < 0) {
            print_status(arg, fd);
            continue;
        }
        while (total < (ssize_t)sizeof(head)) {
            n = read(fd, head + total, sizeof(head) - (size_t)total);
            if (n <= 0) {
                break;
            }
            total += n;
        }
        close(fd);
        if (n < 0) {
            print_status(arg, (int)n);
            continue;
        }
        puts(path);
        puts(": ");
        if (total == 0) {
            puts("empty");
        } else if (total >= 4 && head[0] == 0x7f && head[1] == 'E' && head[2] == 'L' &&
                   head[3] == 'F') {
            /* e_type at 16, e_machine at 18 (little-endian fields; a
             * big-endian image still matches the magic line). */
            unsigned type = total >= 18 ? (unsigned)head[16] | ((unsigned)head[17] << 8) : 0;
            unsigned mach = total >= 20 ? (unsigned)head[18] | ((unsigned)head[19] << 8) : 0;
            const char *kind = type == 2 ? "executable" :
                               type == 3 ? "pie executable" :
                               type == 1 ? "relocatable" : "unknown type";
            const char *arch = mach == 62 ? "x86-64" : mach == 3 ? "i386" : "unknown-machine";

            puts("ELF ");
            put_u64(head[4] == 2 ? 64 : head[4] == 1 ? 32 : 0);
            puts("-bit LSB ");
            puts(kind);
            puts(", ");
            puts(arch);
            if (head[4] != 1 && head[4] != 2) {
                puts(" (odd class byte)");
            }
            is_elf = 1;
        } else if (total >= 2 && head[0] == '#' && head[1] == '!') {
            size_t e = 2;

            while (e < (size_t)total && head[e] != '\n') {
                ++e;
            }
            {
                char interp[48];
                size_t k = e - 2;

                if (k >= sizeof(interp)) {
                    k = sizeof(interp) - 1;
                }
                memcpy(interp, head + 2, k);
                interp[k] = '\0';
                puts(interp);
                puts(" script");
            }
        } else {
            int text = 1;

            for (ssize_t i = 0; i < total; ++i) {
                unsigned char c = head[i];

                if (c != '\n' && c != '\t' && c != '\r' && (c < 0x20 || c > 0x7e)) {
                    text = 0;
                    break;
                }
            }
            /* Only the first FILE_SAMPLE bytes were read: claim text only
             * when the whole file fit (else "data" would lie about binaries
             * with a clean-looking header). */
            if (text && (u64)total < (u64)st.size) {
                text = 0;
            }
            puts(text ? "ASCII text" : "data");
        }
        if (!is_elf && (st.mode & 0111u) != 0 && total > 0) {
            puts(", executable");
        }
        puts("\n");
    }
}

/* type NAME...: shell builtin, /bin path, or missing. */
static void cmd_type(char *rest)
{
    char *arg = next_arg(&rest);

    if (!arg) {
        print_status("type", -E_INVAL);
        return;
    }
    for (; arg; arg = next_arg(&rest)) {
        if (is_builtin(arg)) {
            puts(arg);
            puts(" is a shell builtin\n");
            continue;
        }
        if (arg[0] == '/') {
            char path[PATH_MAX];
            struct stat_info st;

            resolve(arg, path);
            if (stat(path, &st) == 0) {
                puts(path);
                puts("\n");
            } else {
                print_status("type", -E_NOENT);
            }
            continue;
        }
        {
            char path[64];
            struct stat_info st;
            size_t cl = strlen(arg);

            if (cl + 6 >= sizeof(path)) {
                print_status("type", -E_INVAL);
                continue;
            }
            memcpy(path, "/bin/", 5);
            memcpy(path + 5, arg, cl + 1);
            if (stat(path, &st) == 0) {
                puts(path);
                puts("\n");
            } else {
                print_status("type", -E_NOENT);
            }
        }
    }
}

/* uniq [-cdu] [FILE]: collapse adjacent duplicate lines (stdin by default).
 * -d prints only repeated lines, -u only unique ones. */

/* gen_suffix: counter n in `base` (10 digits or 26 letters) exactly `len`
 * chars into out (no NUL). Returns 0, or -1 when n needs more digits. */
static int gen_suffix(char *out, u64 n, int base, int len)
{
    char tmp[20];
    int nd = 0;

    do {
        tmp[nd++] = (char)(n % (u64)base);
        n /= (u64)base;
    } while (n != 0);
    if (nd > len) {
        return -1;
    }
    for (int i = 0; i < len; ++i) {
        int d = (i < len - nd) ? 0 : (tmp[len - 1 - i] & 0xff);

        out[i] = base == 10 ? (char)('0' + d) : (char)('a' + d);
    }
    return 0;
}

/* split [-l N | -b N] [-d] [-a LEN] [FILE] [PREFIX]: default 1000 lines,
 * prefix "x", 2-letter suffixes. Suffixes exhaust at base^len. */
/* Open PREFIX+suffix(piece) for writing. Prints diagnostics, returns the
 * fd or a negative errno. */
static int split_open(const char *prefix, u64 piece, int numeric, int suflen)
{
    char outpath[PATH_MAX];
    char suf[9];
    size_t pl = strlen(prefix);
    int out;

    if (gen_suffix(suf, piece, numeric ? 10 : 26, suflen) != 0 ||
        pl + (size_t)suflen + 1 > sizeof(outpath)) {
        print_status("split", -E_NOSPC);
        return -E_NOSPC;
    }
    memcpy(outpath, prefix, pl);
    memcpy(outpath + pl, suf, (size_t)suflen);
    outpath[pl + (size_t)suflen] = '\0';
    out = open(outpath, O_CREAT | O_TRUNC | O_WRONLY);
    if (out < 0) {
        print_status("split", out);
    }
    return out;
}

static void cmd_split(char *rest)
{
    char *arg;
    u64 count = 1000;
    int bytes = 0;
    int numeric = 0;
    int suflen = 2;
    char *files[2];
    int nfiles = 0;
    char path[PATH_MAX];
    char prefix[PATH_MAX];
    char buf[IO_BUF];
    int fd = 0;
    ssize_t r = 0;
    u64 piece = 0;
    int out = -1;

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (arg[0] == '-' && arg[1] != '\0') {
            if ((arg[1] == 'l' || arg[1] == 'b') && arg[2] == '\0') {
                const char *a = next_arg(&rest);
                u32 v = 0;

                if (!a || parse_num(&a, 10, &v) != 0 || v == 0) {
                    print_status("split", -E_INVAL);
                    return;
                }
                count = v;
                bytes = arg[1] == 'b';
            } else if ((arg[1] == 'l' || arg[1] == 'b') && arg[2] >= '0' && arg[2] <= '9') {
                const char *a = arg + 2;
                u32 v = 0;

                if (parse_num(&a, 10, &v) != 0 || v == 0) {
                    print_status("split", -E_INVAL);
                    return;
                }
                count = v;
                bytes = arg[1] == 'b';
            } else if (strcmp(arg, "-d") == 0) {
                numeric = 1;
            } else if (strcmp(arg, "-a") == 0) {
                const char *a = next_arg(&rest);
                u32 v = 0;

                if (!a || parse_num(&a, 10, &v) != 0 || v < 1 || v > 8) {
                    print_status("split", -E_INVAL);
                    return;
                }
                suflen = (int)v;
            } else {
                print_status("split", -E_INVAL);
                return;
            }
        } else if (nfiles < 2) {
            files[nfiles++] = arg;
        } else {
            print_status("split", -E_INVAL);
            return;
        }
    }
    if (nfiles == 0) {
        fd = 0;
        memcpy(prefix, "x", 2);
    } else {
        if (nfiles == 2) {
            if (strlen(files[1]) >= sizeof(prefix)) {
                print_status("split", -E_INVAL);
                return;
            }
            memcpy(prefix, files[1], strlen(files[1]) + 1);
        } else {
            memcpy(prefix, "x", 2);
        }
        /* PREFIX is relative to cwd, like every other path argument. */
        if (prefix[0] != '/') {
            char tmp[PATH_MAX];
            size_t c = strlen(cwd);
            size_t pl = strlen(prefix);

            if (c + 1 + pl >= sizeof(tmp)) {
                print_status("split", -E_INVAL);
                return;
            }
            memcpy(tmp, cwd, c);
            tmp[c] = '/';
            memcpy(tmp + c + 1, prefix, pl + 1);
            resolve(tmp, prefix);
        }
        resolve(files[0], path);
        fd = open(path, 0);
        if (fd < 0) {
            print_status("split", fd);
            return;
        }
    }
    if (bytes) {
        u64 left = count;

        for (;;) {
            left = count;
            out = split_open(prefix, piece, numeric, suflen);
            if (out < 0) {
                break;
            }
            while (left > 0 && (r = read(fd, buf, sizeof(buf))) > 0) {
                size_t take = (u64)r < left ? (size_t)r : (size_t)left;

                if (write(out, buf, take) != (ssize_t)take) {
                    print_status("split", -12);
                    close(out);
                    out = -1;
                    break;
                }
                left -= take;
            }
            if (out >= 0) {
                close(out);
                out = -1;
            }
            if (r <= 0) {
                break;
            }
            ++piece;
        }
    } else {
        u64 lines = 0;
        int need_open = 1;

        while ((r = read(fd, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < r;) {
                size_t j = i;

                while (j < (size_t)r && buf[j] != '\n') {
                    ++j;
                }
                if (j < (size_t)r) {
                    ++j; /* include the newline */
                }
                if (need_open) {
                    out = split_open(prefix, piece, numeric, suflen);
                    if (out < 0) {
                        goto split_done;
                    }
                    need_open = 0;
                }
                if (write(out, buf + i, j - i) != (ssize_t)(j - i)) {
                    print_status("split", -12);
                    goto split_done;
                }
                i = j;
                if (j > 0 && buf[j - 1] == '\n') {
                    if (++lines >= count) {
                        close(out);
                        out = -1;
                        ++piece;
                        need_open = 1;
                        lines = 0;
                    }
                }
            }
        }
        if (piece == 0 && need_open && r == 0) {
            /* Empty input still yields one empty piece, like GNU. */
            out = split_open(prefix, piece, numeric, suflen);
            if (out >= 0) {
                close(out);
                out = -1;
            }
        }
    split_done:;
    }
    if (out >= 0) {
        close(fd);
        close(out);
    } else if (nfiles > 0) {
        close(fd);
    }
    if (r < 0) {
        print_status("split", (int)r);
    }
}

/* paste [-d LIST] [-s] FILE...: merge lines side by side (default TAB).
 * At most 8 files; lines past 255 bytes truncate (documented subset).
 * -d LIST cycles delimiters (\\n \\t \\ \\ \r escapes); -s joins each
 * file's lines into one output line instead. */
#define PASTE_MAX_FILES 8
#define PASTE_LINE 256

struct paste_src {
    int fd;
    char buf[PASTE_LINE];
    size_t pos;
    size_t len;
    int eof;
};

/* Next line from a source (without newline); 1 ok, 0 at EOF, -1 on error.
 * Over-long lines truncate, remainder skipped so columns stay aligned. */
static int paste_next(struct paste_src *s, char *line)
{
    size_t n = 0;
    int nl = 0;

    for (;;) {
        if (s->pos >= s->len) {
            ssize_t r;

            if (s->eof) {
                break;
            }
            r = read(s->fd, s->buf, sizeof(s->buf));
            if (r < 0) {
                return -1;
            }
            if (r == 0) {
                s->eof = 1;
                break;
            }
            s->pos = 0;
            s->len = (size_t)r;
        }
        while (s->pos < s->len && n + 1 < PASTE_LINE) {
            char c = s->buf[s->pos++];

            if (c == '\n') {
                nl = 1;
                break;
            }
            line[n++] = c;
        }
        if (nl) {
            break;
        }
        if (s->pos < s->len) {
            /* Line too long: skip to its end so the next call starts fresh. */
            while (s->pos < s->len && s->buf[s->pos] != '\n') {
                ++s->pos;
            }
            if (s->pos < s->len) {
                ++s->pos;
            }
            break;
        }
    }
    line[n] = '\0';
    return (n > 0 || nl) ? 1 : 0;
}

/* Parse a paste -d list (escapes \n \t \r \\) into delims; 0 on success. */
static int paste_parse_delims(const char *list, char *delims, size_t cap, size_t *ndelims)
{
    const char *p;
    size_t n = 0;

    if (!list || *list == '\0') {
        return -E_INVAL;
    }
    for (p = list; *p && n < cap; ++p) {
        if (*p == '\\') {
            ++p;
            if (*p == 'n') {
                delims[n++] = '\n';
            } else if (*p == 't') {
                delims[n++] = '\t';
            } else if (*p == 'r') {
                delims[n++] = '\r';
            } else if (*p == '\\') {
                delims[n++] = '\\';
            } else if (*p == '\0') {
                break;
            } else {
                delims[n++] = *p;
            }
        } else {
            delims[n++] = *p;
        }
    }
    if (n == 0 || *p != '\0') {
        return -E_INVAL;
    }
    *ndelims = n;
    return 0;
}

static void cmd_paste(char *rest)
{
    char *arg;
    char *files[PASTE_MAX_FILES];
    int nfiles = 0;
    char delims[32];
    size_t ndelims = 1;
    int serial = 0;
    char path[PATH_MAX];
    struct paste_src src[PASTE_MAX_FILES];

    delims[0] = '\t';
    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (arg[0] == '-' && arg[1] != '\0') {
            if (strcmp(arg, "-s") == 0) {
                serial = 1;
            } else if (strcmp(arg, "-d") == 0 || (arg[1] == 'd' && arg[2] != '\0')) {
                const char *list = arg[2] != '\0' ? arg + 2 : next_arg(&rest);

                if (paste_parse_delims(list, delims, sizeof(delims), &ndelims) != 0) {
                    print_status("paste", -E_INVAL);
                    return;
                }
            } else {
                print_status("paste", -E_INVAL);
                return;
            }
        } else if (nfiles < PASTE_MAX_FILES) {
            files[nfiles++] = arg;
        } else {
            print_status("paste", -E_INVAL);
            return;
        }
    }
    if (nfiles == 0) {
        print_status("paste", -E_INVAL);
        return;
    }
    for (int i = 0; i < nfiles; ++i) {
        resolve(files[i], path);
        src[i].fd = open(path, 0);
        if (src[i].fd < 0) {
            /* Like GNU: report, keep going with empty columns for it. */
            print_status(files[i], src[i].fd);
        }
        src[i].pos = 0;
        src[i].len = 0;
        src[i].eof = 1;
        if (src[i].fd >= 0) {
            src[i].eof = 0;
        }
    }
    if (serial) {
        for (int i = 0; i < nfiles; ++i) {
            char line[PASTE_LINE];
            int first = 1;
            int di = 0;
            int r;

            for (;;) {
                r = paste_next(&src[i], line);
                if (r < 0) {
                    print_status("paste", -5);
                    break;
                }
                if (r == 0) {
                    break;
                }
                if (!first) {
                    write(1, &delims[di % ndelims], 1);
                    di = (di + 1) % (int)ndelims;
                }
                first = 0;
                puts(line);
            }
            puts("\n");
        }
    } else {
        for (;;) {
            /* Fetch all columns before printing: a row goes out only when
             * some file yields a line, so a lagging EOF flag cannot emit
             * phantom delimiter-only rows. Finished files keep empty
             * columns (with their delimiters, like GNU). */
            char cols[PASTE_MAX_FILES][PASTE_LINE];
            int got[PASTE_MAX_FILES];
            int alive = 0;
            int di = 0;

            for (int i = 0; i < nfiles; ++i) {
                int r = 0;

                got[i] = 0;
                if (!src[i].eof) {
                    r = paste_next(&src[i], cols[i]);
                    if (r < 0) {
                        print_status("paste", -5);
                        goto paste_done;
                    }
                    if (r > 0) {
                        got[i] = 1;
                        alive = 1;
                    }
                }
            }
            if (!alive) {
                break;
            }
            for (int i = 0; i < nfiles; ++i) {
                if (i > 0) {
                    write(1, &delims[di % ndelims], 1);
                    di = (di + 1) % (int)ndelims;
                }
                if (got[i]) {
                    puts(cols[i]);
                }
            }
            puts("\n");
        }
    paste_done:;
    }
    for (int i = 0; i < nfiles; ++i) {
        if (src[i].fd >= 0) {
            close(src[i].fd);
        }
    }
}

static void uniq_emit(int count, int dup_only, int uniq_only, u64 run, const char *line)
{
    if ((dup_only && run < 2) || (uniq_only && run != 1)) {
        return;
    }
    if (count) {
        put_u64(run);
        puts(" ");
    }
    puts(line);
    puts("\n");
}

static void cmd_uniq(char *rest)
{
    char *tok = next_arg(&rest);
    char *file = tok;
    int count = 0, dup_only = 0, uniq_only = 0;
    char path[PATH_MAX];
    char buf[IO_BUF];
    char prev[LINE_MAX];
    char cur[LINE_MAX];
    size_t clen = 0;
    u64 run = 0;
    int fd;
    ssize_t r;

    if (tok && tok[0] == '-' && tok[1] != '\0') {
        for (const char *f = tok + 1; *f; ++f) {
            if (*f == 'c') {
                count = 1;
            } else if (*f == 'd') {
                dup_only = 1;
            } else if (*f == 'u') {
                uniq_only = 1;
            } else {
                print_status("uniq", -E_INVAL);
                return;
            }
        }
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
                            uniq_emit(count, dup_only, uniq_only, run, prev);
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
                    uniq_emit(count, dup_only, uniq_only, run, prev);
                }
                memcpy(prev, cur, clen + 1);
                run = 1;
                have = 1;
            } else {
                ++run;
            }
        }
        if (have) {
            uniq_emit(count, dup_only, uniq_only, run, prev);
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

static void cmd_cd(char *rest)
{
    char *arg = next_arg(&rest);
    char path[PATH_MAX];
    char home[PATH_MAX];
    struct stat_info st;
    static char prev[PATH_MAX] = "/";
    char here[PATH_MAX];

    if (arg && next_arg(&rest)) {
        print_status("cd", -E_INVAL);
        return;
    }
    if (!arg || strcmp(arg, "~") == 0) {
        /* Bare `cd` goes to our home from /etc/passwd, `/` when unknown. */
        if (passwd_home((u32)getuid(), home, sizeof(home)) != 0 || home[0] == '\0') {
            memcpy(home, "/", 2);
        }
        if (stat(home, &st) != 0) {
            print_status("cd", -E_NOENT);
            return;
        }
        if (st.type != 2) {
            print_status("cd", -E_NOTDIR);
            return;
        }
        memcpy(prev, cwd, strlen(cwd) + 1);
        memcpy(cwd, home, strlen(home) + 1);
        return;
    }
    if (strcmp(arg, "-") == 0) {
        /* Back to the previous directory, printing it like bash. */
        if (stat(prev, &st) != 0) {
            print_status("cd", -E_NOENT);
            return;
        }
        if (st.type != 2) {
            print_status("cd", -E_NOTDIR);
            return;
        }
        memcpy(here, cwd, strlen(cwd) + 1);
        memcpy(cwd, prev, strlen(prev) + 1);
        memcpy(prev, here, strlen(here) + 1);
        puts(cwd);
        puts("\n");
        return;
    }
    if (arg[0] == '~' && (arg[1] == '\0' || arg[1] == '/')) {
        /* ~/... counts from home; the rest resolves normally. */
        char tmp[PATH_MAX];
        size_t h, n, t;

        if (passwd_home((u32)getuid(), home, sizeof(home)) != 0 || home[0] == '\0') {
            print_status("cd", -E_NOENT);
            return;
        }
        h = strlen(home);
        n = strlen(arg + 1);
        if (h + n >= sizeof(tmp)) {
            n = sizeof(tmp) - h - 1;
        }
        memcpy(tmp, home, h);
        t = h;
        memcpy(tmp + t, arg + 1, n);
        t += n;
        tmp[t] = '\0';
        resolve(tmp, path);
    } else {
        resolve(arg, path);
    }
    if (stat(path, &st) != 0) {
        print_status("cd", -E_NOENT);
        return;
    }
    if (st.type != 2) {
        print_status("cd", -E_NOTDIR);
        return;
    }
    memcpy(prev, cwd, strlen(cwd) + 1);
    memcpy(cwd, path, strlen(path) + 1);
}

/* Directory stack for pushd/popd/dirs (shell memory only). */
static char dirstack[8][PATH_MAX];
static int dstop;

static void cmd_pushd(char *rest)
{
    char *arg = next_arg(&rest);

    if (arg && next_arg(&rest)) {
        print_status("pushd", -E_INVAL);
        return;
    }
    if (!arg) {
        /* No argument: swap the top two, like bash. */
        if (dstop < 1) {
            print_status("pushd", -E_INVAL);
            return;
        }
        {
            char tmp[PATH_MAX];

            memcpy(tmp, cwd, strlen(cwd) + 1);
            memcpy(cwd, dirstack[dstop - 1], strlen(dirstack[dstop - 1]) + 1);
            memcpy(dirstack[dstop - 1], tmp, strlen(tmp) + 1);
        }
        puts(cwd);
        puts("\n");
        return;
    }
    if (dstop >= 8) {
        print_status("pushd", -E_INVAL);
        return;
    }
    {
        char path[PATH_MAX];
        struct stat_info st;

        resolve(arg, path);
        if (stat(path, &st) != 0 || st.type != 2) {
            print_status("pushd", -E_NOENT);
            return;
        }
        memcpy(dirstack[dstop], cwd, strlen(cwd) + 1);
        ++dstop;
        memcpy(cwd, path, strlen(path) + 1);
    }
    puts(cwd);
    puts("\n");
}

static void cmd_popd(char *rest)
{
    char *arg = next_arg(&rest);

    if (arg || dstop < 1) {
        print_status("popd", -E_INVAL);
        return;
    }
    --dstop;
    {
        struct stat_info st;

        if (stat(dirstack[dstop], &st) != 0 || st.type != 2) {
            print_status("popd", -E_NOENT);
            return;
        }
        memcpy(cwd, dirstack[dstop], strlen(dirstack[dstop]) + 1);
    }
    puts(cwd);
    puts("\n");
}

static void cmd_dirs(char *rest)
{
    char *arg = next_arg(&rest);
    int verbose = 0;

    if (arg && strcmp(arg, "-v") == 0) {
        verbose = 1;
        arg = next_arg(&rest);
    }
    if (arg) {
        print_status("dirs", -E_INVAL);
        return;
    }
    puts(cwd);
    puts("\n");
    for (int i = dstop - 1; i >= 0; --i) {
        if (verbose) {
            put_u64((u64)(dstop - 1 - i));
            puts("  ");
        }
        puts(dirstack[i]);
        puts("\n");
    }
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
    int reverse = 0, numeric = 0, unique = 0, check = 0;
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
                else if (*f == 'c') check = 1;
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
    if (check) {
        /* -c: silent when sorted; otherwise the first disorder, like GNU. */
        for (int k = 1; k < n; ++k) {
            int a = reverse ? n - k : k;
            int b = reverse ? n - k + 1 : k - 1;
            int bad = reverse ? sort_less(line_ptr[b], line_ptr[a], numeric)
                              : sort_less(line_ptr[a], line_ptr[b], numeric);

            if (unique && strcmp(line_ptr[a], line_ptr[b]) == 0) {
                continue;
            }
            if (bad) {
                eputs("sort: disorder at line ");
                put_u64_e((u64)(a + 1));
                eputs("\n");
                return;
            }
        }
        return;
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

static void seq_print_wide(long v, int width);

static void cmd_seq(char *rest)
{
    char *a1 = next_arg(&rest);
    int wide = 0;
    char *a2;
    char *a3;
    long first = 1, step = 1, last = 0;
    int ok = 1;
    long v;

    while (a1 && strcmp(a1, "-w") == 0) {
        wide = 1;
        a1 = next_arg(&rest);
    }
    a2 = next_arg(&rest);
    a3 = next_arg(&rest);
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
    /* -w: every value shares the width of the widest endpoint (sign counts),
     * padded with zeros. */
    {
        int width = 0;

        if (wide) {
            long ends[2] = {first, last};

            for (int i = 0; i < 2; ++i) {
                u64 m = ends[i] < 0 ? (u64)(-(ends[i] + 1)) + 1u : (u64)ends[i];
                int d = ends[i] < 0 ? 1 : 0;

                do {
                    ++d;
                    m /= 10;
                } while (m != 0);
                if (d > width) {
                    width = d;
                }
            }
        }
        if (step > 0) {
            for (v = first; v <= last;) {
                if (wide) {
                    seq_print_wide(v, width);
                } else {
                    put_int(v);
                }
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
                if (wide) {
                    seq_print_wide(v, width);
                } else {
                    put_int(v);
                }
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
}

/* One value for `seq -w`: sign, then the magnitude zero-padded so the whole
 * field (sign included) is `width` wide. */
static void seq_print_wide(long v, int width)
{
    if (v < 0) {
        u64 m = (u64)(-(v + 1)) + 1u;
        int d = 0;
        u64 t = m;

        puts("-");
        do {
            ++d;
            t /= 10;
        } while (t != 0);
        while (d < width - 1) {
            puts("0");
            ++d;
        }
        put_u64(m);
    } else {
        put_dec_pad((u64)v, width);
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
#define PW_LINE 128

/* One '\n'-terminated line without the newline; over-long lines are
 * truncated and the remainder skipped. Returns length, 0 on EOF, <0 on
 * read error. */
static int read_line(int fd, char *line, size_t cap)
{
    size_t n = 0;
    int skip = 0;

    for (;;) {
        char c;
        ssize_t r = read(fd, &c, 1);

        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return (int)n;
        }
        if (c == '\n') {
            return (int)n;
        }
        if (!skip) {
            if (n + 1 < cap) {
                line[n++] = c;
            } else {
                skip = 1;
            }
        }
    }
}

/* Split "name:passwd:uid:gid:..." (NUL-terminated, mutated in place):
 * name start, uid/gid parsed. ok=0 unless both numeric fields parse. */
static char *split_pw(char *line, u32 *uid, u32 *gid, int *ok)
{
    char *name = line;
    char *p = line;

    *ok = 0;
    while (*p && *p != ':') {
        ++p;
    }
    if (*p == '\0') {
        return name;
    }
    *p++ = '\0';
    while (*p && *p != ':') {
        ++p;
    }
    if (*p == '\0') {
        return name;
    }
    *p++ = '\0';
    *uid = 0;
    {
        int digits = 0;

        while (*p >= '0' && *p <= '9') {
            *uid = *uid * 10 + (u32)(*p - '0');
            ++p;
            ++digits;
        }
        if (digits == 0) {
            return name;
        }
    }
    if (*p != ':') {
        return name;
    }
    ++p;
    *gid = 0;
    {
        int digits = 0;

        while (*p >= '0' && *p <= '9') {
            *gid = *gid * 10 + (u32)(*p - '0');
            ++p;
            ++digits;
        }
        if (digits == 0) {
            return name;
        }
    }
    *ok = 1;
    return name;
}

/* name -> uid from /etc/passwd. 0 found, -1 not. */
static int passwd_uid(const char *name, u32 *uid)
{
    int fd = open("/etc/passwd", 0);
    char line[PW_LINE];
    int found = -1;

    if (fd < 0) {
        return -1;
    }
    for (;;) {
        int r = read_line(fd, line, sizeof(line));
        u32 u = 0, g = 0;
        int ok = 0;
        char *nm;

        if (r < 0) {
            found = -1;
            break;
        }
        if (r == 0) {
            break;
        }
        nm = split_pw(line, &u, &g, &ok);
        (void)g;
        if (ok && strcmp(nm, name) == 0) {
            *uid = u;
            found = 0;
            break;
        }
    }
    close(fd);
    return found;
}

/* uid -> login name. 0 found (NUL-terminated in out), -1 not. */
static int passwd_name(u32 uid, char *out, size_t cap)
{
    int fd = open("/etc/passwd", 0);
    char line[PW_LINE];
    int found = -1;

    if (fd < 0 || cap == 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    for (;;) {
        int r = read_line(fd, line, sizeof(line));
        u32 u = 0, g = 0;
        int ok = 0;
        char *nm;

        if (r < 0) {
            found = -1;
            break;
        }
        if (r == 0) {
            break;
        }
        nm = split_pw(line, &u, &g, &ok);
        (void)g;
        if (ok && u == uid) {
            size_t k = strlen(nm);

            if (k >= cap) {
                k = cap - 1;
            }
            memcpy(out, nm, k);
            out[k] = '\0';
            found = 0;
            break;
        }
    }
    close(fd);
    return found;
}

/* Home directory (6th field) for uid from /etc/passwd
 * (name:passwd:uid:gid:gecos:home:shell). 0 found (empty when the field is
 * missing), -1 when the uid is not listed. */
static int passwd_home(u32 uid, char *out, size_t cap)
{
    int fd = open("/etc/passwd", 0);
    char line[PW_LINE];
    int found = -1;

    if (fd < 0 || cap == 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    for (;;) {
        int r = read_line(fd, line, sizeof(line));
        char *p = line;
        u32 u = 0;
        int digits = 0;

        if (r < 0) {
            found = -1;
            break;
        }
        if (r == 0) {
            break;
        }
        while (*p && *p != ':') { /* name */
            ++p;
        }
        if (*p == '\0') {
            continue;
        }
        ++p;
        while (*p && *p != ':') { /* passwd */
            ++p;
        }
        if (*p == '\0') {
            continue;
        }
        ++p;
        while (*p >= '0' && *p <= '9') { /* uid */
            u = u * 10 + (u32)(*p - '0');
            ++p;
            ++digits;
        }
        if (digits == 0 || *p != ':') {
            continue;
        }
        ++p;
        while (*p >= '0' && *p <= '9') { /* gid */
            ++p;
        }
        if (*p != ':') {
            continue;
        }
        ++p;
        while (*p && *p != ':') { /* gecos */
            ++p;
        }
        if (*p == '\0') {
            continue;
        }
        ++p; /* home runs to the next ':' or end of line */
        if (u == uid) {
            size_t k = 0;

            while (p[k] && p[k] != ':') {
                ++k;
            }
            if (k >= cap) {
                k = cap - 1;
            }
            memcpy(out, p, k);
            out[k] = '\0';
            found = 0;
            break;
        }
    }
    close(fd);
    return found;
}

/* ---- SHA-256 (FIPS 180-4) for /etc/shadow password hashes ---- */

static const u32 sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static u32 sha_rotr(u32 x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(u32 h[8], const u8 *p)
{
    u32 w[64];
    u32 a, b, c, d, e, f, g, hh;

    for (int i = 0; i < 16; ++i) {
        w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) |
               ((u32)p[4 * i + 2] << 8) | (u32)p[4 * i + 3];
    }
    for (int i = 16; i < 64; ++i) {
        u32 s0 = sha_rotr(w[i - 15], 7) ^ sha_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = sha_rotr(w[i - 2], 17) ^ sha_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0];
    b = h[1];
    c = h[2];
    d = h[3];
    e = h[4];
    f = h[5];
    g = h[6];
    hh = h[7];
    for (int i = 0; i < 64; ++i) {
        u32 s1 = sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = hh + s1 + ch + sha256_k[i] + w[i];
        u32 s0 = sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = s0 + maj;

        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

/* out[32] = SHA-256(salt[0..salt_len] + pw[0..pw_len]). */
static void sha256_salted(const u8 *salt, size_t salt_len, const char *pw, size_t pw_len,
                          u8 out[32])
{
    static const u32 init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    u32 h[8];
    u8 block[64];
    size_t bfill = 0;
    u64 total = (u64)salt_len + (u64)pw_len;

    for (int i = 0; i < 8; ++i) {
        h[i] = init[i];
    }
    /* Feed helper: buffer into 64-byte blocks. */
    for (size_t i = 0; i < salt_len; ++i) {
        block[bfill++] = salt[i];
        if (bfill == 64) {
            sha256_block(h, block);
            bfill = 0;
        }
    }
    for (size_t i = 0; i < pw_len; ++i) {
        block[bfill++] = (u8)pw[i];
        if (bfill == 64) {
            sha256_block(h, block);
            bfill = 0;
        }
    }
    block[bfill++] = 0x80;
    if (bfill > 56) {
        while (bfill < 64) {
            block[bfill++] = 0;
        }
        sha256_block(h, block);
        bfill = 0;
    }
    while (bfill < 56) {
        block[bfill++] = 0;
    }
    {
        u64 bits = total * 8;
        int i;

        for (i = 0; i < 8; ++i) {
            block[56 + i] = (u8)(bits >> (56 - 8 * i));
        }
    }
    sha256_block(h, block);
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = (u8)(h[i] >> 24);
        out[4 * i + 1] = (u8)(h[i] >> 16);
        out[4 * i + 2] = (u8)(h[i] >> 8);
        out[4 * i + 3] = (u8)h[i];
    }
}

static void bin2hex(const u8 *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < n; ++i) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xf];
    }
    out[2 * n] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* ---- /etc/shadow: `name:salthex$hashhex` or locked `name:!` ---- */

#define SHADOW_LINE 192
#define SHADOW_MAX 8192

/* 0 = password set (salt16/hash64 filled), 1 = locked, -1 = missing/unreadable.
 * A malformed line for the name fails closed (locked). */
static int shadow_lookup(const char *name, char *salt16, char *hash64)
{
    int fd = open("/etc/shadow", 0);
    char line[SHADOW_LINE];
    int found = -1;

    if (fd < 0) {
        return -1;
    }
    for (;;) {
        int r = read_line(fd, line, sizeof(line));
        size_t nl = strlen(name);
        size_t i;

        if (r < 0) {
            found = -1;
            break;
        }
        if (r == 0) {
            break;
        }
        {
            size_t k = 0;

            while (k < nl && line[k] == name[k]) {
                ++k;
            }
            if (k != nl || line[nl] != ':') {
                continue;
            }
        }
        /* Our name: decide locked vs hash. */
        if (line[nl + 1] == '!' && line[nl + 2] == '\0') {
            found = 1;
            break;
        }
        found = 1; /* malformed unless the full salt$hash checks out */
        for (i = 0; i < 16; ++i) {
            if (hexval(line[nl + 1 + i]) < 0) {
                break;
            }
        }
        if (i != 16 || line[nl + 17] != '$') {
            break;
        }
        for (i = 0; i < 64; ++i) {
            if (hexval(line[nl + 18 + i]) < 0) {
                break;
            }
        }
        if (i != 64 || line[nl + 82] != '\0') {
            break;
        }
        memcpy(salt16, line + nl + 1, 16);
        salt16[16] = '\0';
        memcpy(hash64, line + nl + 18, 64);
        hash64[64] = '\0';
        found = 0;
        break;
    }
    close(fd);
    return found;
}

static ssize_t write_all(int fd, const char *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t w = write(fd, buf + done, len - done);

        if (w <= 0) {
            return w == 0 ? (ssize_t)done : w;
        }
        done += (size_t)w;
    }
    return (ssize_t)done;
}

/* Set (salt+hash) or lock (both NULL) the entry for name, adding it when
 * missing. The file stays 0600 root. */
static int shadow_set(const char *name, const char *salt16, const char *hash64)
{
    static char oldb[SHADOW_MAX];
    static char newb[SHADOW_MAX];
    int fd = open("/etc/shadow", 0);
    ssize_t total = 0;
    size_t nlen = 0;
    int r;

    if (fd >= 0) {
        for (;;) {
            ssize_t k = read(fd, oldb + total, sizeof(oldb) - (size_t)total);

            if (k < 0) {
                r = (int)k;
                close(fd);
                return r;
            }
            if (k == 0) {
                break;
            }
            total += k;
            if (total >= (ssize_t)sizeof(oldb)) {
                /* Room left? Peek one byte: a full buffer means too big. */
                char probe;

                if (read(fd, &probe, 1) > 0) {
                    close(fd);
                    return -E_NOSPC;
                }
                break;
            }
        }
        close(fd);
    }
    /* Rebuild, swapping in our entry. */
    {
        size_t i = 0;
        size_t nl = strlen(name);

        while (i < (size_t)total) {
            size_t j = i;
            int ours = 0;

            while (j < (size_t)total && oldb[j] != '\n') {
                ++j;
            }
            /* Line is oldb[i..j], newline at j (or EOF). */
            if (j - i > nl && memcmp(oldb + i, name, nl) == 0 && oldb[i + nl] == ':') {
                ours = 1;
            }
            if (ours) {
                /* Drop it: the fresh entry is appended below. */
            } else {
                size_t k = j < (size_t)total ? j - i + 1 : j - i; /* keep the newline */

                if (nlen + k >= sizeof(newb)) {
                    return -E_NOSPC;
                }
                memcpy(newb + nlen, oldb + i, k);
                nlen += k;
            }
            i = j < (size_t)total ? j + 1 : j;
        }
    }
    {
        char entry[128];
        size_t k = strlen(name);

        if (k + 84 >= sizeof(entry)) {
            return -E_INVAL;
        }
        memcpy(entry, name, k);
        entry[k++] = ':';
        if (!salt16 || !hash64) {
            entry[k++] = '!';
        } else {
            memcpy(entry + k, salt16, 16);
            k += 16;
            entry[k++] = '$';
            memcpy(entry + k, hash64, 64);
            k += 64;
        }
        entry[k++] = '\n';
        if (nlen + k >= sizeof(newb)) {
            return -E_NOSPC;
        }
        memcpy(newb + nlen, entry, k);
        nlen += k;
    }
    fd = open("/etc/shadow", O_WRONLY | O_TRUNC);
    if (fd < 0) {
        fd = open("/etc/shadow", O_WRONLY | O_CREAT);
        if (fd < 0) {
            return fd;
        }
    }
    r = (int)write_all(fd, newb, nlen);
    close(fd);
    if (r < 0) {
        return r;
    }
    if ((size_t)r != nlen) {
        return -E_NOSPC;
    }
    r = chmod("/etc/shadow", 0600);
    if (r < 0) {
        return r;
    }
    return 0;
}

/* Append one line (with '\n') to a root-owned config file. */
static int append_line(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    size_t len = strlen(text);
    int r;

    if (fd < 0) {
        return fd;
    }
    r = (int)write_all(fd, text, len);
    close(fd);
    if (r < 0) {
        return r;
    }
    if ((size_t)r != len) {
        return -E_NOSPC;
    }
    return 0;
}

#define SECRET_MAX 256

/* One hidden line from the console (no echo, backspace still works);
 * length without '\n', -1 on EOF/error, -2 when it does not fit. */
static int read_secret(char *buf, size_t cap)
{
    size_t total = 0;

    console_echo(0);
    for (;;) {
        ssize_t n = read(0, buf + total, cap - 1 - total);

        if (n <= 0) {
            console_echo(1);
            return total > 0 ? (int)total : -1;
        }
        total += (size_t)n;
        if (buf[total - 1] == '\n' || total >= cap - 1) {
            break;
        }
    }
    console_echo(1);
    if (buf[total - 1] != '\n') {
        /* Ctrl-D ended the line early (the kernel hands out everything
         * pending, so nothing is left behind): not a complete secret. */
        memset(buf, 0, cap);
        return -2;
    }
    buf[total - 1] = '\0';
    return (int)(total - 1);
}

/* Split "name:x:gid:m1,m2" (mutated): name start, gid parsed, members after
 * the third colon (maybe empty). ok=0 unless the gid parses. */
static char *split_group(char *line, u32 *gid, char **members, int *ok)
{
    char *name = line;
    char *p = line;

    *ok = 0;
    *members = "";
    while (*p && *p != ':') {
        ++p;
    }
    if (*p == '\0') {
        return name;
    }
    *p++ = '\0';
    while (*p && *p != ':') {
        ++p;
    }
    if (*p == '\0') {
        return name;
    }
    *p++ = '\0';
    *gid = 0;
    {
        int digits = 0;

        while (*p >= '0' && *p <= '9') {
            *gid = *gid * 10 + (u32)(*p - '0');
            ++p;
            ++digits;
        }
        if (digits == 0) {
            return name;
        }
    }
    if (*p == ':') {
        *members = p + 1;
    } else if (*p != '\0') {
        return name;
    }
    *ok = 1;
    return name;
}

/* group name -> gid from /etc/group. 0 found, -1 not. */
static int group_gid(const char *name, u32 *gid)
{
    int fd = open("/etc/group", 0);
    char line[PW_LINE];
    int found = -1;

    if (fd < 0) {
        return -1;
    }
    for (;;) {
        int r = read_line(fd, line, sizeof(line));
        u32 g = 0;
        char *members;
        int ok = 0;
        char *nm;

        if (r < 0) {
            found = -1;
            break;
        }
        if (r == 0) {
            break;
        }
        nm = split_group(line, &g, &members, &ok);
        (void)members;
        if (ok && strcmp(nm, name) == 0) {
            *gid = g;
            found = 0;
            break;
        }
    }
    close(fd);
    return found;
}

/* gid -> group name. 0 found, -1 not. */
static int group_name(u32 gid, char *out, size_t cap)
{
    int fd = open("/etc/group", 0);
    char line[PW_LINE];
    int found = -1;

    if (fd < 0 || cap == 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    for (;;) {
        int r = read_line(fd, line, sizeof(line));
        u32 g = 0;
        char *members;
        int ok = 0;
        char *nm;

        if (r < 0) {
            found = -1;
            break;
        }
        if (r == 0) {
            break;
        }
        nm = split_group(line, &g, &members, &ok);
        (void)members;
        if (ok && g == gid) {
            size_t k = strlen(nm);

            if (k >= cap) {
                k = cap - 1;
            }
            memcpy(out, nm, k);
            out[k] = '\0';
            found = 0;
            break;
        }
    }
    close(fd);
    return found;
}

/* Ask "overwrite PATH?" on stderr, read one line. True on y/Y. EOF or
 * anything else means no (safe default for scripts). */
static int confirm(const char *path)
{
    char buf[16];
    ssize_t n;

    eputs(T("overwrite"));
    eputs(path);
    eputs("? ");
    n = read(0, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return 0;
    }
    return buf[0] == 'y' || buf[0] == 'Y';
}

static void cmd_whoami(void)
{
    char name[32];
    u32 me = (u32)getuid();

    if (passwd_name(me, name, sizeof(name)) == 0) {
        puts(name);
    } else {
        put_u64(me);
    }
    puts("\n");
}

/* id [-u|-g|-G] [-n] [USER]: identity. Last of -u/-g/-G wins; -n prints
 * names. No -Z (no SELinux), no -r (no audit ids). */
static void cmd_id(char *rest)
{
    char *arg;
    int which = 0; /* 0 both, 1 -u, 2 -g, 3 -G */
    int names = 0;
    char *user = 0;
    u32 uid, gid;
    char username[32];

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (arg[0] == '-' && arg[1] != '\0') {
            for (const char *f = arg + 1; *f; ++f) {
                if (*f == 'u') {
                    which = 1;
                } else if (*f == 'g') {
                    which = 2;
                } else if (*f == 'G') {
                    which = 3;
                } else if (*f == 'n') {
                    names = 1;
                } else {
                    print_status("id", -E_INVAL);
                    return;
                }
            }
        } else if (!user) {
            user = arg;
        } else {
            print_status("id", -E_INVAL);
            return;
        }
    }
    if (!user) {
        uid = (u32)getuid();
        gid = (u32)getgid();
        if (names && passwd_scan(uid, 0, &uid, &gid, username, sizeof(username)) != 0) {
            print_status("id", -E_INVAL);
            return;
        }
    } else if (passwd_resolve(user, &uid, &gid, username, sizeof(username)) != 0) {
        print_status("id", -E_INVAL);
        return;
    }
    if (which == 0) {
        puts("uid=");
        if (names) {
            if (passwd_name(uid, username, sizeof(username)) != 0) {
                print_status("id", -E_INVAL);
                return;
            }
            puts(username);
        } else {
            put_u64(uid);
        }
        puts(" gid=");
        if (names) {
            char gname[32];

            if (group_name(gid, gname, sizeof(gname)) != 0) {
                print_status("id", -E_INVAL);
                return;
            }
            puts(gname);
        } else {
            put_u64(gid);
        }
        puts("\n");
        return;
    }
    if (which == 1) {
        if (names) {
            if (!user && passwd_scan(uid, 0, &uid, &gid, username, sizeof(username)) != 0) {
                print_status("id", -E_INVAL);
                return;
            }
            puts(username);
        } else {
            put_u64(uid);
        }
        puts("\n");
        return;
    }
    if (which == 2) {
        if (names) {
            char gname[32];

            if (group_name(gid, gname, sizeof(gname)) != 0) {
                print_status("id", -E_INVAL);
                return;
            }
            puts(gname);
        } else {
            put_u64(gid);
        }
        puts("\n");
        return;
    }
    /* -G: primary, then every extra group the login belongs to. */
    if (names) {
        char gname[32];

        if (group_name(gid, gname, sizeof(gname)) != 0) {
            print_status("id", -E_INVAL);
            return;
        }
        puts(gname);
    } else {
        put_u64(gid);
    }
    {
        int fd = open("/etc/group", 0);

        if (fd >= 0) {
            char line[PW_LINE];

            for (;;) {
                int r = read_line(fd, line, sizeof(line));
                u32 g = 0;
                char *members;
                int ok = 0;
                char *nm;

                if (r <= 0) {
                    break;
                }
                nm = split_group(line, &g, &members, &ok);
                if (!ok || g == gid) {
                    continue;
                }
                for (char *m = members;;) {
                    char *comma = m;
                    size_t ulen = strlen(username);

                    while (*comma && *comma != ',') {
                        ++comma;
                    }
                    if ((size_t)(comma - m) == ulen && memcmp(m, username, ulen) == 0) {
                        if (names) {
                            puts(" ");
                            puts(nm);
                        } else {
                            puts(" ");
                            put_u64(g);
                        }
                        break;
                    }
                    if (*comma == '\0') {
                        break;
                    }
                    m = comma + 1;
                }
            }
            close(fd);
        }
    }
    puts("\n");
}

/* Scan /etc/passwd by uid or login name. Fills uid/gid/name. */
static int passwd_scan(u32 want_uid, const char *want_name, u32 *uid, u32 *gid, char *name,
                       size_t cap)
{
    int fd = open("/etc/passwd", 0);
    char line[PW_LINE];
    int found = -1;

    if (fd < 0 || cap == 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    for (;;) {
        int r = read_line(fd, line, sizeof(line));
        u32 u = 0, g = 0;
        int ok = 0;
        char *nm;

        if (r < 0) {
            found = -1;
            break;
        }
        if (r == 0) {
            break;
        }
        nm = split_pw(line, &u, &g, &ok);
        if (!ok) {
            continue;
        }
        if ((want_name ? strcmp(nm, want_name) == 0 : u == want_uid)) {
            size_t k = strlen(nm);

            *uid = u;
            *gid = g;
            if (k >= cap) {
                k = cap - 1;
            }
            memcpy(name, nm, k);
            name[k] = '\0';
            found = 0;
            break;
        }
    }
    close(fd);
    return found;
}

/* Resolve a user designator (login name or decimal uid). */
static int passwd_resolve(const char *arg, u32 *uid, u32 *gid, char *name, size_t cap)
{
    const char *a = arg;
    u32 v = 0;

    if (parse_num(&a, 10, &v) == 0) {
        return passwd_scan(v, 0, uid, gid, name, cap);
    }
    return passwd_scan(0, arg, uid, gid, name, cap);
}

static void cmd_groups(char *rest)
{
    char *arg = next_arg(&rest);
    char username[32];
    u32 uid, gid;
    char gname[32];
    int fd;

    if (arg && next_arg(&rest)) {
        print_status("groups", -E_INVAL);
        return;
    }
    if (!arg) {
        uid = (u32)getuid();
        if (passwd_scan(uid, 0, &uid, &gid, username, sizeof(username)) != 0) {
            put_u64(uid);
            puts("\n");
            return;
        }
    } else if (passwd_resolve(arg, &uid, &gid, username, sizeof(username)) != 0) {
        print_status("groups", -E_INVAL);
        return;
    }
    puts(username);
    puts(" :");
    if (group_name(gid, gname, sizeof(gname)) == 0) {
        puts(" ");
        puts(gname);
    } else {
        puts(" ");
        put_u64(gid);
    }
    fd = open("/etc/group", 0);
    if (fd >= 0) {
        char line[PW_LINE];

        for (;;) {
            int r = read_line(fd, line, sizeof(line));
            u32 g = 0;
            char *members;
            int ok = 0;
            char *nm;

            if (r <= 0) {
                break;
            }
            nm = split_group(line, &g, &members, &ok);
            if (!ok || g == gid) {
                continue; /* primary already shown (or malformed) */
            }
            for (char *m = members;;) {
                char *comma = m;
                size_t ulen = strlen(username);

                while (*comma && *comma != ',') {
                    ++comma;
                }
                if ((size_t)(comma - m) == ulen && memcmp(m, username, ulen) == 0) {
                    puts(" ");
                    puts(nm);
                    break;
                }
                if (*comma == '\0') {
                    break;
                }
                m = comma + 1;
            }
        }
        close(fd);
    }
    puts("\n");
}

/* ---- local users: useradd/passwd/login over /etc/passwd + /etc/shadow ----
 * Root administers (only root can write the 0600 shadow); `login` switches
 * identity after verifying the salted SHA-256 password. Self-service
 * passwords need setuid executables, which the kernel does not have yet. */

static void lang_load(void)
{
    int fd = open("/etc/lang", 0);
    char buf[8];
    ssize_t n;

    lang_es = 0;
    if (fd < 0) {
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n >= 2 && buf[0] == 'e' && buf[1] == 's') {
        lang_es = 1;
    }
}

/* lang [en|es]: show the interface language, or set it (root only,
 * stored in /etc/lang, applied at once). */
static void cmd_lang(char *rest)
{
    char *arg = next_arg(&rest);
    int want_es;
    int fd;
    ssize_t w;
    const char *text;

    if (arg && next_arg(&rest)) {
        print_status("lang", -E_INVAL);
        return;
    }
    if (!arg) {
        puts(lang_es ? "es\n" : "en\n");
        return;
    }
    if (strcmp(arg, "en") == 0) {
        want_es = 0;
    } else if (strcmp(arg, "es") == 0) {
        want_es = 1;
    } else {
        print_status("lang", -E_INVAL);
        return;
    }
    if (getuid() != 0) {
        print_status("lang", -E_PERM);
        return;
    }
    fd = open("/etc/lang", O_WRONLY | O_TRUNC);
    if (fd < 0) {
        fd = open("/etc/lang", O_WRONLY | O_CREAT);
        if (fd < 0) {
            print_status("lang", fd);
            return;
        }
    }
    text = want_es ? "es\n" : "en\n";
    w = write_all(fd, text, 3);
    close(fd);
    if (w != 3) {
        print_status("lang", w < 0 ? (int)w : -E_NOSPC);
        return;
    }
    if (chmod("/etc/lang", 0644) < 0) {
        print_status("lang", -E_ACCES);
        return;
    }
    lang_es = want_es;
    puts(text);
}

static int valid_username(const char *s)
{
    size_t n = strlen(s);
    size_t i;

    if (n == 0 || n > 32) {
        return 0;
    }
    if (!((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) {
        return 0;
    }
    for (i = 1; i < n; ++i) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

/* Highest uid in /etc/passwd, or -1 when unreadable/empty. */
static int passwd_max_uid(u32 *max_uid)
{
    int fd = open("/etc/passwd", 0);
    char line[PW_LINE];
    int any = 0;
    u32 mx = 0;

    if (fd < 0) {
        return -1;
    }
    for (;;) {
        int r = read_line(fd, line, sizeof(line));
        u32 u = 0, g = 0;
        int ok = 0;

        if (r < 0) {
            close(fd);
            return -1;
        }
        if (r == 0) {
            break;
        }
        (void)split_pw(line, &u, &g, &ok);
        if (!ok) {
            continue;
        }
        if (!any || u > mx) {
            mx = u;
            any = 1;
        }
    }
    close(fd);
    if (!any) {
        return -1;
    }
    *max_uid = mx;
    return 0;
}

static int append_u32(char *buf, size_t cap, size_t *pos, u32 v)
{
    char rev[10];
    int m = 0;

    if (v == 0) {
        rev[m++] = '0';
    } else {
        while (v > 0) {
            rev[m++] = (char)('0' + v % 10);
            v /= 10;
        }
    }
    if (*pos + (size_t)m >= cap) {
        return -1;
    }
    while (m > 0) {
        buf[(*pos)++] = rev[--m];
    }
    return 0;
}

static int append_bytes(char *buf, size_t cap, size_t *pos, const char *s, size_t n)
{
    if (*pos + n >= cap) {
        return -1;
    }
    memcpy(buf + *pos, s, n);
    *pos += n;
    return 0;
}

/* useradd NAME: per-user group, passwd entry, locked shadow entry, home. */
static void cmd_useradd(char *rest)
{
    char *arg = next_arg(&rest);
    u32 uid, gid, mx;
    char home[PATH_MAX];
    char entry[160];
    size_t pos = 0;
    struct stat_info st;
    int r;

    if (getuid() != 0) {
        print_status("useradd", -E_PERM);
        return;
    }
    if (!arg || next_arg(&rest)) {
        print_status("useradd", -E_INVAL);
        return;
    }
    if (!valid_username(arg)) {
        print_status("useradd", -E_INVAL);
        return;
    }
    if (passwd_uid(arg, &uid) == 0 || group_gid(arg, &gid) == 0) {
        print_status("useradd", -E_EXIST);
        return;
    }
    /* First free uid from 1000 up. */
    uid = 1000;
    if (passwd_max_uid(&mx) == 0 && mx >= uid) {
        uid = mx + 1;
    }
    while (uid < 60000) {
        char nm[33];

        if (passwd_scan(uid, 0, &uid, &gid, nm, sizeof(nm)) != 0) {
            break;
        }
        ++uid;
    }
    if (uid >= 60000) {
        print_status("useradd", -E_NOSPC);
        return;
    }
    gid = uid;
    {
        size_t hl = strlen(arg);

        if (6 + hl >= sizeof(home)) {
            print_status("useradd", -E_INVAL);
            return;
        }
        memcpy(home, "/home/", 6);
        memcpy(home + 6, arg, hl + 1);
    }
    if (stat(home, &st) == 0) {
        print_status("useradd", -E_EXIST);
        return;
    }
    /* passwd: name:x:uid:gid:name:/home/name:/sbin/init */
    if (append_bytes(entry, sizeof(entry), &pos, arg, strlen(arg)) != 0 ||
        append_bytes(entry, sizeof(entry), &pos, ":x:", 3) != 0 ||
        append_u32(entry, sizeof(entry), &pos, uid) != 0 ||
        append_bytes(entry, sizeof(entry), &pos, ":", 1) != 0 ||
        append_u32(entry, sizeof(entry), &pos, gid) != 0 ||
        append_bytes(entry, sizeof(entry), &pos, ":", 1) != 0 ||
        append_bytes(entry, sizeof(entry), &pos, arg, strlen(arg)) != 0 ||
        append_bytes(entry, sizeof(entry), &pos, ":", 1) != 0 ||
        append_bytes(entry, sizeof(entry), &pos, home, strlen(home)) != 0 ||
        append_bytes(entry, sizeof(entry), &pos, ":/sbin/init\n", 12) != 0) {
        print_status("useradd", -E_INVAL);
        return;
    }
    entry[pos] = '\0';
    r = append_line("/etc/passwd", entry);
    if (r < 0) {
        print_status("useradd", r);
        return;
    }
    /* group: name:x:gid: */
    pos = 0;
    if (append_bytes(entry, sizeof(entry), &pos, arg, strlen(arg)) != 0 ||
        append_bytes(entry, sizeof(entry), &pos, ":x:", 3) != 0 ||
        append_u32(entry, sizeof(entry), &pos, gid) != 0 ||
        append_bytes(entry, sizeof(entry), &pos, ":\n", 2) != 0) {
        print_status("useradd", -E_INVAL);
        return;
    }
    entry[pos] = '\0';
    r = append_line("/etc/group", entry);
    if (r < 0) {
        print_status("useradd", r);
        return;
    }
    /* Locked password until `passwd` sets one. */
    r = shadow_set(arg, 0, 0);
    if (r < 0) {
        print_status("useradd", r);
        return;
    }
    r = mkdir(home);
    if (r < 0) {
        print_status("useradd", r);
        return;
    }
    r = chown(home, uid, gid);
    if (r == 0) {
        r = chmod(home, 0755);
    }
    if (r < 0) {
        print_status("useradd", r);
    }
}

/* passwd [NAME]: root sets the login password (interactive, never echoed). */
static void cmd_passwd(char *rest)
{
    char *arg = next_arg(&rest);
    char target[33];
    u32 uid, gid;
    char pw1[SECRET_MAX], pw2[SECRET_MAX];
    u8 saltb[8];
    char salt16[17], hash64[65];
    u8 digest[32];
    int n1, n2, r;

    if (getuid() != 0) {
        print_status("passwd", -E_PERM);
        return;
    }
    if (arg && next_arg(&rest)) {
        print_status("passwd", -E_INVAL);
        return;
    }
    if (!arg) {
        if (passwd_name((u32)getuid(), target, sizeof(target)) != 0) {
            print_status("passwd", -E_NOENT);
            return;
        }
    } else if (passwd_resolve(arg, &uid, &gid, target, sizeof(target)) != 0) {
        print_status("passwd", -E_NOENT);
        return;
    }
    eputs(T("newpw"));
    n1 = read_secret(pw1, sizeof(pw1));
    if (n1 <= 0) {
        memset(pw1, 0, sizeof(pw1));
        print_status("passwd", n1 == -2 ? -E_INVAL : -E_ACCES);
        return;
    }
    eputs(T("retype"));
    n2 = read_secret(pw2, sizeof(pw2));
    if (n2 != n1 || memcmp(pw1, pw2, (size_t)n1) != 0) {
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        print_status("passwd", -E_INVAL);
        return;
    }
    memset(pw2, 0, sizeof(pw2));
    if (getrandom(saltb, sizeof(saltb)) != (long)sizeof(saltb)) {
        memset(pw1, 0, sizeof(pw1));
        print_status("passwd", -E_NODEV);
        return;
    }
    sha256_salted(saltb, sizeof(saltb), pw1, (size_t)n1, digest);
    memset(pw1, 0, sizeof(pw1));
    bin2hex(saltb, sizeof(saltb), salt16);
    bin2hex(digest, sizeof(digest), hash64);
    memset(digest, 0, sizeof(digest));
    memset(saltb, 0, sizeof(saltb));
    r = shadow_set(target, salt16, hash64);
    memset(salt16, 0, sizeof(salt16));
    memset(hash64, 0, sizeof(hash64));
    if (r < 0) {
        print_status("passwd", r);
        return;
    }
    puts(T("pwupdated"));
}

/* login [NAME]: verify the password and take that identity (root console). */
static void cmd_login(char *rest)
{
    char *arg = next_arg(&rest);
    char name[33];
    u32 uid, gid;
    char salt16[17], want64[65], got64[65];
    u8 saltb[8];
    u8 digest[32];
    char pw[SECRET_MAX];
    char home[PATH_MAX];
    struct stat_info st;
    int n, r;

    if (arg && next_arg(&rest)) {
        print_status("login", -E_INVAL);
        return;
    }
    if (getuid() != 0) {
        print_status("login", -E_PERM);
        return;
    }
    if (!arg) {
        char buf[64];
        char *p;
        ssize_t k;

        eputs(T("loginname"));
        k = read(0, buf, sizeof(buf) - 1);
        if (k <= 0) {
            return;
        }
        buf[k] = '\0';
        p = buf;
        arg = next_arg(&p);
        if (!arg || next_arg(&p)) {
            print_status("login", -E_INVAL);
            return;
        }
    }
    if (passwd_resolve(arg, &uid, &gid, name, sizeof(name)) != 0) {
        print_status("login", -E_ACCES);
        return;
    }
    eputs(T("password"));
    n = read_secret(pw, sizeof(pw));
    if (n < 0) {
        memset(pw, 0, sizeof(pw));
        if (n == -2) {
            print_status("login", -E_ACCES);
        }
        return;
    }
    if (shadow_lookup(name, salt16, want64) != 0) {
        memset(pw, 0, sizeof(pw));
        print_status("login", -E_ACCES);
        return;
    }
    for (int i = 0; i < 8; ++i) {
        int hi = hexval(salt16[2 * i]);
        int lo = hexval(salt16[2 * i + 1]);

        if (hi < 0 || lo < 0) { /* checked by lookup; fail closed anyway */
            memset(pw, 0, sizeof(pw));
            print_status("login", -E_ACCES);
            return;
        }
        saltb[i] = (u8)((hi << 4) | lo);
    }
    sha256_salted(saltb, sizeof(saltb), pw, (size_t)n, digest);
    memset(pw, 0, sizeof(pw));
    memset(saltb, 0, sizeof(saltb));
    bin2hex(digest, sizeof(digest), got64);
    memset(digest, 0, sizeof(digest));
    if (memcmp(got64, want64, 64) != 0) {
        memset(got64, 0, sizeof(got64));
        memset(want64, 0, sizeof(want64));
        print_status("login", -E_ACCES);
        return;
    }
    memset(got64, 0, sizeof(got64));
    memset(want64, 0, sizeof(want64));
    r = setgid(gid);
    if (r == 0) {
        r = setuid(uid);
    }
    if (r < 0) {
        print_status("login", r);
        return;
    }
    /* Land in their home when it exists; otherwise stay where we are. */
    if (passwd_home(uid, home, sizeof(home)) == 0 && home[0] != '\0' &&
        stat(home, &st) == 0 && st.type == 2) {
        memcpy(cwd, home, strlen(home) + 1);
    }
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

static void cmd_uname(char *rest)
{
    char *arg;
    int s = 0, n = 0, r = 0, m = 0, a = 0;

    for (arg = next_arg(&rest); arg; arg = next_arg(&rest)) {
        if (arg[0] != '-' || arg[1] == '\0') {
            print_status("uname", -E_INVAL);
            return;
        }
        for (const char *f = arg + 1; *f; ++f) {
            if (*f == 's') {
                s = 1;
            } else if (*f == 'n') {
                n = 1;
            } else if (*f == 'r') {
                r = 1;
            } else if (*f == 'm') {
                m = 1;
            } else if (*f == 'a') {
                a = 1;
            } else {
                print_status("uname", -E_INVAL);
                return;
            }
        }
    }
    if (!s && !n && !r && !m && !a) {
        puts("axysOS\n");
        return;
    }
    if (a) {
        cmd_cat_proc("uname", "/proc/version");
        return;
    }
    {
        int first = 1;

        /* Fixed order s n r m, like uname(1). -s: system name. -n:
         * hostname. -r: release (2nd word of /proc/version). -m: machine. */
        if (s) {
            puts("axysOS");
            first = 0;
        }
        if (n) {
            char buf[64];
            int fd = open("/etc/hostname", 0);
            ssize_t k, total = 0;

            if (!first) {
                puts(" ");
            }
            first = 0;
            if (fd >= 0) {
                while (total < (ssize_t)sizeof(buf) - 1 &&
                       (k = read(fd, buf + (size_t)total,
                                 sizeof(buf) - 1 - (size_t)total)) > 0) {
                    total += k;
                }
                close(fd);
            }
            if (total <= 0) {
                puts("axys");
            } else {
                buf[total] = '\0';
                if (total > 0 && buf[total - 1] == '\n') {
                    buf[total - 1] = '\0';
                }
                puts(buf);
            }
        }
        if (r) {
            char buf[64];
            int fd = open("/proc/version", 0);
            ssize_t k, total = 0;

            if (!first) {
                puts(" ");
            }
            first = 0;
            if (fd >= 0) {
                while (total < (ssize_t)sizeof(buf) - 1 &&
                       (k = read(fd, buf + (size_t)total,
                                 sizeof(buf) - 1 - (size_t)total)) > 0) {
                    total += k;
                }
                close(fd);
            }
            buf[total] = '\0';
            {
                /* Second whitespace-separated word ("axysOS 1.0 x86_64"). */
                char *p = buf;

                while (*p && *p != ' ' && *p != '\n') {
                    ++p;
                }
                while (*p == ' ') {
                    ++p;
                }
                {
                    char *e = p;

                    while (*e && *e != ' ' && *e != '\n') {
                        ++e;
                    }
                    if (e == p) {
                        puts("?");
                    } else {
                        char save = *e;

                        *e = '\0';
                        puts(p);
                        *e = save;
                    }
                }
            }
        }
        if (m) {
            if (!first) {
                puts(" ");
            }
            puts("x86_64");
        }
        puts("\n");
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

/* umask [MASK]: show or set the file mode creation mask (octal). */
static void cmd_umask(char *rest)
{
    char *arg = next_arg(&rest);
    u32 mask;
    int r;

    if (!arg) {
        /* No getter syscall: set 0, read the old value back, restore.
         * Single thread with per-process state, so nothing interleaves. */
        int cur = umask_set(0);

        if (cur < 0) {
            print_status("umask", cur);
            return;
        }
        umask_set((u32)cur);
        put_octal((u32)cur);
        puts("\n");
        return;
    }
    if (next_arg(&rest)) {
        print_status("umask", -E_INVAL);
        return;
    }
    {
        const char *a = arg;

        if (parse_num(&a, 8, &mask) != 0 || mask > 0777u) {
            print_status("umask", -E_INVAL);
            return;
        }
    }
    r = umask_set(mask);
    if (r < 0) {
        print_status("umask", r);
    }
}

/* logger [-t TAG] [MSG...]: append a line to the kernel log (see dmesg).
 * No MSG reads standard input. Over-long input is truncated to fit one
 * klog call. */
static void cmd_logger(char *rest)
{
    char *arg = next_arg(&rest);
    char tag[32];
    char msg[480];
    size_t len = 0;

    if (arg && strcmp(arg, "-t") == 0) {
        size_t k;

        arg = next_arg(&rest);
        if (!arg) {
            print_status("logger", -E_INVAL);
            return;
        }
        k = strlen(arg);
        if (k >= sizeof(tag)) {
            k = sizeof(tag) - 1;
        }
        memcpy(tag, arg, k);
        tag[k] = '\0';
        arg = next_arg(&rest);
    } else {
        if (passwd_name((u32)getuid(), tag, sizeof(tag)) != 0) {
            memcpy(tag, "user", 5);
        }
    }
    /* Nested function would be a GNU extension trap; inline the appender. */
    {
        size_t k = strlen(tag);

        if (len + k + 2 < sizeof(msg)) {
            memcpy(msg + len, tag, k);
            len += k;
            msg[len++] = ':';
            msg[len++] = ' ';
        }
    }
    if (!arg) {
        char buf[IO_BUF];
        ssize_t r;

        while (len + 1 < sizeof(msg) && (r = read(0, buf, sizeof(buf))) > 0) {
            size_t take = (size_t)r;

            if (len + take >= sizeof(msg)) {
                take = sizeof(msg) - 1 - len;
            }
            memcpy(msg + len, buf, take);
            len += take;
        }
    } else {
        int first = 1;

        for (; arg; arg = next_arg(&rest)) {
            size_t k = strlen(arg);

            if (!first) {
                if (len + 1 >= sizeof(msg)) {
                    break;
                }
                msg[len++] = ' ';
            }
            first = 0;
            if (len + k >= sizeof(msg)) {
                k = sizeof(msg) - 1 - len;
            }
            memcpy(msg + len, arg, k);
            len += k;
            if (len + 1 >= sizeof(msg)) {
                break;
            }
        }
    }
    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == ' ')) {
        --len; /* one trailing newline, added below */
    }
    if (len + 1 < sizeof(msg)) {
        msg[len++] = '\n';
    }
    if (len > 0 && klog(msg, len) < 0) {
        print_status("logger", -E_INVAL);
    }
}

static const char *const weekday_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};static const char *const month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
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

/* kill [-s SIGNAL | -SIGNAL] PID..., -l lists signals. Every signal
 * terminates immediately and cannot be caught; the names exist so scripts
 * written for Linux keep working. */
static int parse_signal(const char *s)
{
    const char *a = s;
    u32 v = 0;

    if (parse_num(&a, 10, &v) == 0 && (v == 9 || v == 15)) {
        return (int)v;
    }
    if (strcmp(s, "KILL") == 0 || strcmp(s, "SIGKILL") == 0) {
        return 9;
    }
    if (strcmp(s, "TERM") == 0 || strcmp(s, "SIGTERM") == 0) {
        return 15;
    }
    return -1;
}

static void cmd_kill(char *rest)
{
    char *arg = next_arg(&rest);
    int sig = 15;

    if (arg && strcmp(arg, "-l") == 0) {
        if (next_arg(&rest)) {
            print_status("kill", -E_INVAL);
            return;
        }
        puts(" 9) SIGKILL\n15) SIGTERM\n");
        return;
    }
    if (arg && arg[0] == '-' && arg[1] != '\0') {
        if (arg[1] == 's') {
            if (arg[2] == '\0') {
                arg = next_arg(&rest);
                if (!arg) {
                    print_status("kill", -E_INVAL);
                    return;
                }
            } else {
                arg += 2;
            }
            sig = parse_signal(arg);
            if (sig < 0) {
                print_status("kill", -E_INVAL);
                return;
            }
            arg = next_arg(&rest);
        } else {
            sig = parse_signal(arg + 1);
            if (sig < 0) {
                print_status("kill", -E_INVAL);
                return;
            }
            arg = next_arg(&rest);
        }
    }
    if (!arg) {
        print_status("kill", -E_INVAL);
        return;
    }
    /* Every signal terminates; numbers/names only matter for scripts. */
    (void)sig;
    for (; arg; arg = next_arg(&rest)) {
        const char *a = arg;
        u32 pid = 0;
        int r;

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
}

static void help_cmd(const char *c)
{
    if (strcmp(c, "ls") == 0) {
        puts("ls [-l] [-R] [-F] [-d] [-h] [PATH...]: names of a directory (-l: mode, uid, gid, size;\n"
             "  -R: recurse; -F: append / * =; -d: list arguments themselves; -h: human sizes);\n"
             "  a file argument lists that file; several paths get a header each\n");
    } else if (strcmp(c, "cat") == 0) {
        puts("cat [-n] [-E] [-T] [FILE...]: print files (-n numbers lines, -E marks line ends with $,\n"
             "  -T shows tabs as ^I); no FILE copies standard input\n");
    } else if (strcmp(c, "echo") == 0) {
        puts("echo [-n] [-e] [WORD...]: print the words separated by spaces (-n: no newline,\n"
             "  -e: backslash escapes like \\n \\t \\\\).\n"
             "  Redirect with > or >> like any command; quote to keep spaces: echo \"a  b\"\n");
    } else if (strcmp(c, "mkdir") == 0) {
        puts("mkdir [-p] [-m MODE] PATH...: create directories (-p makes parents, -m sets the mode)\n");
    } else if (strcmp(c, "rm") == 0) {
        puts("rm [-r] [-f] [-i] PATH...: remove files or empty directories (-r removes trees,\n"
             "  -f never complains, -i asks for each path)\n");
    } else if (strcmp(c, "mv") == 0) {
        puts("mv [-i] SRC DST: rename or move within the VFS (-i asks before overwriting)\n");
    } else if (strcmp(c, "rmdir") == 0) {
        puts("rmdir PATH...: remove empty directories only\n");
    } else if (strcmp(c, "rmtree") == 0) {
        puts("rmtree PATH: remove a tree recursively (rm -rf)\n");
    } else if (strcmp(c, "touch") == 0) {
        puts("touch [-c] FILE...: create empty files if missing (-c: never create)\n");
    } else if (strcmp(c, "cp") == 0) {
        puts("cp [-r] [-i] SRC DST: copy a file, or a tree with -r (-i asks before overwriting)\n");
    } else if (strcmp(c, "head") == 0) {
        puts("head [-N | -n N | -c N] [-q] [-v] [FILE...]: first lines/bytes (default 10 lines);\n"
             "  no FILE reads standard input; headers only with several files (-v forces them)\n");
    } else if (strcmp(c, "tail") == 0) {
        puts("tail [-N | -n N | -c N] [-q] [-v] [FILE...]: last lines/bytes (default 10 lines);\n"
             "  no FILE reads standard input; headers only with several files (-v forces them)\n");
    } else if (strcmp(c, "wc") == 0) {
        puts("wc [-l] [-w] [-c] [FILE...]: lines, words and bytes; no FILE reads standard input;\n"
             "  several files add a total line\n");
    } else if (strcmp(c, "grep") == 0) {
        puts("grep [-i] [-n] [-v] [-c] [-r] [-m N] PATTERN [FILE...]: lines containing PATTERN (a plain\n"
             "  substring, not a regex). -i ignore case, -n line numbers, -v invert, -c count,\n"
             "  -r walk directories, -m N stop after N matches. No FILE reads standard input.\n");
    } else if (strcmp(c, "stat") == 0) {
        puts("stat PATH: type, size, mode, owner\n");
    } else if (strcmp(c, "du") == 0) {
        puts("du [-h] [PATH]: bytes under a tree (default: .)\n");
    } else if (strcmp(c, "find") == 0) {
        puts("find [PATH...] [-name PAT] [-type f|d] [-maxdepth N] [-print] [-exec CMD ;] [-delete]:\n"
             "  walk trees (default: .); patterns allow * and ?; -exec runs CMD per match ({}: path)\n");
    } else if (strcmp(c, "hexdump") == 0) {
        puts("hexdump [-C] FILE: offset, hex bytes and ASCII\n");
    } else if (strcmp(c, "cmp") == 0) {
        puts("cmp FILE1 FILE2: first differing byte, or 'identical'\n");
    } else if (strcmp(c, "basename") == 0) {
        puts("basename PATH: last component\n");
    } else if (strcmp(c, "dirname") == 0) {
        puts("dirname PATH: all but the last component\n");
    } else if (strcmp(c, "seq") == 0) {
        puts("seq [-w] LAST | FIRST LAST | FIRST STEP LAST: print numbers (-w pads with zeros)\n");
    } else if (strcmp(c, "which") == 0) {
        puts("which PROG: where the shell would run it from\n");
    } else if (strcmp(c, "time") == 0) {
        puts("time PROG [ARGS]: run and report wall milliseconds\n");
    } else if (strcmp(c, "whoami") == 0) {
        puts("whoami: login name for our uid from /etc/passwd\n");
    } else if (strcmp(c, "cut") == 0) {
        puts("cut [-d DELIM] -f LIST [FILE]: delimiter fields (stdin default)\n");
    } else if (strcmp(c, "uniq") == 0) {
        puts("uniq [-c] [-d] [-u] [FILE]: collapse adjacent duplicates (stdin default);\n"
             "  -c counts, -d only repeats, -u only one-offs\n");
    } else if (strcmp(c, "tr") == 0) {
        puts("tr SET1 SET2 [FILE]: translate bytes, ranges like a-z\n");
    } else if (strcmp(c, "strings") == 0) {
        puts("strings [FILE]: runs of 4+ printable bytes (stdin default)\n");
    } else if (strcmp(c, "pwd") == 0) {
        puts("pwd: print the working directory\n");
    } else if (strcmp(c, "cd") == 0) {
        puts("cd [PATH]: change directory (default: your home from /etc/passwd).\n"
             "  `cd -` goes back (and prints it), `~` means home. Missing: -2, file: -20\n");
    } else if (strcmp(c, "pushd") == 0) {
        puts("pushd [DIR]: save cwd (up to 8 deep) and change to DIR, printing the stack\n");
    } else if (strcmp(c, "popd") == 0) {
        puts("popd: return to the top saved directory and print the stack\n");
    } else if (strcmp(c, "dirs") == 0) {
        puts("dirs: show the saved directory stack, newest last\n");
    } else if (strcmp(c, "tee") == 0) {
        puts("tee [-a] FILE: stdin to file and stdout (-a appends)\n");
    } else if (strcmp(c, "clear") == 0) {
        puts("clear: clear the screen (VGA and serial terminal)\n");
    } else if (strcmp(c, "reset") == 0) {
        puts("reset: same as clear\n");
    } else if (strcmp(c, "yes") == 0) {
        puts("yes [WORD...]: print the words (default y) forever, until killed or piped out\n");
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
        puts("uptime [-s]: seconds since boot (-s: the boot moment as UTC date and time)\n");
    } else if (strcmp(c, "date") == 0) {
        puts("date [+FORMAT | -s \"YYYY-MM-DD HH:MM:SS\"]: current UTC date and time from the RTC.\n"
             "  FORMAT: %Y %m %d %H %M %S %s %a %b %F %T %%. -s (root) sets a software offset\n"
             "  that lasts until reboot; the hardware clock is not written.\n");
    } else if (strcmp(c, "random") == 0) {
        puts("random: 64 bits from the kernel RNG\n");
    } else if (strcmp(c, "pid") == 0) {
        puts("pid: print our process id\n");
    } else if (strcmp(c, "id") == 0) {
        puts("id [-u] [-g] [-G] [-n] [USER]: uid, gid and groups (-u/-g print one id, -n names)\n");
    } else if (strcmp(c, "groups") == 0) {
        puts("groups [USER]: group names for USER (default: us) from /etc/group\n");
    } else if (strcmp(c, "su") == 0) {
        puts("su UID: root may switch down; going back to 0 is refused\n");
    } else if (strcmp(c, "useradd") == 0) {
        puts("useradd NAME: root creates a login (own group, /home/NAME, locked password).\n"
             "  NAME is [a-z_][a-z0-9_-]*, uid is the first free one from 1000\n");
    } else if (strcmp(c, "passwd") == 0) {
        puts("passwd [NAME]: root sets the login password (asks twice, never echoed).\n"
             "  Stored as salt$sha256(salt+password) in /etc/shadow (0600). No match: -22\n");
    } else if (strcmp(c, "login") == 0) {
        puts("login [NAME]: check the password and take that identity (root console).\n"
             "  Asks for the name when missing; wrong or locked: -13. `exit` returns to root\n");
    } else if (strcmp(c, "lang") == 0) {
        puts("lang [en|es]: interface language (this shell's own messages).\n"
             "  No argument prints it; setting it is root-only, stored in\n"
             "  /etc/lang and applied at once. Per-command help stays English\n");
    } else if (strcmp(c, "chmod") == 0) {
        puts("chmod [-R] MODE PATH: octal (755) or symbolic (u+x,go-w,a=rx); owner or root\n");
    } else if (strcmp(c, "chown") == 0) {
        puts("chown [-R] OWNER[:GROUP] PATH: uid/gid numbers or names from /etc/passwd and /etc/group (root only)\n");
    } else if (strcmp(c, "sync") == 0) {
        puts("sync: flush the file system to disk\n");
    } else if (strcmp(c, "sleep") == 0) {
        puts("sleep SECS: wait (whole or fractional seconds, up to one hour)\n");
    } else if (strcmp(c, "umask") == 0) {
        puts("umask [MODE]: show the creation mask, or set it (octal, root may use -S style too)\n");
    } else if (strcmp(c, "logger") == 0) {
        puts("logger [-t TAG] MESSAGE...: append a line to the kernel log (shows in dmesg)\n");
    } else if (strcmp(c, "kill") == 0) {
        puts("kill [-s SIGNAL | -SIGNAL] PID...: terminate processes (-l lists signals)\n");
    } else if (strcmp(c, "hostname") == 0) {
        puts("hostname [NAME]: show or set /etc/hostname\n");
    } else if (strcmp(c, "uname") == 0) {
        puts("uname [-a] [-s] [-n] [-r] [-m]: system name, or selected fields of /proc/version\n");
    } else if (strcmp(c, "run") == 0) {
        puts("run PROG [ARGS]: spawn /bin or absolute program and wait\n");
    } else if (strcmp(c, "poweroff") == 0 || strcmp(c, "halt") == 0 ||
               strcmp(c, "shutdown") == 0) {
        puts("poweroff|halt|shutdown [-h] [-r]: halt (default, root only) or -r to reboot\n");
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
        puts("df [-h] [-i] [PATH]: system disk backend and capacity (/proc/disk; -h sizes, -i nodes)\n");
    } else if (strcmp(c, "ip") == 0 || strcmp(c, "ifconfig") == 0) {
        puts("ip [addr|link]: live NIC state from the e1000 driver\n");
    } else if (strcmp(c, "sort") == 0) {
        puts("usage: sort [-r] [-n] [-u] [-c] [FILE...]   (no FILE: standard input; up to 64 KiB / 4096 lines;\n"
             "  -c only checks the order and reports the first disorder)\n");
    } else if (strcmp(c, "tac") == 0) {
        puts("usage: tac [FILE...]   (lines in reverse order)\n");
    } else if (strcmp(c, "nl") == 0) {
        puts("usage: nl [FILE...]   (numbers every line)\n");
    } else if (strcmp(c, "split") == 0) {
        puts("split [-l N] [-b N[k|m]] [-d] [-a N] [FILE [PREFIX]]: chop a file into PREFIXaa...\n"
             "  pieces (-l lines, -b bytes; -d numeric suffixes of length -a)\n");
    } else if (strcmp(c, "paste") == 0) {
        puts("paste [-d LIST] [-s] [FILE...]: merge lines side by side (-d delimiters, -s serial)\n");
    } else if (strcmp(c, "xargs") == 0) {
        puts("xargs [-n MAX] [-0] CMD [ARGS...]: append stdin lines to CMD and run it in batches\n"
             "  (-0 splits on NUL; empty input still runs once; CMD must be a /bin program,\n"
             "  shell builtins like echo do not work here)\n");
    } else if (strcmp(c, "file") == 0) {
        puts("file FILE...: ELF details, #! scripts, empty/text/data from the first bytes\n");
    } else if (strcmp(c, "type") == 0) {
        puts("type NAME...: shell builtin, /bin path, or missing\n");
    } else if (strcmp(c, "code") == 0) {
        puts("code [PATH]: fullscreen file browser + text editor (/bin/code).\n"
             "  Tree: Up/Down move, Enter opens, h hidden, Tab focuses editor, q quits.\n"
             "  Editor: arrows/Home/End move, type/Enter/Tab edit, Backspace/Delete\n"
             "  erase, Ctrl+S saves, Ctrl+F searches, Ctrl+W closes, Esc quits.\n"
             "  ? shows keys, Y/N/C answers unsaved-change prompts. 64 KiB max.\n"
             "  Copy/paste/undo need selection: not in v1.\n");
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

static const char *const help_categories[2][5] = {
    {"Files and directories", "Text and data", "System and users", "Processes and hardware",
     "Programs in /bin"},
    {"Archivos y directorios", "Texto y datos", "Sistema y usuarios", "Procesos y hardware",
     "Programas en /bin"},
};

static const struct help_entry help_table[] = {
    {"ls", "list a directory (-l, -R, -F classify, -d, -h)", 0},
    {"cd", "change directory (default: home; - back, ~ home)", 0},
    {"pwd", "print the working directory", 0},
    {"pushd", "save cwd and change directory", 0},
    {"popd", "return to the saved directory", 0},
    {"dirs", "show the directory stack", 0},
    {"mkdir", "create directories (-p parents, -m MODE)", 0},
    {"rmdir", "remove empty directories", 0},
    {"touch", "create empty files (-c: no create)", 0},
    {"cp", "copy files (-r trees, -i confirm)", 0},
    {"mv", "move or rename (-i confirm)", 0},
    {"rm", "remove files or empty dirs (-r trees, -i confirm)", 0},
    {"rmtree", "remove a directory and everything in it", 0},
    {"stat", "show type, mode, owner and size", 0},
    {"du", "disk usage of a tree", 0},
    {"find", "search a tree (-name -type -maxdepth -exec -delete)", 0},
    {"basename", "strip the directory from a path", 0},
    {"dirname", "strip the last path component", 0},
    {"tee", "copy input to a file and to the screen", 0},
    {"cat", "print files, or standard input (-n -E -T)", 1},
    {"echo", "print arguments (-n -e, -> file)", 1},
    {"head", "first lines (-c bytes) of a file or input", 1},
    {"tail", "last lines (-c bytes) of a file or input", 1},
    {"wc", "count lines, words and bytes (-l -w -c)", 1},
    {"grep", "lines containing text (-i -n -v -c -r -m)", 1},
    {"cut", "select fields or characters from lines", 1},
    {"sort", "sort lines (-r reverse, -n numeric, -u unique, -c check)", 1},
    {"uniq", "collapse repeated adjacent lines (-c -d -u)", 1},
    {"tac", "print lines in reverse order", 1},
    {"nl", "number lines", 1},
    {"tr", "translate or delete characters", 1},
    {"strings", "printable strings found in a file", 1},
    {"hexdump", "bytes of a file in hex", 1},
    {"cmp", "compare two files byte by byte", 1},
    {"seq", "print a sequence of numbers (-w pads)", 1},
    {"split", "split a file into pieces (-l -b -d -a)", 1},
    {"paste", "merge lines side by side (-d -s)", 1},
    {"xargs", "build /bin commands from input lines (-n -0)", 1},
    {"file", "identify file type by contents", 1},
    {"type", "tell builtins from /bin programs", 1},
    {"clear", "clear the screen", 2},
    {"reset", "same as clear", 2},
    {"date", "show or set the date and time (UTC)", 2},
    {"uptime", "time since boot (-s: boot moment)", 2},
    {"uname", "kernel name and version (-a -s -n -r -m)", 2},
    {"hostname", "show the host name", 2},
    {"whoami", "current user name", 2},
    {"groups", "group names of a user", 2},
    {"id", "user and group ids (-u -g -G -n)", 2},
    {"su", "switch to another user id (root only)", 2},
    {"useradd", "create a login with home and locked password (root)", 2},
    {"passwd", "set a login password, salted SHA-256 (root)", 2},
    {"login", "verify a password and take that identity (root)", 2},
    {"lang", "show or set the interface language (en|es)", 2},
    {"chmod", "change file permissions, octal or symbolic", 2},
    {"chown", "change file owner: UID GID or NAME[:GROUP]", 2},
    {"sync", "write the file system to disk", 2},
    {"sleep", "wait some seconds (fractions allowed)", 2},
    {"yes", "repeat a line until killed", 2},
    {"time", "run a command and report how long it took", 2},
    {"which", "locate a command", 2},
    {"umask", "show or set the creation mask", 2},
    {"logger", "append a line to the kernel log", 2},
    {"random", "random numbers from the kernel generator", 2},
    {"run", "run a program by path", 2},
    {"poweroff", "power the machine off", 2},
    {"halt", "stop the machine", 2},
    {"shutdown", "halt or reboot (-h default, -r reboots)", 2},
    {"reboot", "restart the machine", 2},
    {"exit", "leave the shell", 2},
    {"help", "this list; `help CMD` for details", 2},
    {"ps", "list processes", 3},
    {"kill", "terminate processes (-s SIGNAL, -l lists)", 3},
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
    {"code", "fullscreen file browser + text editor", 4},
};

#define HELP_COUNT ((int)(sizeof(help_table) / sizeof(help_table[0])))

/* True when `name` is a shell builtin (for `type`). */
static int is_builtin(const char *name)
{
    for (int i = 0; i < HELP_COUNT; ++i) {
        if (strcmp(help_table[i].name, name) == 0 && help_table[i].category != 4) {
            return 1;
        }
    }
    return 0;
}

static void help(void)
{
    puts(T("help0"));
    puts(T("help1"));
    puts(T("help2"));
    puts(T("help3"));
    puts(T("help4"));
    for (int cat = 0; cat < 5; ++cat) {
        puts("\n");
        puts(help_categories[lang_es][cat]);
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
    puts("\nEvery command above takes < > >> redirection; most filters also\n"
         "read standard input. Unknown words run /bin/<word>.\n");
}

/* One symbolic clause [ugoa]*[+-=][rwxXst]* (copy forms g=u etc. allowed).
 * `old` is the current mode, `is_dir` gates X. An omitted who means `a`
 * (for `=` the umask is not applied: this shell documents the difference).
 * `s` sets setuid/setgid for the named u/g classes (`o+s` is accepted and
 * does nothing, like GNU); `t` always sets the sticky bit when named.
 * Returns 0 with *out set. */
static int apply_clause(const char *s, u32 old, int is_dir, u32 *out)
{
    u32 who = 0;
    char op = 0;
    u32 mode = old;
    u32 perms = 0;

    while (*s == 'u' || *s == 'g' || *s == 'o' || *s == 'a') {
        if (*s == 'u') {
            who |= 0700u;
        } else if (*s == 'g') {
            who |= 0070u;
        } else if (*s == 'o') {
            who |= 0007u;
        } else {
            who |= 0777u;
        }
        ++s;
    }
    if (*s != '+' && *s != '-' && *s != '=') {
        return -1;
    }
    op = *s++;
    if (who == 0) {
        who = 0777u;
    }
    /* A bare operator with no perms clears (for =) or no-ops (+/-). */
    if (*s == '\0' || *s == ',') {
        if (op == '=') {
            mode &= ~(who | 07000u);
        }
        *out = mode;
        return 0;
    }
    /* Copy forms (u/g/o as the perm): replicate one class across who. */
    if ((*s == 'u' || *s == 'g' || *s == 'o') && (s[1] == '\0' || s[1] == ',')) {
        u32 src = old;

        if (*s == 'u') {
            src = (old >> 6) & 7u;
        } else if (*s == 'g') {
            src = (old >> 3) & 7u;
        } else {
            src = old & 7u;
        }
        perms = (src | (src << 3) | (src << 6)) & who;
        ++s;
    } else {
        while (*s && *s != ',') {
            if (*s == 'r') {
                perms |= 0444u & who;
            } else if (*s == 'w') {
                perms |= 0222u & who;
            } else if (*s == 'x') {
                perms |= 0111u & who;
            } else if (*s == 'X') {
                if (is_dir || (old & 0111u) != 0) {
                    perms |= 0111u & who;
                }
            } else if (*s == 's') {
                if ((who & 0700u) != 0) {
                    perms |= 04000u;
                }
                if ((who & 0070u) != 0) {
                    perms |= 02000u;
                }
            } else if (*s == 't') {
                perms |= 01000u;
            } else {
                return -1;
            }
            ++s;
        }
        perms &= who | 07000u;
    }
    if (op == '+') {
        mode |= perms;
    } else if (op == '-') {
        mode &= ~perms;
    } else {
        /* = assigns exactly: who-rwx plus any named s/t, rest cleared. */
        mode = (mode & ~(who | 07000u)) | perms;
    }
    *out = mode;
    return 0;
}

/* Full symbolic spec: comma-separated clauses. */
static int parse_symbolic(const char *spec, u32 old, int is_dir, u32 *out)
{
    u32 mode = old;

    if (*spec == '\0') {
        return -1;
    }
    for (;;) {
        const char *comma = spec;

        while (*comma && *comma != ',') {
            ++comma;
        }
        {
            char clause[32];
            size_t k = (size_t)(comma - spec);

            if (k == 0 || k >= sizeof(clause)) {
                return -1;
            }
            memcpy(clause, spec, k);
            clause[k] = '\0';
            if (apply_clause(clause, mode, is_dir, &mode) != 0) {
                return -1;
            }
        }
        if (*comma == '\0') {
            break;
        }
        spec = comma + 1;
    }
    *out = mode & 07777u;
    return 0;
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
        } else if (strcmp(cmd, "split") == 0) {
            cmd_split(rest);
        } else if (strcmp(cmd, "paste") == 0) {
            cmd_paste(rest);
        } else if (strcmp(cmd, "xargs") == 0) {
            cmd_xargs(rest);
        } else if (strcmp(cmd, "file") == 0) {
            cmd_file(rest);
        } else if (strcmp(cmd, "type") == 0) {
            cmd_type(rest);
        } else if (strcmp(cmd, "cat") == 0) {
            cmd_cat(rest);
        } else if (strcmp(cmd, "echo") == 0) {
            cmd_echo(rest);
        } else if (strcmp(cmd, "mkdir") == 0) {
            char *arg = next_arg(&rest);
            char path[PATH_MAX];
            int pflag = 0;
            u32 mmode = 0;
            int have_mode = 0;

            /* Flags first (-p, -m MODE, clustered): paths follow. */
            for (; arg && arg[0] == '-' && arg[1] != '\0'; arg = next_arg(&rest)) {
                if (strcmp(arg, "-p") == 0) {
                    pflag = 1;
                } else if (strcmp(arg, "-m") == 0) {
                    const char *a;

                    arg = next_arg(&rest);
                    if (!arg) {
                        print_status("mkdir", -E_INVAL);
                        goto mkdir_done;
                    }
                    a = arg;
                    if (parse_num(&a, 8, &mmode) != 0 || mmode > 07777u) {
                        print_status("mkdir", -E_INVAL);
                        goto mkdir_done;
                    }
                    have_mode = 1;
                    arg = next_arg(&rest);
                    break;
                } else {
                    print_status("mkdir", -E_INVAL);
                    goto mkdir_done;
                }
            }
            if (!arg) {
                print_status("mkdir", -E_INVAL);
            } else {
                for (; arg; arg = next_arg(&rest)) {
                    int r;
                    struct stat_info st;

                    resolve(arg, path);
                    r = mkdir(path);
                    if (r == -E_EXIST && pflag) {
                        /* -p: existing directories are fine, files are not. */
                        if (stat(path, &st) == 0 && st.type == 2) {
                            r = 0;
                        }
                    }
                    if (r == 0 && have_mode) {
                        r = chmod(path, mmode);
                    }
                    if (r < 0) {
                        print_status("mkdir", r);
                    }
                }
            }
        mkdir_done:;
        } else if (strcmp(cmd, "rmdir") == 0) {
            char *arg = next_arg(&rest);

            if (!arg) {
                print_status("rmdir", -E_INVAL);
            } else {
                char path[PATH_MAX];

                for (; arg; arg = next_arg(&rest)) {
                    struct stat_info st;
                    int r;

                    resolve(arg, path);
                    if (stat(path, &st) != 0) {
                        print_status("rmdir", -E_NOENT);
                        continue;
                    }
                    if (st.type != 2) {
                        print_status("rmdir", -E_NOTDIR);
                        continue;
                    }
                    r = unlink(path);
                    if (r < 0) {
                        print_status("rmdir", r);
                    }
                }
            }
        } else if (strcmp(cmd, "rm") == 0) {
            char *arg = next_arg(&rest);
            char path[PATH_MAX];
            int rec = 0;
            int interactive = 0;

            for (; arg && arg[0] == '-' && arg[1] != '\0'; arg = next_arg(&rest)) {
                for (const char *f = arg + 1; *f; ++f) {
                    if (*f == 'r' || *f == 'R') {
                        rec = 1;
                    } else if (*f == 'i') {
                        interactive = 1;
                    } else if (*f == 'f') {
                        /* Accepted: errors are already non-fatal per path. */
                    } else {
                        print_status("rm", -E_INVAL);
                        goto rm_done;
                    }
                }
            }
            if (!arg) {
                print_status("rm", -E_INVAL);
            } else {
                for (; arg; arg = next_arg(&rest)) {
                    int r;

                    resolve(arg, path);
                    if (interactive && !confirm(path)) {
                        continue;
                    }
                    if (rec) {
                        r = rmtree(path);
                    } else {
                        r = unlink(path);
                    }
                    if (r < 0) {
                        print_status("rm", r);
                    }
                }
            }
        rm_done:;
        } else if (strcmp(cmd, "mv") == 0) {
            char *src = next_arg(&rest);
            char *dst;
            char spath[PATH_MAX], dpath[PATH_MAX];
            int interactive = 0;

            while (src && src[0] == '-' && src[1] != '\0') {
                if (strcmp(src, "-i") != 0) {
                    print_status("mv", -E_INVAL);
                    goto mv_done;
                }
                interactive = 1;
                src = next_arg(&rest);
            }
            dst = next_arg(&rest);
            if (!src || !dst || next_arg(&rest)) {
                print_status("mv", -E_INVAL);
            } else {
                int r;
                struct stat_info st;

                resolve(src, spath);
                resolve(dst, dpath);
                if (interactive && stat(dpath, &st) == 0 && !confirm(dpath)) {
                    goto mv_done;
                }
                r = rename(spath, dpath);
                if (r < 0) {
                    print_status("mv", r);
                }
            }
        mv_done:;
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
        } else if (strcmp(cmd, "pushd") == 0) {
            cmd_pushd(rest);
        } else if (strcmp(cmd, "popd") == 0) {
            cmd_popd(rest);
        } else if (strcmp(cmd, "dirs") == 0) {
            cmd_dirs(rest);
        } else if (strcmp(cmd, "tee") == 0) {
            cmd_tee(rest);
        } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "reset") == 0) {
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
            char *arg = next_arg(&rest);
            u64 ms = uptime_ms();

            if (arg && strcmp(arg, "-s") == 0) {
                /* Boot moment = wall clock minus uptime, as UTC. */
                long now = time_unix();
                struct axys_civil c;

                if (next_arg(&rest) || now < 0 || (u64)now < ms / 1000) {
                    print_status("uptime", -E_INVAL);
                } else {
                    axys_civil_from_unix(now - (long)(ms / 1000), &c);
                    put_u64(c.year);
                    puts("-");
                    put_2digits(c.month);
                    puts("-");
                    put_2digits(c.day);
                    puts(" ");
                    put_2digits(c.hour);
                    puts(":");
                    put_2digits(c.minute);
                    puts(":");
                    put_2digits(c.second);
                    puts("\n");
                }
            } else if (arg) {
                print_status("uptime", -E_INVAL);
            } else {
                put_u64(ms / 1000);
                puts(".");
                put_u64((ms % 1000) / 100);
                puts(" s\n");
            }
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
            cmd_id(rest);
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
        } else if (strcmp(cmd, "useradd") == 0) {
            cmd_useradd(rest);
        } else if (strcmp(cmd, "passwd") == 0) {
            cmd_passwd(rest);
        } else if (strcmp(cmd, "login") == 0) {
            cmd_login(rest);
        } else if (strcmp(cmd, "lang") == 0) {
            cmd_lang(rest);
        } else if (strcmp(cmd, "chmod") == 0) {
            char *p = rest;
            char *tok;
            u32 mode = 0;
            int rec = 0;
            int r = 0;

            if (p[0] == '-' && p[1] == 'R' && (p[2] == ' ' || p[2] == '\0')) {
                rec = 1;
                p += 2;
            }
            tok = next_arg(&p);
            if (!tok) {
                print_status("chmod", -E_INVAL);
            } else if (tok[0] >= '0' && tok[0] <= '7') {
                const char *a = tok;
                char *patharg;

                r = parse_num(&a, 8, &mode) == 0 ? 0 : -22;
                patharg = next_arg(&p);
                if (r == 0 && (!patharg || next_arg(&p))) {
                    r = -E_INVAL;
                }
                if (r == 0) {
                    char path[PATH_MAX];

                    resolve(patharg, path);
                    r = rec ? chmod_tree(path, mode, 0, 0, 0, 0, 0)
                            : chmod(path, mode);
                }
                if (r < 0) {
                    print_status("chmod", r);
                }
            } else {
                /* Symbolic mode: validate first so a bad spec fails before
                 * touching anything, then apply per node (X and copies read
                 * each node's own mode). */
                char *patharg = next_arg(&p);
                char *extra = patharg ? next_arg(&p) : 0;
                char path[PATH_MAX];
                struct stat_info st;
                u32 dummy = 0;

                if (!patharg || extra) {
                    print_status("chmod", -E_INVAL);
                } else if (parse_symbolic(tok, 0, 0, &dummy) != 0) {
                    print_status("chmod", -22);
                } else {
                    resolve(patharg, path);
                    if (!rec) {
                        u32 want = 0;

                        if (stat(path, &st) != 0) {
                            print_status("chmod", -E_NOENT);
                        } else if (parse_symbolic(tok, st.mode, st.type == 2,
                                                  &want) != 0) {
                            print_status("chmod", -22);
                        } else {
                            r = chmod(path, want);
                            if (r < 0) {
                                print_status("chmod", r);
                            }
                        }
                    } else {
                        r = chmod_tree(path, 0, 0, 0, 0, 0, tok);
                        if (r < 0) {
                            print_status("chmod", r);
                        }
                    }
                }
            }
        } else if (strcmp(cmd, "chown") == 0) {
            char *p = rest;
            char *tok;
            int rec = 0;

            if (p[0] == '-' && p[1] == 'R' && (p[2] == ' ' || p[2] == '\0')) {
                rec = 1;
                p += 2;
            }
            tok = next_arg(&p);
            if (!tok) {
                print_status("chown", -E_INVAL);
            } else {
                /* Forms: UID GID PATH (legacy), OWNER[:GROUP] PATH,
                 * :GROUP PATH. A colon always means the new form. */
                char *colon = 0;
                char *patharg;
                u32 uid = 0, gid = 0;
                int keep_uid = 0, keep_gid = 0;
                int r = 0;

                for (char *c = tok; *c; ++c) {
                    if (*c == ':') {
                        colon = c;
                        break;
                    }
                }
                if (!colon) {
                    char *second = next_arg(&p);

                    if (second) {
                        /* Two leading words: legacy UID GID PATH. */
                        const char *a = tok;

                        r = parse_num(&a, 10, &uid) == 0 ? 0 : -22;
                        if (r == 0) {
                            a = second;
                            r = parse_num(&a, 10, &gid) == 0 ? 0 : -22;
                        }
                        patharg = next_arg(&p);
                        if (r == 0 && (!patharg || next_arg(&p))) {
                            r = -E_INVAL;
                        }
                        if (r != 0) {
                            print_status("chown", r);
                        } else {
                            char path[PATH_MAX];

                            resolve(patharg, path);
                            r = rec ? chmod_tree(path, 0, uid, gid, 1, 0, 0)
                                    : chown(path, uid, gid);
                            if (r < 0) {
                                print_status("chown", r);
                            }
                        }
                        goto chown_done;
                    }
                    /* Single word: an owner name (or bare uid). */
                    patharg = second;
                    if (!patharg || next_arg(&p)) {
                        print_status("chown", -E_INVAL);
                        goto chown_done;
                    }
                    {
                        const char *a = tok;
                        u32 v = 0;

                        if (parse_num(&a, 10, &v) == 0) {
                            uid = v;
                        } else if (passwd_uid(tok, &uid) != 0) {
                            print_status("chown", -22);
                            goto chown_done;
                        }
                    }
                    keep_gid = 1;
                } else {
                    /* OWNER:GROUP, OWNER:, :GROUP (either side may be empty). */
                    char owner[32], group[32];
                    size_t on = (size_t)(colon - tok);
                    size_t gn = strlen(colon + 1);

                    patharg = next_arg(&p);
                    if (on >= sizeof(owner) || gn >= sizeof(group) ||
                        !patharg || next_arg(&p)) {
                        print_status("chown", -E_INVAL);
                        goto chown_done;
                    }
                    memcpy(owner, tok, on);
                    owner[on] = '\0';
                    memcpy(group, colon + 1, gn + 1);
                    if (on == 0) {
                        keep_uid = 1;
                    } else {
                        const char *a = owner;
                        u32 v = 0;

                        if (parse_num(&a, 10, &v) == 0) {
                            uid = v;
                        } else if (passwd_uid(owner, &uid) != 0) {
                            print_status("chown", -22);
                            goto chown_done;
                        }
                    }
                    if (gn == 0) {
                        keep_gid = 1;
                    } else {
                        const char *a = group;
                        u32 v = 0;

                        if (parse_num(&a, 10, &v) == 0) {
                            gid = v;
                        } else if (group_gid(group, &gid) != 0) {
                            print_status("chown", -22);
                            goto chown_done;
                        }
                    }
                }
                {
                    char path[PATH_MAX];
                    struct stat_info st;

                    resolve(patharg, path);
                    if (stat(path, &st) != 0) {
                        print_status("chown", -E_NOENT);
                    } else {
                        if (keep_uid) {
                            uid = st.uid;
                        }
                        if (keep_gid) {
                            gid = st.gid;
                        }
                        r = rec ? chmod_tree(path, 0, uid, gid, 1, 0, 0)
                                : chown(path, uid, gid);
                        if (r < 0) {
                            print_status("chown", r);
                        }
                    }
                }
            chown_done:;
            }
        } else if (strcmp(cmd, "sync") == 0) {
            int r = sync();
            if (r < 0) {
                print_status("sync", r);
            }
        } else if (strcmp(cmd, "sleep") == 0) {
            /* Seconds, with an optional fraction (sleep 0.5). */
            const char *a = rest;
            u32 s = 0;
            u64 ms = 0;
            int r = parse_num(&a, 10, &s);

            if (r == 0 && *a == '.') {
                u32 frac = 0;
                u32 mult = 100;
                const char *f = a + 1;

                ++a;
                while (*a >= '0' && *a <= '9') {
                    if (mult > 0) {
                        frac += (u32)(*a - '0') * mult;
                        mult /= 10;
                    }
                    ++a;
                }
                if (f == a) {
                    r = -1; /* lone dot */
                } else {
                    ms = (u64)s * 1000 + frac;
                }
                (void)f;
            } else if (r == 0) {
                if (s > 86400u * 1000u) {
                    r = -1; /* absurd, and s * 1000 would wrap u32 math */
                } else {
                    ms = (u64)s * 1000;
                }
            }
            while (*a == ' ') {
                ++a;
            }
            if (r != 0 || *a != '\0') {
                print_status("sleep", -22);
            } else {
                sleep_ms(ms);
            }
        } else if (strcmp(cmd, "yes") == 0) {
            /* Repeat a line until killed (Ctrl-C) or the output breaks.
             * Words join with spaces like echo; bare `yes` prints "y". */
            char *words[16];
            int nwords = 0;
            char *arg;

            for (arg = next_arg(&rest); arg && nwords < 16; arg = next_arg(&rest)) {
                words[nwords++] = arg;
            }
            if (arg) {
                print_status("yes", -E_INVAL);
                return 0;
            }
            /* Pipes are sequential temp files, so `yes | head` first fills
             * the disk: write 4 KiB chunks so even that finishes fast
             * (ENOSPC still ends us, as does a closed pipe or Ctrl-C). */
            {
                char line[256];
                char chunk[4096];
                size_t llen = 0;
                size_t off = 0;

                if (nwords == 0) {
                    line[0] = 'y';
                    line[1] = '\n';
                    llen = 2;
                } else {
                    for (int i = 0; i < nwords; ++i) {
                        size_t k = strlen(words[i]);

                        if (llen + k + 2 >= sizeof(line)) {
                            print_status("yes", -E_INVAL);
                            return 0;
                        }
                        if (i > 0) {
                            line[llen++] = ' ';
                        }
                        memcpy(line + llen, words[i], k);
                        llen += k;
                    }
                    line[llen++] = '\n';
                }
                while (off + llen <= sizeof(chunk)) {
                    memcpy(chunk + off, line, llen);
                    off += llen;
                }
                for (;;) {
                    if (write(1, chunk, (u64)off) != (ssize_t)off) {
                        return 0;
                    }
                }
            }
        } else if (strcmp(cmd, "kill") == 0) {
            cmd_kill(rest);
        } else if (strcmp(cmd, "hostname") == 0) {
            cmd_hostname(rest);
        } else if (strcmp(cmd, "umask") == 0) {
            cmd_umask(rest);
        } else if (strcmp(cmd, "logger") == 0) {
            cmd_logger(rest);
        } else if (strcmp(cmd, "uname") == 0) {
            cmd_uname(rest);
        } else if (strcmp(cmd, "whoami") == 0) {
            cmd_whoami();
        } else if (strcmp(cmd, "groups") == 0) {
            cmd_groups(rest);
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
        } else if (strcmp(cmd, "code") == 0) {
            /* The editor needs absolute paths (it has no cwd): resolve
             * here, defaulting to where we stand. */
            char *arg = next_arg(&rest);
            char path[PATH_MAX];

            if (arg && next_arg(&rest)) {
                print_status("code", -E_INVAL);
            } else {
                if (!arg) {
                    memcpy(path, cwd, strlen(cwd) + 1);
                } else {
                    resolve(arg, path);
                }
                run("/bin/code", path);
            }
        } else if (strcmp(cmd, "poweroff") == 0 || strcmp(cmd, "halt") == 0) {
            print_status("poweroff", power(0));
        } else if (strcmp(cmd, "shutdown") == 0) {
            char *arg = next_arg(&rest);

            if (!arg) {
                print_status("poweroff", power(0));
            } else if ((strcmp(arg, "-r") == 0 ||
                        strcmp(arg, "--reboot") == 0) &&
                       !next_arg(&rest)) {
                print_status("reboot", power(1));
            } else if ((strcmp(arg, "-h") == 0 || strcmp(arg, "-H") == 0 ||
                        strcmp(arg, "--halt") == 0 ||
                        strcmp(arg, "--poweroff") == 0) &&
                       !next_arg(&rest)) {
                print_status("poweroff", power(0));
            } else {
                print_status("shutdown", -E_INVAL);
            }
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
                    puts(T("unknown"));
                    puts(cmd);
                    puts("\n");
                }
            } else {
                /* Too long for /bin/<cmd>: say so instead of swallowing it. */
                puts(T("unknown"));
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
            /* The previous stage wrote the pipe, unless it redirected to
             * a file instead: then this stage's input is empty (/dev/null),
             * not the terminal. Reading the terminal here hung the shell
             * until Ctrl-D (`echo x > f | cat` never came back). */
            if (pipe_used[(i - 1) & 1]) {
                in_fd = open(pipe_path[(i - 1) & 1], 0);
            } else {
                in_fd = open("/dev/null", 0);
            }
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
        if (find_nested_exit) {
            /* `find ... -exec exit` above: a nested exit must leave the
             * shell, and the void cmd_find cannot return it itself. */
            find_nested_exit = 0;
            exit_shell = 1;
        }
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
    lang_load();
    puts("+-- axys shell ----------------------------------------+\n"
         "| 'help' lists commands, 'help CMD' explains one.      |\n"
         "| ';' chains, '|' pipes, < > >> redirect.              |\n"
         "+------------------------------------------------------+\n");
    puts(T("banner2"));
    puts("\n");
    puts(T("banner3"));
    puts("\n");
    /* Ready marker, intentionally untranslated: every test driver waits
     * for exactly this line. */
    puts("axys shell ready (type 'help')\n");
    for (;;) {
        ssize_t n;
        char *seg;
        int done = 0;

        puts("axys:");
        puts(cwd);
        puts(getuid() == 0 ? "# " : "$ ");
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
