#include "axys.h"

/* /sbin/init: a small interactive shell. */

#define LINE_MAX 256
#define ARG_MAX 8

static void print_status(const char *what, long value)
{
    puts(what);
    puts(": ");
    if (value < 0) {
        puts("-");
        put_u64((u64)-value);
    } else {
        put_u64((u64)value);
    }
    puts("\n");
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

static void cmd_ls(const char *path)
{
    char name[64];
    char child[128];
    struct stat_info st;

    if (path[0] == '\0') {
        path = "/";
    }
    for (u64 i = 0;; ++i) {
        int r = readdir(path, i, name, sizeof(name));
        size_t pl = strlen(path);

        if (r < 0) {
            print_status("ls", r);
            return;
        }
        if (r == 0) {
            return;
        }
        if (pl + strlen(name) + 2 < sizeof(child)) {
            memcpy(child, path, pl);
            if (pl == 0 || child[pl - 1] != '/') {
                child[pl++] = '/';
            }
            memcpy(child + pl, name, strlen(name) + 1);
            if (stat(child, &st) == 0) {
                put_mode(st.type, st.mode);
                puts(" ");
                put_u64(st.uid);
                puts(" ");
                put_u64(st.gid);
                puts(" ");
                put_u64(st.size);
                puts("\t");
            }
        }
        puts(name);
        puts(r == 2 ? "/\n" : "\n");
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

static void cmd_cat(const char *path)
{
    char buf[128];
    int fd = open(path, 0);
    ssize_t n;

    if (fd < 0) {
        print_status("cat", fd);
        return;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, (size_t)n);
    }
    close(fd);
}

/* echo TEXT [> FILE | >> FILE] */
static void cmd_echo(char *rest)
{
    char *redirect = 0;
    int append = 0;

    for (char *c = rest; *c; ++c) {
        if (*c == '>') {
            *c = '\0';
            append = c[1] == '>';
            redirect = c + (append ? 2 : 1);
            while (*redirect == ' ') {
                ++redirect;
            }
            break;
        }
    }
    size_t n = strlen(rest);
    while (n > 0 && rest[n - 1] == ' ') {
        rest[--n] = '\0';
    }
    if (redirect == 0) {
        puts(rest);
        puts("\n");
        return;
    }
    int fd = open(redirect, O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC));

    if (fd < 0) {
        print_status("echo", fd);
        return;
    }
    write(fd, rest, n);
    write(fd, "\n", 1);
    close(fd);
}

static void help(void)
{
    puts("builtins: help ls cat echo mkdir rm mv rmtree meminfo uptime random pid id su chmod chown sync sleep run poweroff reboot exit\n"
         "any other word runs /bin/<word> (arguments are passed through)\n"
         "try: hello world | crash null | crash kexec | crash badptr | heap | fileio\n");
}

int main(const char *args, size_t len)
{
    char line[LINE_MAX];

    (void)args;
    (void)len;
    puts("axys shell ready (type 'help')\n");
    for (;;) {
        ssize_t n;
        char *cmd;
        char *rest;

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
        cmd = line;
        while (*cmd == ' ') {
            ++cmd;
        }
        if (*cmd == '\0') {
            continue;
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
            help();
        } else if (strcmp(cmd, "ls") == 0) {
            cmd_ls(rest);
        } else if (strcmp(cmd, "cat") == 0) {
            cmd_cat(rest);
        } else if (strcmp(cmd, "echo") == 0) {
            cmd_echo(rest);
        } else if (strcmp(cmd, "mkdir") == 0) {
            int r = mkdir(rest);
            if (r < 0) {
                print_status("mkdir", r);
            }
        } else if (strcmp(cmd, "rm") == 0) {
            int r = unlink(rest);
            if (r < 0) {
                print_status("rm", r);
            }
        } else if (strcmp(cmd, "mv") == 0) {
            char *dst = rest;

            while (*dst && *dst != ' ') {
                ++dst;
            }
            if (*dst) {
                *dst++ = '\0';
            }
            while (*dst == ' ') {
                ++dst;
            }
            if (*rest == '\0' || *dst == '\0') {
                print_status("mv", -22);
            } else {
                int r = rename(rest, dst);
                if (r < 0) {
                    print_status("mv", r);
                }
            }
        } else if (strcmp(cmd, "rmtree") == 0) {
            print_status("rmtree", rmtree(rest));
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
        } else if (strcmp(cmd, "uptime") == 0) {
            u64 ms = uptime_ms();
            put_u64(ms / 1000);
            puts(".");
            put_u64((ms % 1000) / 100);
            puts(" s\n");
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
            int r = parse_num(&a, 8, &mode) == 0 ? 0 : -22;

            while (*a == ' ') {
                ++a;
            }
            if (r == 0) {
                r = chmod(a, mode);
            }
            if (r < 0) {
                print_status("chmod", r);
            }
        } else if (strcmp(cmd, "chown") == 0) {
            const char *a = rest;
            u32 uid = 0;
            u32 gid = 0;
            int r = parse_num(&a, 10, &uid) == 0 ? 0 : -22;

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
                r = chown(a, uid, gid);
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
        } else if (strcmp(cmd, "run") == 0) {
            char *a = rest;

            while (*a && *a != ' ') {
                ++a;
            }
            if (*a) {
                *a++ = '\0';
            }
            run(rest, a);
        } else if (strcmp(cmd, "poweroff") == 0) {
            print_status("poweroff", power(0));
        } else if (strcmp(cmd, "reboot") == 0) {
            print_status("reboot", power(1));
        } else if (strcmp(cmd, "exit") == 0) {
            break;
        } else {
            char path[64];
            size_t cl = strlen(cmd);

            if (cmd[0] == '/') {
                run(cmd, rest);
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
            }
        }
    }
    return 0;
}
