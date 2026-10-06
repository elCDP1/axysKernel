#include "axys.h"

/* File syscalls: create, write, seek, read back, append, stat, readdir, unlink. */
int main(const char *args, size_t len)
{
    char buf[32];
    struct stat_info st;

    (void)args;
    (void)len;
    int fd = open("/tmp/fileio.txt", O_CREAT | O_TRUNC | O_WRONLY);
    if (fd < 3 || write(fd, "hello axys", 10) != 10) {
        return 1;
    }
    if (lseek(fd, 6, 0) != 6 || write(fd, "AXYS", 4) != 4) {
        return 2;
    }
    close(fd);
    fd = open("/tmp/fileio.txt", O_APPEND | O_WRONLY);
    if (fd < 3 || write(fd, "!", 1) != 1) {
        return 3;
    }
    close(fd);
    fd = open("/tmp/fileio.txt", 0);
    memset(buf, 0, sizeof(buf));
    if (fd < 3) {
        return 4;
    }
    /* A zero-length read must return 0 without touching the buffer, so even
     * an unmapped pointer is safe: the kernel returns before validating it. */
    if (read(fd, buf, 0) != 0 || read(fd, (void *)(u64)0x10, 0) != 0) {
        return 8;
    }
    if (read(fd, buf, sizeof(buf)) != 11 || memcmp(buf, "hello AXYS!", 11) != 0) {
        return 4;
    }
    close(fd);
    if (stat("/tmp/fileio.txt", &st) != 0 || st.type != 1 || st.size != 11 || st.mode != 0644 || st.uid != (u32)getuid()) {
        return 5;
    }
    if (open("/tmp/none/x", O_CREAT | O_WRONLY) >= 0 || open("/nope", 0) != -2) {
        return 6;
    }
    int seen = 0;
    for (u64 i = 0; readdir("/tmp", i, buf, sizeof(buf)) > 0; ++i) {
        seen += strcmp(buf, "fileio.txt") == 0;
    }
    if (seen != 1 || unlink("/tmp/fileio.txt") != 0 || stat("/tmp/fileio.txt", &st) != -2) {
        return 7;
    }
    puts("fileio: all checks passed\n");
    return 0;
}
