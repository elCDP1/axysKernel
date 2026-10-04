#include "axys/syscall.h"
#include "axys/acpi.h"
#include "axys/console.h"
#include "axys/cpu.h"
#include "axys/disk.h"
#include "axys/gdt.h"
#include "axys/input.h"
#include "axys/io.h"
#include "axys/pit.h"
#include "axys/printf.h"
#include "axys/persist.h"
#include "axys/process.h"
#include "axys/random.h"
#include "axys/sched.h"
#include "axys/string.h"
#include "axys/vfs.h"

extern void axys_syscall_entry(void);

/* Mirrors the push order in arch/x86_64/syscall.S (lowest address first). */
struct axys_syscall_frame {
    axys_uint64_t r15, r14, r13, r12, rbp, rbx;
    axys_uint64_t r9, r8, r10, rdx, rsi, rdi;
    axys_uint64_t rax;
    axys_uint64_t rflags;
    axys_uint64_t rip;
    axys_uint64_t rsp;
};

#define MSR_STAR 0xc0000081u
#define MSR_LSTAR 0xc0000082u
#define MSR_SFMASK 0xc0000084u
/* TF | IF | DF | NT | RF | AC: entered with all of these clear. */
#define SYSCALL_FLAG_MASK 0x54700u

#define IO_CHUNK 256u
#define MAX_PATH AXYS_VFS_MAX_PATH

void axys_syscall_init(void)
{
    axys_cpu_write_msr(AXYS_MSR_EFER, axys_cpu_read_msr(AXYS_MSR_EFER) | AXYS_EFER_SCE);
    axys_cpu_write_msr(MSR_STAR, ((axys_uint64_t)0x10 << 48) | ((axys_uint64_t)AXYS_SEL_KERNEL_CODE << 32));
    axys_cpu_write_msr(MSR_LSTAR, (axys_uint64_t)(axys_uintptr_t)axys_syscall_entry);
    axys_cpu_write_msr(MSR_SFMASK, SYSCALL_FLAG_MASK);
}

static axys_int64_t err(int e)
{
    return -(axys_int64_t)e;
}

static struct axys_fd *fd_get_raw(struct axys_process *proc, axys_uint64_t fd)
{
    if (fd >= AXYS_MAX_FDS || !proc->fds[fd].used) {
        return AXYS_NULL;
    }
    return &proc->fds[fd];
}

/* A descriptor whose file was unlinked (and whose VFS slot may have been reused
 * by an unrelated file) is dead: report EBADF rather than touching that slot.
 * close() uses fd_get_raw so a dead descriptor can still be released. */
static struct axys_fd *fd_get(struct axys_process *proc, axys_uint64_t fdn)
{
    struct axys_fd *fd = fd_get_raw(proc, fdn);

    if (fd != AXYS_NULL && !fd->console && axys_vfs_generation(fd->node) != fd->node_gen) {
        return AXYS_NULL;
    }
    return fd;
}

static axys_int64_t user_path(struct axys_process *proc, axys_uint64_t ptr, char *out)
{
    if (axys_aspace_copy_string(&proc->space, out, ptr, MAX_PATH) < 0) {
        return err(AXYS_EFAULT);
    }
    return 0;
}

/* The VFS reports every create/remove failure as a single -1, which would surface
 * to userspace as a flat ENOSPC. Re-walk the path so the caller is told what
 * actually went wrong: a missing component, a non-directory in the middle, a
 * name that cannot fit in one entry, or a genuinely full node table. */
static int path_errno(const char *path)
{
    char partial[MAX_PATH];
    axys_size_t len = axys_strlen(path);
    axys_size_t slash = 0;

    for (axys_size_t i = 1; i < len; ++i) {
        if (path[i] != '/') {
            continue;
        }
        axys_memcpy(partial, path, i);
        partial[i] = '\0';
        axys_vfs_node_t dir = axys_vfs_lookup(partial);
        if (dir < 0) {
            return AXYS_ENOENT;
        }
        if (axys_vfs_type(dir) != AXYS_VFS_DIR) {
            return AXYS_ENOTDIR;
        }
    }
    for (axys_size_t i = 0; i < len; ++i) {
        if (path[i] == '/') {
            slash = i;
        }
    }
    if (len - slash - 1u > AXYS_VFS_NAME_MAX) {
        return AXYS_ENAMETOOLONG;
    }
    if (axys_vfs_lookup(path) >= 0) {
        return AXYS_EEXIST;
    }
    return AXYS_ENOSPC;
}

/* Every directory on the way to `path` must be searchable (x) by the caller. */
static axys_int64_t search_check(struct axys_process *proc, const char *path)
{
    char prefix[MAX_PATH];
    axys_size_t len = axys_strlen(path);

    if (axys_vfs_access(0, proc->uid, proc->gid, AXYS_PERM_X) != 0) {
        return err(AXYS_EACCES);
    }
    for (axys_size_t i = 1; i < len; ++i) {
        axys_vfs_node_t dir;

        if (path[i] != '/') {
            continue;
        }
        axys_memcpy(prefix, path, i);
        prefix[i] = '\0';
        dir = axys_vfs_lookup(prefix);
        if (dir < 0) {
            return err(AXYS_ENOENT);
        }
        if (axys_vfs_type(dir) != AXYS_VFS_DIR) {
            return err(AXYS_ENOTDIR);
        }
        if (axys_vfs_access(dir, proc->uid, proc->gid, AXYS_PERM_X) != 0) {
            return err(AXYS_EACCES);
        }
    }
    return 0;
}

/* Creating or removing an entry needs write + search permission on its parent. */
static axys_int64_t parent_check(struct axys_process *proc, const char *path, axys_uint32_t want)
{
    char parent[MAX_PATH];
    axys_size_t len = axys_strlen(path);
    axys_size_t slash = 0;
    axys_vfs_node_t dir;

    for (axys_size_t i = 0; i < len; ++i) {
        if (path[i] == '/') {
            slash = i;
        }
    }
    if (slash == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        axys_memcpy(parent, path, slash);
        parent[slash] = '\0';
    }
    dir = axys_vfs_lookup(parent);
    if (dir < 0) {
        return err(AXYS_ENOENT);
    }
    if (axys_vfs_type(dir) != AXYS_VFS_DIR) {
        return err(AXYS_ENOTDIR);
    }
    return axys_vfs_access(dir, proc->uid, proc->gid, want) == 0 ? 0 : err(AXYS_EACCES);
}

static axys_int64_t sys_write(struct axys_process *proc, axys_uint64_t fdn, axys_uint64_t buf, axys_uint64_t len)
{
    struct axys_fd *fd = fd_get(proc, fdn);
    axys_uint64_t done = 0;

    if (fd == AXYS_NULL || !fd->writable) {
        return err(AXYS_EBADF);
    }
    if (len > (1u << 20)) {
        len = 1u << 20;
    }
    while (done < len) {
        char chunk[IO_CHUNK];
        axys_size_t n = len - done < IO_CHUNK ? (axys_size_t)(len - done) : IO_CHUNK;

        if (axys_aspace_copy_from(&proc->space, chunk, buf + done, n) != 0) {
            return done != 0 ? (axys_int64_t)done : err(AXYS_EFAULT);
        }
        if (fd->console) {
            axys_console_write_len(chunk, n);
        } else {
            axys_uint64_t at = (fd->flags & AXYS_O_APPEND) ? (axys_uint64_t)axys_vfs_size(fd->node) : fd->offset;

            if (axys_vfs_pwrite(fd->node, at, chunk, n) < 0) {
                return done != 0 ? (axys_int64_t)done : err(AXYS_ENOSPC);
            }
            fd->offset = at + n;
        }
        done += n;
    }
    return (axys_int64_t)done;
}

/* Canonical-mode console input: read a whole line (with echo and backspace),
 * hand it out byte by byte. Ctrl-D on an empty line is end of file. */
static axys_int64_t console_read(struct axys_process *proc, axys_uint64_t buf, axys_uint64_t len)
{
    axys_uint64_t n;

    if (proc->line_pos >= proc->line_len) {
        int oversize = 0; /* one discarded-remainder flag per over-long line */

        proc->line_len = proc->line_pos = 0;
        for (;;) {
            int c = axys_input_getc();

            if (c == 4 && proc->line_len == 0) {
                return 0; /* EOF */
            }
            if (c == '\n' || c == 4) {
                /* A line that overflowed the editor buffer had its extra
                 * keystrokes silently dropped while still echoing them: the
                 * shell would run a truncated command with no indication, and
                 * a truncated argument can turn "rm -rf /a/b" into "rm -rf /a".
                 * Discard the rest of the physical line without echoing, then
                 * report the error to the user instead of delivering the
                 * partial line. */
                if (oversize) {
                    /* Drop the partial line entirely and hand the reader an
                     * empty one, so the shell prints its prompt again. Not
                     * resetting line_len left the buffer full, which made
                     * every later line overflow too: the shell was wedged. */
                    axys_console_write("line too long\n");
                    proc->line_len = 0;
                    proc->line[proc->line_len++] = '\n';
                    break;
                }
                axys_console_putc('\n');
                if (c == '\n') {
                    proc->line[proc->line_len++] = '\n';
                }
                break;
            }
            if (c == '\b') {
                if (!oversize && proc->line_len > 0) {
                    --proc->line_len;
                    axys_console_write("\b \b");
                }
                continue;
            }
            if (c >= 0x20 && c < 0x7f) {
                if (proc->line_len < sizeof(proc->line) - 1) {
                    proc->line[proc->line_len++] = (char)c;
                    axys_console_putc((char)c);
                } else {
                    oversize = 1;
                }
            }
        }
    }
    n = proc->line_len - proc->line_pos;
    if (n > len) {
        n = len;
    }
    if (axys_aspace_copy_to(&proc->space, buf, proc->line + proc->line_pos, n) != 0) {
        return err(AXYS_EFAULT);
    }
    proc->line_pos += (axys_uint32_t)n;
    return (axys_int64_t)n;
}

static axys_int64_t sys_read(struct axys_process *proc, axys_uint64_t fdn, axys_uint64_t buf, axys_uint64_t len)
{
    struct axys_fd *fd = fd_get(proc, fdn);
    axys_uint64_t done = 0;

    if (fd == AXYS_NULL || !fd->readable) {
        return err(AXYS_EBADF);
    }
    if (axys_aspace_check(&proc->space, buf, len < (1u << 20) ? len : (1u << 20), 1) != 0) {
        return err(AXYS_EFAULT); /* before any blocking */
    }
    if (fd->console) {
        return console_read(proc, buf, len);
    }
    if (len > (1u << 20)) {
        len = 1u << 20;
    }
    while (done < len) {
        char chunk[IO_CHUNK];
        axys_size_t want = len - done < IO_CHUNK ? (axys_size_t)(len - done) : IO_CHUNK;
        axys_int32_t got = axys_vfs_pread(fd->node, fd->offset, chunk, want);

        if (got < 0) {
            return err(AXYS_EISDIR);
        }
        if (got == 0) {
            break;
        }
        if (axys_aspace_copy_to(&proc->space, buf + done, chunk, (axys_size_t)got) != 0) {
            return done != 0 ? (axys_int64_t)done : err(AXYS_EFAULT);
        }
        fd->offset += (axys_uint64_t)got;
        done += (axys_uint64_t)got;
    }
    return (axys_int64_t)done;
}

static axys_int64_t sys_open(struct axys_process *proc, axys_uint64_t path_ptr, axys_uint64_t flags)
{
    char path[MAX_PATH];
    axys_vfs_node_t node;
    int writable = (flags & (AXYS_O_WRONLY | AXYS_O_RDWR)) != 0;
    int readable = (flags & AXYS_O_WRONLY) == 0;
    axys_int64_t rc = user_path(proc, path_ptr, path);

    if (rc != 0) {
        return rc;
    }
    if ((flags & (AXYS_O_TRUNC | AXYS_O_APPEND)) && !writable) {
        return err(AXYS_EINVAL);
    }
    rc = search_check(proc, path);
    if (rc != 0) {
        return rc;
    }
    node = axys_vfs_lookup(path);
    if (node < 0) {
        if (!(flags & AXYS_O_CREAT)) {
            return err(AXYS_ENOENT);
        }
        rc = parent_check(proc, path, AXYS_PERM_W | AXYS_PERM_X);
        if (rc != 0) {
            return rc;
        }
        node = axys_vfs_create_as(path, AXYS_VFS_FILE, 0644u, proc->uid, proc->gid);
        if (node < 0) {
            return err(path_errno(path)); /* name too long, bad component or table full */
        }
    } else if (axys_vfs_type(node) != AXYS_VFS_FILE) {
        return err(AXYS_EISDIR);
    } else {
        axys_uint32_t want = (readable ? AXYS_PERM_R : 0u) | (writable ? AXYS_PERM_W : 0u);

        if (axys_vfs_access(node, proc->uid, proc->gid, want) != 0) {
            return err(AXYS_EACCES);
        }
        if (flags & AXYS_O_TRUNC) {
            (void)axys_vfs_write(node, "", 0);
        }
    }
    for (int i = 3; i < AXYS_MAX_FDS; ++i) {
        if (!proc->fds[i].used) {
            proc->fds[i].used = 1;
            proc->fds[i].console = 0;
            proc->fds[i].readable = readable;
            proc->fds[i].writable = writable;
            proc->fds[i].node = node;
            proc->fds[i].node_gen = axys_vfs_generation(node);
            proc->fds[i].offset = 0;
            proc->fds[i].flags = (axys_uint32_t)flags;
            return i;
        }
    }
    return err(AXYS_EMFILE);
}

static axys_int64_t sys_lseek(struct axys_process *proc, axys_uint64_t fdn, axys_int64_t off, axys_uint64_t whence)
{
    struct axys_fd *fd = fd_get(proc, fdn);
    axys_int64_t base;
    axys_int64_t target;

    if (fd == AXYS_NULL || fd->console) {
        return err(AXYS_EBADF);
    }
    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = (axys_int64_t)fd->offset;
    } else if (whence == 2) {
        base = axys_vfs_size(fd->node);
    } else {
        return err(AXYS_EINVAL);
    }
    /* Range-check before adding: a hostile `off` near INT64_MAX/MIN would make
     * base + off signed overflow, which is undefined behaviour. */
    if (off < -base || off > (axys_int64_t)AXYS_VFS_MAX_FILE_BYTES - base) {
        return err(AXYS_EINVAL);
    }
    target = base + off;
    fd->offset = (axys_uint64_t)target;
    return target;
}

static axys_int64_t sys_sbrk(struct axys_process *proc, axys_int64_t delta)
{
    axys_uint64_t old = proc->brk;
    axys_uint64_t old_top = (old + 4095u) & ~4095ULL;
    axys_uint64_t new_brk;
    axys_uint64_t new_top;

    if (delta < 0 || (axys_uint64_t)delta > AXYS_USER_HEAP_LIMIT) {
        return err(AXYS_EINVAL); /* shrinking is not supported */
    }
    new_brk = old + (axys_uint64_t)delta;
    if (new_brk - proc->brk_start > AXYS_USER_HEAP_LIMIT) {
        return err(AXYS_ENOMEM);
    }
    new_top = (new_brk + 4095u) & ~4095ULL;
    if (new_top > old_top &&
        axys_aspace_map_zeroed(&proc->space, old_top, new_top - old_top, AXYS_MAP_WRITE) != 0) {
        return err(AXYS_ENOMEM);
    }
    proc->brk = new_brk;
    return (axys_int64_t)old;
}

static axys_int64_t sys_readdir(struct axys_process *proc, axys_uint64_t path_ptr, axys_uint64_t index,
                                axys_uint64_t buf, axys_uint64_t size)
{
    char path[MAX_PATH];
    axys_vfs_node_t dir;
    axys_vfs_node_t child = -1;
    axys_int64_t rc = user_path(proc, path_ptr, path);

    if (rc != 0) {
        return rc;
    }
    rc = search_check(proc, path);
    if (rc != 0) {
        return rc;
    }
    dir = axys_vfs_lookup(path);
    if (dir < 0) {
        return err(AXYS_ENOENT);
    }
    if (axys_vfs_type(dir) != AXYS_VFS_DIR) {
        return err(AXYS_ENOTDIR);
    }
    if (axys_vfs_access(dir, proc->uid, proc->gid, AXYS_PERM_R) != 0) {
        return err(AXYS_EACCES);
    }
    for (axys_uint64_t i = 0; i <= index; ++i) {
        child = axys_vfs_next_child(dir, child);
        if (child < 0) {
            return 0; /* end of directory */
        }
    }
    {
        const char *name = axys_vfs_name(child);
        axys_size_t len = axys_strlen(name) + 1;

        if (size < len || axys_aspace_copy_to(&proc->space, buf, name, len) != 0) {
            return err(size < len ? AXYS_EINVAL : AXYS_EFAULT);
        }
        return axys_vfs_type(child) == AXYS_VFS_DIR ? 2 : 1;
    }
}

axys_int64_t axys_syscall_dispatch_inner(struct axys_process *proc, struct axys_syscall_frame *f)
{
    axys_uint64_t a0 = f->rdi;
    axys_uint64_t a1 = f->rsi;
    axys_uint64_t a2 = f->rdx;
    char path[MAX_PATH];
    axys_int64_t rc;

    switch (f->rax) {
    case AXYS_SYS_EXIT:
        axys_process_exit((int)a0);
    case AXYS_SYS_WRITE:
        return sys_write(proc, a0, a1, a2);
    case AXYS_SYS_READ:
        return sys_read(proc, a0, a1, a2);
    case AXYS_SYS_OPEN:
        return sys_open(proc, a0, a1);
    case AXYS_SYS_CLOSE:
        if (fd_get_raw(proc, a0) == AXYS_NULL) {
            return err(AXYS_EBADF);
        }
        proc->fds[a0].used = a0 < 3; /* the std streams stay open */
        return 0;
    case AXYS_SYS_LSEEK:
        return sys_lseek(proc, a0, (axys_int64_t)a1, a2);
    case AXYS_SYS_SLEEP_MS:
        axys_task_sleep_ms(a0 > 60000 ? 60000 : a0);
        return 0;
    case AXYS_SYS_GETPID:
        return proc->pid;
    case AXYS_SYS_GETRANDOM: {
        axys_uint8_t chunk[IO_CHUNK];
        axys_uint64_t done = 0;

        if (a1 > (1u << 16)) {
            a1 = 1u << 16;
        }
        while (done < a1) {
            axys_size_t n = a1 - done < IO_CHUNK ? (axys_size_t)(a1 - done) : IO_CHUNK;

            if (axys_random_bytes(chunk, n) != 0) {
                return err(AXYS_EAGAIN);
            }
            if (axys_aspace_copy_to(&proc->space, a0 + done, chunk, n) != 0) {
                return err(AXYS_EFAULT);
            }
            done += n;
        }
        axys_memset(chunk, 0, sizeof(chunk));
        return (axys_int64_t)done;
    }
    case AXYS_SYS_YIELD:
        axys_yield();
        return 0;
    case AXYS_SYS_UPTIME_MS:
        return (axys_int64_t)(axys_pit_ticks() * 1000u / axys_pit_hz());
    case AXYS_SYS_SBRK:
        return sys_sbrk(proc, (axys_int64_t)a0);
    case AXYS_SYS_SPAWN: {
        char args[AXYS_ARGS_MAX];

        rc = user_path(proc, a0, path);
        if (rc != 0) {
            return rc;
        }
        args[0] = '\0';
        if (a1 != 0 && axys_aspace_copy_string(&proc->space, args, a1, sizeof(args)) < 0) {
            return err(AXYS_EFAULT);
        }
        rc = search_check(proc, path);
        if (rc != 0) {
            return rc;
        }
        {
            axys_vfs_node_t exe = axys_vfs_lookup(path);

            if (exe < 0) {
                return err(AXYS_ENOENT);
            }
            if (axys_vfs_type(exe) != AXYS_VFS_FILE) {
                return err(AXYS_EISDIR);
            }
            if (axys_vfs_access(exe, proc->uid, proc->gid, AXYS_PERM_X) != 0) {
                return err(AXYS_EACCES); /* no execute permission */
            }
        }
        return axys_process_spawn_as(path, args, proc->pid, proc->uid, proc->gid);
    }
    case AXYS_SYS_WAIT: {
        int status = 0;

        rc = axys_process_wait((int)a0, proc->pid, &status);
        if (rc < 0) {
            return rc;
        }
        if (a1 != 0 && axys_aspace_copy_to(&proc->space, a1, &status, sizeof(status)) != 0) {
            return err(AXYS_EFAULT);
        }
        return rc;
    }
    case AXYS_SYS_MKDIR:
        rc = user_path(proc, a0, path);
        if (rc != 0) {
            return rc;
        }
        rc = search_check(proc, path);
        if (rc != 0) {
            return rc;
        }
        if (axys_vfs_lookup(path) >= 0) {
            return err(AXYS_EEXIST);
        }
        rc = parent_check(proc, path, AXYS_PERM_W | AXYS_PERM_X);
        if (rc != 0) {
            return rc;
        }
        /* mkdir -p semantics: create every missing ancestor, not just the
         * final component. Each intermediate directory is owned by the caller
         * with mode 0755, exactly like the leaf. Trailing slashes are
         * normalized away first so "/a/b/" behaves like "/a/b" instead of
         * trying to create an empty-named leaf (which would fail with ENOENT
         * after /a/b already exists). */
        {
            axys_size_t len = axys_strlen(path);

            while (len > 1 && path[len - 1] == '/') {
                path[--len] = '\0';
            }
            for (axys_size_t i = 1; i < len; ++i) {
                char saved;

                if (path[i] != '/') {
                    continue;
                }
                saved = path[i];
                path[i] = '\0';
                if (axys_vfs_lookup(path) < 0 &&
                    axys_vfs_create_as(path, AXYS_VFS_DIR, 0755u, proc->uid, proc->gid) < 0) {
                    path[i] = saved;
                    return err(path_errno(path));
                }
                path[i] = saved;
            }
        }
        return axys_vfs_create_as(path, AXYS_VFS_DIR, 0755u, proc->uid, proc->gid) >= 0 ? 0 : err(path_errno(path));
    case AXYS_SYS_UNLINK: {
        axys_vfs_node_t node;
        axys_uint32_t type;

        rc = user_path(proc, a0, path);
        if (rc != 0) {
            return rc;
        }
        rc = search_check(proc, path);
        if (rc != 0) {
            return rc;
        }
        node = axys_vfs_lookup(path);
        if (node < 0) {
            return err(AXYS_ENOENT);
        }
        rc = parent_check(proc, path, AXYS_PERM_W | AXYS_PERM_X);
        if (rc != 0) {
            return rc;
        }
        type = axys_vfs_type(node);
        if (axys_vfs_unlink(path) == 0) {
            return 0; /* empty directories are removable, as the contract states */
        }
        if (type == AXYS_VFS_DIR) {
            return err(AXYS_ENOTEMPTY);
        }
        return err(path_errno(path));
    }
    case AXYS_SYS_READDIR:
        return sys_readdir(proc, a0, a1, a2, f->r10);
    case AXYS_SYS_STAT: {
        struct axys_stat st;
        struct axys_vfs_attr attr;
        axys_vfs_node_t node;

        rc = user_path(proc, a0, path);
        if (rc != 0) {
            return rc;
        }
        rc = search_check(proc, path);
        if (rc != 0) {
            return rc;
        }
        node = axys_vfs_lookup(path);
        if (node < 0 || axys_vfs_getattr(node, &attr) != 0) {
            return err(AXYS_ENOENT);
        }
        st.type = attr.type;
        st.size = attr.size;
        st.mode = attr.mode;
        st.uid = attr.uid;
        st.gid = attr.gid;
        return axys_aspace_copy_to(&proc->space, a1, &st, sizeof(st)) == 0 ? 0 : err(AXYS_EFAULT);
    }
    case AXYS_SYS_KILL:
        return axys_process_kill_checked((int)a0, proc->uid);
    case AXYS_SYS_GETUID:
        return proc->uid;
    case AXYS_SYS_GETGID:
        return proc->gid;
    case AXYS_SYS_SETUID:
        if (proc->uid != 0) {
            return err(AXYS_EPERM);
        }
        proc->uid = (axys_uint32_t)a0;
        return 0;
    case AXYS_SYS_SETGID:
        if (proc->uid != 0) {
            return err(AXYS_EPERM);
        }
        proc->gid = (axys_uint32_t)a0;
        return 0;
    case AXYS_SYS_CHMOD: {
        axys_vfs_node_t node;
        struct axys_vfs_attr attr;

        rc = user_path(proc, a0, path);
        if (rc != 0) {
            return rc;
        }
        rc = search_check(proc, path);
        if (rc != 0) {
            return rc;
        }
        node = axys_vfs_lookup(path);
        if (node < 0 || axys_vfs_getattr(node, &attr) != 0) {
            return err(AXYS_ENOENT);
        }
        if (proc->uid != 0 && proc->uid != attr.uid) {
            return err(AXYS_EPERM);
        }
        return axys_vfs_chmod(node, (axys_uint32_t)a1) == 0 ? 0 : err(AXYS_ENOENT);
    }
    case AXYS_SYS_CHOWN: {
        axys_vfs_node_t node;

        if (proc->uid != 0) {
            return err(AXYS_EPERM);
        }
        rc = user_path(proc, a0, path);
        if (rc != 0) {
            return rc;
        }
        node = axys_vfs_lookup(path);
        if (node < 0) {
            return err(AXYS_ENOENT);
        }
        return axys_vfs_chown(node, (axys_uint32_t)a1, (axys_uint32_t)a2) == 0 ? 0 : err(AXYS_ENOENT);
    }
    case AXYS_SYS_SYNC:
        return axys_persist_sync() == 0 ? 0 : err(AXYS_ENODEV);
    case AXYS_SYS_RENAME: {
        char to[MAX_PATH];
        axys_vfs_node_t src;

        rc = user_path(proc, a0, path);
        if (rc != 0) {
            return rc;
        }
        rc = user_path(proc, a1, to);
        if (rc != 0) {
            return rc;
        }
        rc = search_check(proc, path);
        if (rc != 0) {
            return rc;
        }
        rc = search_check(proc, to);
        if (rc != 0) {
            return rc;
        }
        src = axys_vfs_lookup(path);
        if (src < 0) {
            return err(AXYS_ENOENT);
        }
        /* permission on the source itself: renaming rewrites its parent's
         * entry and the node's name, so it follows POSIX rename(): the caller
         * must own the file (or be root) for the object being moved. */
        {
            struct axys_vfs_attr attr;

            if (axys_vfs_getattr(src, &attr) != 0) {
                return err(AXYS_ENOENT);
            }
            if (proc->uid != 0 && proc->uid != attr.uid) {
                return err(AXYS_EPERM);
            }
        }
        rc = parent_check(proc, path, AXYS_PERM_W | AXYS_PERM_X);
        if (rc != 0) {
            return rc;
        }
        rc = parent_check(proc, to, AXYS_PERM_W | AXYS_PERM_X);
        if (rc != 0) {
            return rc;
        }
        /* a file cannot be renamed onto an existing directory: report EISDIR
         * instead of the generic failure the VFS layer returns. */
        {
            axys_vfs_node_t dst = axys_vfs_lookup(to);

            if (dst >= 0 && axys_vfs_type(dst) == AXYS_VFS_DIR &&
                axys_vfs_type(src) != AXYS_VFS_DIR) {
                return err(AXYS_EISDIR);
            }
        }
        return axys_vfs_rename(path, to) == 0 ? 0 : err(AXYS_EINVAL);
    }
    case AXYS_SYS_RMDIR_TREE: {
        axys_vfs_node_t node;

        rc = user_path(proc, a0, path);
        if (rc != 0) {
            return rc;
        }
        rc = search_check(proc, path);
        if (rc != 0) {
            return rc;
        }
        node = axys_vfs_lookup(path);
        if (node < 0) {
            return err(AXYS_ENOENT);
        }
        rc = parent_check(proc, path, AXYS_PERM_W | AXYS_PERM_X);
        if (rc != 0) {
            return rc;
        }
        {
            axys_int32_t removed = axys_vfs_remove_tree(path);

            return removed >= 0 ? removed : err(AXYS_EACCES);
        }
    }
    case AXYS_SYS_POWER:
        if (proc->uid != 0) {
            return err(AXYS_EPERM); /* only root may halt the machine */
        }
        if (a0 > 1) {
            return err(AXYS_EINVAL); /* only 0 = off and 1 = reboot exist */
        }
        (void)axys_persist_sync(); /* never lose data on an orderly shutdown */
        if (a0 == 1) {
            axys_reboot();
        }
        axys_power_off();
    default:
        return err(AXYS_ENOSYS);
    }
}

void axys_syscall_dispatch(struct axys_syscall_frame *frame)
{
    struct axys_process *proc = axys_process_current();

    if (proc == AXYS_NULL) {
        frame->rax = (axys_uint64_t)err(AXYS_ENOSYS); /* SYSCALL from a task with no process */
        return;
    }
    frame->rax = (axys_uint64_t)axys_syscall_dispatch_inner(proc, frame);
    axys_process_check_kill(); /* a kill that arrived while we were blocked */
}

/* ---- power control ------------------------------------------------- */

/* Register addresses and sleep types come from validated firmware tables.
 * Unsupported ACPI paths halt safely instead of writing guessed chipset ports.
 * Flushing first matters: a snapshot that reached storage before the machine
 * goes down is the difference between the next boot restoring state and
 * starting from nothing. */

AXYS_NORETURN void axys_power_off(void)
{
    axys_cpu_disable_interrupts();
    (void)axys_disk_flush();
    axys_printf("axys: powering off\n");
    axys_acpi_poweroff();
}

AXYS_NORETURN void axys_reboot(void)
{
    axys_cpu_disable_interrupts();
    (void)axys_disk_flush();
    axys_printf("axys: rebooting\n");
    axys_acpi_reboot();
}
