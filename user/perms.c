#include "axys.h"

/* Runs as an unprivileged user (uid 1000). Every privileged action must be
 * refused; every action the user is entitled to must work. The argument is the
 * pid of a root-owned process that must be immune to our kill(). Returns 0 if
 * all checks pass, otherwise the number of the first failing check. */
static int parse(const char *s)
{
    int v = 0;

    while (s && *s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return v;
}

int main(const char *args, size_t len)
{
    char buf[16];
    struct stat_info st;
    int fd;

    (void)len;
    if (getuid() != 1000 || getgid() != 1000) return 1;
    fd = open("/etc/passwd", 0);
    if (fd < 3 || read(fd, buf, sizeof(buf)) <= 0) return 2;      /* world-readable */
    close(fd);
    if (open("/etc/passwd", O_WRONLY) != -13) return 3;            /* not writable */
    if (open("/etc/secret", 0) != -13) return 4;                   /* mode 0600 root */
    if (open("/root/anything", 0) != -13) return 5;                /* /root is 0700 */
    if (unlink("/etc/passwd") != -13 || unlink("/bin/hello") != -13) return 6;
    if (mkdir("/etc/evil") != -13) return 7;
    if (open("/etc/new", O_CREAT | O_WRONLY) != -13) return 8;     /* no write on /etc */
    if (mkdir("/tmp/udir") != 0) return 9;                         /* /tmp is open to all */
    fd = open("/tmp/ufile", O_CREAT | O_WRONLY);
    if (fd < 3 || write(fd, "x", 1) != 1) return 10;
    close(fd);
    if (stat("/tmp/ufile", &st) != 0 || st.uid != 1000 || st.gid != 1000 || st.mode != 0644) return 11;
    if (spawn("/tmp/ufile", 0) != -13) return 12;                  /* no execute bit */
    if (chmod("/tmp/ufile", 0600) != 0) return 13;                 /* owner may chmod */
    if (stat("/tmp/ufile", &st) != 0 || st.mode != 0600) return 14;
    if (chmod("/etc/passwd", 0777) != -1) return 15;               /* not the owner */
    if (chown("/tmp/ufile", 0, 0) != -1) return 16;                /* chown is root-only */
    if (kill(parse(args)) != -1) return 17;                        /* someone else's process */
    if (setuid(0) != -1 || setgid(0) != -1) return 18;             /* no privilege escalation */
    if (power(0) != -1 || power(1) != -1) return 19;               /* only root halts the box */
    if (spawn("/bin/hello", 0) < 0) return 20;                     /* but x on /bin is fine */
    {
        int st2 = 0;
        if (wait(-1, &st2) != -10) { /* ECHILD for a pid that is not ours */ }
    }
    unlink("/tmp/ufile");
    unlink("/tmp/udir");
    return 0;
}
