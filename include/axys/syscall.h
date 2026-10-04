#ifndef AXYS_SYSCALL_H
#define AXYS_SYSCALL_H

#include "axys/types.h"

/*
 * System call ABI (x86-64 SYSCALL/SYSRET), Linux-like register convention:
 *   rax = number; args in rdi, rsi, rdx, r10, r8, r9; result in rax
 *   (negative values are -errno). rcx and r11 are clobbered by the CPU;
 *   every other register is preserved.
 */
enum axys_syscall_number {
    AXYS_SYS_EXIT = 0,      /* exit(code)                         */
    AXYS_SYS_WRITE = 1,     /* write(fd, buf, len)                */
    AXYS_SYS_READ = 2,      /* read(fd, buf, len)                 */
    AXYS_SYS_OPEN = 3,      /* open(path, flags) -> fd            */
    AXYS_SYS_CLOSE = 4,     /* close(fd)                          */
    AXYS_SYS_LSEEK = 5,     /* lseek(fd, offset, whence)          */
    AXYS_SYS_SLEEP_MS = 6,  /* sleep_ms(ms)                       */
    AXYS_SYS_GETPID = 7,    /* getpid()                           */
    AXYS_SYS_GETRANDOM = 8, /* getrandom(buf, len)                */
    AXYS_SYS_YIELD = 9,     /* yield()                            */
    AXYS_SYS_UPTIME_MS = 10,/* uptime_ms()                        */
    AXYS_SYS_SBRK = 11,     /* sbrk(delta) -> previous break      */
    AXYS_SYS_SPAWN = 12,    /* spawn(path, args) -> pid           */
    AXYS_SYS_WAIT = 13,     /* wait(pid, &status) -> pid          */
    AXYS_SYS_MKDIR = 14,    /* mkdir(path)                        */
    AXYS_SYS_UNLINK = 15,   /* unlink(path)                       */
    AXYS_SYS_READDIR = 16,  /* readdir(path, index, buf, size)    */
    AXYS_SYS_STAT = 17,     /* stat(path, &struct axys_stat)      */
    AXYS_SYS_POWER = 18,    /* power(0 = off, 1 = reboot)         */
    AXYS_SYS_KILL = 19,     /* kill(pid)                          */
    AXYS_SYS_GETUID = 20,   /* getuid()                           */
    AXYS_SYS_GETGID = 21,   /* getgid()                           */
    AXYS_SYS_SETUID = 22,   /* setuid(uid)   - root only          */
    AXYS_SYS_SETGID = 23,   /* setgid(gid)   - root only          */
    AXYS_SYS_CHMOD = 24,    /* chmod(path, mode) - owner or root  */
    AXYS_SYS_CHOWN = 25,    /* chown(path, uid, gid) - root only  */
    AXYS_SYS_SYNC = 26,     /* sync() - write the file system to disk */
    AXYS_SYS_RENAME = 27,   /* rename(from, to) - move within the VFS */
    AXYS_SYS_RMDIR_TREE = 28, /* rmdir_tree(path) - recursive remove (rm -rf) */
    AXYS_SYS_COUNT
};

struct axys_stat {
    axys_uint32_t type; /* 1 = file, 2 = directory */
    axys_uint32_t size;
    axys_uint32_t mode; /* permission bits */
    axys_uint32_t uid;
    axys_uint32_t gid;
};

/* Program the SYSCALL MSRs. Call once, after the GDT is loaded. */
void axys_syscall_init(void);

/* Power control (also used by the syscall). Never returns on success. */
AXYS_NORETURN void axys_power_off(void);
AXYS_NORETURN void axys_reboot(void);

#endif
