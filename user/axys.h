#ifndef AXYS_USER_H
#define AXYS_USER_H

/* Minimal freestanding user-space runtime for axysOS programs. */

typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

#define O_CREAT 0x1
#define O_TRUNC 0x2
#define O_APPEND 0x4
#define O_WRONLY 0x8
#define O_RDWR 0x10

enum {
    SYS_EXIT, SYS_WRITE, SYS_READ, SYS_OPEN, SYS_CLOSE, SYS_LSEEK, SYS_SLEEP_MS, SYS_GETPID,
    SYS_GETRANDOM, SYS_YIELD, SYS_UPTIME_MS, SYS_SBRK, SYS_SPAWN, SYS_WAIT, SYS_MKDIR,
    SYS_UNLINK, SYS_READDIR, SYS_STAT, SYS_POWER, SYS_KILL, SYS_GETUID, SYS_GETGID, SYS_SETUID,
    SYS_SETGID, SYS_CHMOD, SYS_CHOWN, SYS_SYNC, SYS_RENAME, SYS_RMDIR_TREE, SYS_MEMINFO,
    SYS_NET_SEND, SYS_NET_RECV, SYS_NET_STAT, SYS_NET_SET_ADDR
};

struct meminfo {
    u64 free_frames;
    u64 heap_used;
    u64 heap_free;
    u64 live_nodes;
};

struct net_stat {
    u8 mac[6];
    u8 link;
    u8 pad;
    u32 speed;
    u8 ip[4];
    u8 ip_pad[4];
    u64 tx_packets;
    u64 rx_packets;
    u64 rx_dropped;
};

struct stat_info {
    u32 type; /* 1 file, 2 directory */
    u32 size;
    u32 mode;
    u32 uid;
    u32 gid;
};

static inline long sys4(long n, long a, long b, long c, long d)
{
    long ret;
    register long r10 __asm__("r10") = d;

    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}
#define sys3(n, a, b, c) sys4(n, (long)(a), (long)(b), (long)(c), 0)

__attribute__((noreturn)) static inline void exit(int code)
{
    sys3(SYS_EXIT, code, 0, 0);
    for (;;) {
    }
}
static inline ssize_t write(int fd, const void *buf, size_t len) { return sys3(SYS_WRITE, fd, buf, len); }
static inline ssize_t read(int fd, void *buf, size_t len) { return sys3(SYS_READ, fd, buf, len); }
static inline int open(const char *path, int flags) { return (int)sys3(SYS_OPEN, path, flags, 0); }
static inline int close(int fd) { return (int)sys3(SYS_CLOSE, fd, 0, 0); }
static inline long lseek(int fd, long off, int whence) { return sys3(SYS_LSEEK, fd, off, whence); }
static inline void sleep_ms(u64 ms) { sys3(SYS_SLEEP_MS, ms, 0, 0); }
static inline int getpid(void) { return (int)sys3(SYS_GETPID, 0, 0, 0); }
static inline long getrandom(void *buf, size_t len) { return sys3(SYS_GETRANDOM, buf, len, 0); }
static inline void yield(void) { sys3(SYS_YIELD, 0, 0, 0); }
static inline u64 uptime_ms(void) { return (u64)sys3(SYS_UPTIME_MS, 0, 0, 0); }
static inline void *sbrk(long delta) { return (void *)sys3(SYS_SBRK, delta, 0, 0); }
static inline int spawn(const char *path, const char *args) { return (int)sys3(SYS_SPAWN, path, args, 0); }
static inline int wait(int pid, int *status) { return (int)sys3(SYS_WAIT, pid, status, 0); }
static inline int mkdir(const char *path) { return (int)sys3(SYS_MKDIR, path, 0, 0); }
static inline int unlink(const char *path) { return (int)sys3(SYS_UNLINK, path, 0, 0); }
static inline int readdir(const char *path, u64 index, char *buf, size_t size)
{
    return (int)sys4(SYS_READDIR, (long)path, (long)index, (long)buf, (long)size);
}
static inline int stat(const char *path, struct stat_info *st) { return (int)sys3(SYS_STAT, path, st, 0); }
static inline int kill(int pid) { return (int)sys3(SYS_KILL, pid, 0, 0); }
static inline int getuid(void) { return (int)sys3(SYS_GETUID, 0, 0, 0); }
static inline int getgid(void) { return (int)sys3(SYS_GETGID, 0, 0, 0); }
static inline int setuid(u32 uid) { return (int)sys3(SYS_SETUID, uid, 0, 0); }
static inline int setgid(u32 gid) { return (int)sys3(SYS_SETGID, gid, 0, 0); }
static inline int chmod(const char *path, u32 mode) { return (int)sys3(SYS_CHMOD, path, mode, 0); }
static inline int chown(const char *path, u32 uid, u32 gid) { return (int)sys3(SYS_CHOWN, path, uid, gid); }
static inline int sync(void) { return (int)sys3(SYS_SYNC, 0, 0, 0); }
static inline int power(int reboot) { return (int)sys3(SYS_POWER, reboot, 0, 0); }
static inline int rename(const char *from, const char *to) { return (int)sys3(SYS_RENAME, from, to, 0); }
static inline int rmtree(const char *path) { return (int)sys3(SYS_RMDIR_TREE, path, 0, 0); }
static inline int meminfo(struct meminfo *out) { return (int)sys3(SYS_MEMINFO, out, 0, 0); }
static inline long net_send(const void *buf, size_t len) { return sys3(SYS_NET_SEND, buf, len, 0); }
static inline long net_recv(void *buf, size_t cap) { return sys3(SYS_NET_RECV, buf, cap, 0); }
static inline int net_stat(struct net_stat *out) { return (int)sys3(SYS_NET_STAT, out, 0, 0); }
static inline int net_set_addr(const u8 ip[4]) { return (int)sys3(SYS_NET_SET_ADDR, ip, 0, 0); }

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
void puts(const char *s);          /* no trailing newline */
void put_u64(u64 value);
void put_hex(u64 value);

int main(const char *args, size_t args_len);

#endif
