#ifndef AXYS_PROCESS_H
#define AXYS_PROCESS_H

#include "axys/aspace.h"
#include "axys/interrupts.h"
#include "axys/sched.h"
#include "axys/types.h"

/*
 * User processes: one address space, one task, a small fd table.
 *
 * Programs are ELF64 x86-64 executables (ET_EXEC inside the user window, or
 * position-independent ET_DYN images that are loaded at a randomised base)
 * read from the VFS. The loader enforces W^X, rejects overlapping segments and
 * anything outside the user window, and randomises the load base, heap start
 * and stack top (ASLR).
 */

#define AXYS_MAX_PROCS 32
#define AXYS_MAX_FDS 16
#define AXYS_PROC_NAME_MAX 16
#define AXYS_USER_STACK_PAGES 16u
#define AXYS_USER_HEAP_LIMIT (64ull * 1024 * 1024)
#define AXYS_ARGS_MAX 128
#define AXYS_KERNEL_PARENT (-1) /* spawned from kernel code; kernel waits */

/* Exit status of a process killed by a CPU exception: 128 + vector. */
#define AXYS_EXIT_FAULT_BASE 128

#define AXYS_O_CREAT 0x1u
#define AXYS_O_TRUNC 0x2u
#define AXYS_O_APPEND 0x4u
#define AXYS_O_WRONLY 0x8u
#define AXYS_O_RDWR 0x10u

enum axys_errno {
    AXYS_ENOENT = 2, AXYS_ENOEXEC = 8, AXYS_EBADF = 9, AXYS_ECHILD = 10,
    AXYS_ENOMEM = 12, AXYS_EFAULT = 14, AXYS_EEXIST = 17, AXYS_ENOTDIR = 20,
    AXYS_EISDIR = 21, AXYS_EINVAL = 22, AXYS_EMFILE = 24, AXYS_ENOSPC = 28,
    AXYS_ENOSYS = 38, AXYS_EAGAIN = 11, AXYS_ESRCH = 3, AXYS_EPERM = 1, AXYS_EACCES = 13, AXYS_ENODEV = 19,
    AXYS_ENAMETOOLONG = 36, AXYS_ENOTEMPTY = 39, AXYS_ELOOP = 40, AXYS_EIO = 5,
};

struct axys_fd {
    int used;
    int console;             /* 1: stdin/stdout/stderr */
    int readable;
    int writable;
    axys_int32_t node;       /* VFS node when !console */
    axys_uint32_t node_gen;  /* VFS slot generation recorded at open() */
    axys_uint64_t offset;
    axys_uint32_t flags;
};

struct axys_process {
    int used;
    int pid;
    int parent;              /* pid, AXYS_KERNEL_PARENT, or 0 when orphaned */
    int zombie;
    axys_uint32_t uid;
    axys_uint32_t gid;
    int killed;              /* set by kill/Ctrl-C; honoured on the next kernel entry/exit */
    int exit_code;
    struct axys_aspace space;
    axys_uint64_t brk_start;
    axys_uint64_t brk;
    struct axys_fd fds[AXYS_MAX_FDS];
    char line[256];          /* canonical-mode stdin buffer */
    axys_uint32_t line_len;
    axys_uint32_t line_pos;
    char name[AXYS_PROC_NAME_MAX];
};

void axys_process_init(void);

/* Load `path` and start it. `args` (may be NULL) is passed to the program as
 * (rdi = pointer to a NUL-terminated string on its stack, rsi = length).
 * Returns the new pid, or -errno. */
int axys_process_spawn(const char *path, const char *args, int parent);

/* Same, but the new process runs with the given credentials (kernel code only:
 * user programs always inherit their parent's uid/gid). */
int axys_process_spawn_as(const char *path, const char *args, int parent,
                          axys_uint32_t uid, axys_uint32_t gid);

/* Wait for child `pid` of `parent` to exit; stores its status. Returns pid or
 * -errno (ECHILD if it is not a child of `parent`). Blocks. */
int axys_process_wait(int pid, int parent, int *status);

struct axys_process *axys_process_current(void);

AXYS_NORETURN void axys_process_exit(int code);

/* A CPU exception arrived from ring 3: kill the process, never the kernel.
 * Called from the exception dispatcher; does not return. */
AXYS_NORETURN void axys_process_fault(const struct axys_interrupt_frame *frame);

axys_uint32_t axys_process_count(void);

/* Ask a process to die (SIGKILL-like: cannot be caught). It exits at its next
 * timer interrupt or system-call boundary. Returns 0, or -ESRCH. */
int axys_process_kill(int pid);

/* kill() as seen from a user process: only root, or a process with the same
 * uid, may signal its target. Returns 0, -ESRCH or -EPERM. */
int axys_process_kill_checked(int pid, axys_uint32_t caller_uid);

/* Ctrl-C: kill the youngest running process that was started by another
 * process (the shell's foreground job). Interrupt-safe. Returns 1 if a target
 * existed. */
int axys_process_interrupt_foreground(void);

/* Exit now if this process has been killed. Cheap; called on kernel entries. */
void axys_process_check_kill(void);

#define AXYS_EXIT_KILLED (AXYS_EXIT_FAULT_BASE + 9)

/* Boot-time end-to-end test of ring 3, isolation and cleanup. */
void axys_process_selftest(void);

#endif
