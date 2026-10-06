#include "axys/process.h"
#include "axys/cpu.h"
#include "axys/elf.h"
#include "axys/heap.h"
#include "axys/panic.h"
#include "axys/pit.h"
#include "axys/pmm.h"
#include "axys/printf.h"
#include "axys/random.h"
#include "axys/string.h"
#include "axys/vfs.h"

extern axys_uint64_t pml4_table[512];
extern void axys_enter_user(axys_uint64_t rip, axys_uint64_t rsp, axys_uint64_t arg0, axys_uint64_t arg1)
    AXYS_NORETURN;

#define PAGE 4096ULL
#define ELF_MAX_BYTES (8u * 1024u * 1024u)
#define ASLR_BASE_PAGES (1u << 16)  /* up to 256 MiB of load-base slide */
#define ASLR_HEAP_PAGES (1u << 12)  /* up to 16 MiB gap before the heap  */
#define ASLR_STACK_PAGES (1u << 18) /* up to 1 GiB of stack slide        */

static struct axys_process procs[AXYS_MAX_PROCS];
static int next_pid = 1;
static int wait_channel;

static axys_uint64_t align_down(axys_uint64_t v) { return v & ~(PAGE - 1); }
static axys_uint64_t align_up(axys_uint64_t v) { return (v + PAGE - 1) & ~(PAGE - 1); }

/* Random page count in [0, limit); zero if the RNG has no entropy yet. */
static axys_uint64_t random_pages(axys_uint32_t limit)
{
    axys_uint32_t value = 0;

    if (axys_random_u32(&value) != 0) {
        return 0;
    }
    return value % limit;
}

/* ET_DYN load bias must preserve every PT_LOAD p_align congruence. ELF
 * linkers commonly use 4 KiB, but a valid image may require a larger page
 * alignment. Since ELF p_align values are powers of two, aligning the bias to
 * the largest segment alignment preserves the smaller ones as well. */
static int choose_load_bias(const axys_uint8_t *image,
                            const struct axys_elf64_header *eh,
                            axys_uint64_t *bias_out)
{
    axys_uint64_t alignment = PAGE;
    axys_uint64_t base = AXYS_USER_BASE + 0x400000ULL;
    axys_uint64_t span = (axys_uint64_t)ASLR_BASE_PAGES * PAGE;
    axys_uint64_t slots;

    for (unsigned i = 0; i < eh->phnum; ++i) {
        struct axys_elf64_phdr ph;

        axys_memcpy(&ph, image + eh->phoff + i * sizeof(ph), sizeof(ph));
        if (ph.type == AXYS_ELF_PT_LOAD && ph.align > alignment) {
            alignment = ph.align;
        }
    }
    if (base % alignment != 0) {
        axys_uint64_t padding = alignment - (base % alignment);

        if (base > ~(axys_uint64_t)0 - padding) {
            return -1;
        }
        base += padding;
    }
    if (base >= AXYS_USER_TOP) {
        return -1;
    }
    slots = span / alignment;
    if (slots == 0) {
        slots = 1;
    }
    *bias_out = base + random_pages((axys_uint32_t)slots) * alignment;
    return *bias_out < AXYS_USER_TOP ? 0 : -1;
}

void axys_process_init(void)
{
    axys_memset(procs, 0, sizeof(procs));
    next_pid = 1;
}

axys_uint32_t axys_process_count(void)
{
    axys_uint32_t n = 0;

    for (int i = 0; i < AXYS_MAX_PROCS; ++i) {
        n += procs[i].used && !procs[i].zombie;
    }
    return n;
}

struct axys_process *axys_process_current(void)
{
    struct axys_task *task = axys_task_current();

    return task != AXYS_NULL ? (struct axys_process *)task->process : AXYS_NULL;
}

static struct axys_process *find_pid(int pid)
{
    for (int i = 0; i < AXYS_MAX_PROCS; ++i) {
        if (procs[i].used && procs[i].pid == pid) {
            return &procs[i];
        }
    }
    return AXYS_NULL;
}

static struct axys_process *slot_alloc(void)
{
    for (int i = 0; i < AXYS_MAX_PROCS; ++i) {
        if (!procs[i].used) {
            axys_memset(&procs[i], 0, sizeof(procs[i]));
            procs[i].used = 1;
            procs[i].pid = next_pid++;
            return &procs[i];
        }
    }
    return AXYS_NULL;
}

/* Static-PIE images carry R_X86_64_RELATIVE relocations for absolute
 * pointers; apply them for the chosen load base. Any other relocation type
 * (symbol lookups need a dynamic linker) rejects the image. */
static int apply_relocations(struct axys_process *proc, const axys_uint8_t *image,
                             const struct axys_elf64_header *eh, axys_uint64_t bias)
{
    axys_uint64_t rela = 0;
    axys_uint64_t relasz = 0;
    axys_uint64_t relaent = 24;
    int dynamic_terminated = 0;
    int dynamic_seen = 0;

    for (unsigned i = 0; i < eh->phnum; ++i) {
        struct axys_elf64_phdr ph;

        axys_memcpy(&ph, image + eh->phoff + i * sizeof(ph), sizeof(ph));
        if (ph.type != AXYS_ELF_PT_DYNAMIC) {
            continue;
        }
        dynamic_seen = 1;
        if (ph.filesz > 4096 || (ph.filesz & 15u) != 0 ||
            ph.vaddr > ~(axys_uint64_t)0 - bias ||
            ph.vaddr + bias > ~(axys_uint64_t)0 - ph.filesz) {
            return -1;
        }
        for (axys_uint64_t off = 0; off + 16 <= ph.filesz; off += 16) {
            axys_uint64_t entry[2];

            if (axys_aspace_copy_from(&proc->space, entry, ph.vaddr + bias + off, 16) != 0) {
                return -1;
            }
            if (entry[0] == 0) {
                dynamic_terminated = 1;
                break;
            }
            if (entry[0] == 7) {
                rela = entry[1];
            } else if (entry[0] == 8) {
                relasz = entry[1];
            } else if (entry[0] == 9) {
                relaent = entry[1];
            } else if (entry[0] == 1 || entry[0] == 2 || entry[0] == 17 ||
                       entry[0] == 18 || entry[0] == 19 || entry[0] == 22 ||
                       entry[0] == 23 || entry[0] == 35 || entry[0] == 36 ||
                       entry[0] == 37) {
                /* Dependencies, PLT/REL/text/RELR relocations require a
                 * dynamic linker or relocation implementation we do not have. */
                return -1;
            }
        }
    }
    if (!dynamic_seen) {
        return 0; /* static PIE with no dynamic metadata has no relocations */
    }
    if (!dynamic_terminated || ((rela == 0) != (relasz == 0))) {
        return -1;
    }
    if (rela == 0) {
        return 0;
    }
    if (relaent != 24 || relasz > (1u << 20) || relasz % relaent != 0 ||
        rela > ~(axys_uint64_t)0 - bias ||
        rela + bias > ~(axys_uint64_t)0 - relasz) {
        return -1;
    }
    for (axys_uint64_t off = 0; off + 24 <= relasz; off += 24) {
        axys_uint64_t r[3]; /* offset, info, addend */
        axys_uint64_t value;

        if (axys_aspace_copy_from(&proc->space, r, rela + bias + off, 24) != 0 ||
            (r[1] >> 32u) != 0 ||
            (r[1] & 0xffffffffu) != 8 /* R_X86_64_RELATIVE */ ||
            r[0] > ~(axys_uint64_t)0 - bias) {
            return -1;
        }
        value = bias + r[2];
        if (axys_aspace_load(&proc->space, bias + r[0], &value, 8) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Returns 0 or -errno. On success fills entry and the highest mapped address. */
static int load_elf(struct axys_process *proc, const axys_uint8_t *image, axys_size_t size,
                    axys_uint64_t *entry_out, axys_uint64_t *end_out)
{
    const struct axys_elf64_header *eh = (const struct axys_elf64_header *)(const void *)image;
    struct axys_elf_image_info elf_info;
    axys_uint64_t bias = 0;
    axys_uint64_t highest = 0;

    if (axys_elf_validate_image(image, size, &elf_info) != 0) {
        return -AXYS_ENOEXEC;
    }
    if (eh->type == AXYS_ELF_ET_DYN) { /* position independent: slide the whole image */
        if (choose_load_bias(image, eh, &bias) != 0) {
            return -AXYS_ENOEXEC;
        }
    }

    for (unsigned i = 0; i < eh->phnum; ++i) {
        struct axys_elf64_phdr ph;
        axys_uint64_t start;
        axys_uint64_t end;
        axys_uint64_t load_address;
        axys_uint64_t raw_end;
        axys_uint32_t flags = 0;

        axys_memcpy(&ph, image + eh->phoff + i * sizeof(ph), sizeof(ph));
        if (ph.type != AXYS_ELF_PT_LOAD || ph.memsz == 0) {
            continue;
        }
        if (ph.vaddr > ~(axys_uint64_t)0 - bias) {
            return -AXYS_ENOEXEC;
        }
        load_address = ph.vaddr + bias;
        if (ph.memsz > ~(axys_uint64_t)0 - load_address) {
            return -AXYS_ENOEXEC;
        }
        raw_end = load_address + ph.memsz;
        if (raw_end > ~(axys_uint64_t)0 - (PAGE - 1u)) {
            return -AXYS_ENOEXEC;
        }
        start = align_down(load_address);
        end = align_up(raw_end);
        if (start < AXYS_USER_BASE || end > AXYS_USER_TOP - 0x10000000ULL || end <= start) {
            return -AXYS_ENOEXEC;
        }
        if (ph.flags & AXYS_ELF_PF_W) {
            flags |= AXYS_MAP_WRITE;
        }
        if (ph.flags & AXYS_ELF_PF_X) {
            flags |= AXYS_MAP_EXEC;
        }
        if (axys_aspace_map_zeroed(&proc->space, start, end - start, flags) != 0) {
            return -AXYS_ENOEXEC; /* overlap or out of memory */
        }
        if (ph.filesz != 0 &&
            axys_aspace_load(&proc->space, load_address, image + ph.offset, ph.filesz) != 0) {
            return -AXYS_ENOEXEC;
        }
        if (end > highest) {
            highest = end;
        }
    }
    if (highest == 0 || elf_info.entry > ~(axys_uint64_t)0 - bias ||
        elf_info.entry + bias < AXYS_USER_BASE || elf_info.entry + bias >= highest) {
        return -AXYS_ENOEXEC;
    }
    if (bias != 0 && apply_relocations(proc, image, eh, bias) != 0) {
        return -AXYS_ENOEXEC;
    }
    *entry_out = elf_info.entry + bias;
    *end_out = highest;
    return 0;
}

struct start_info {
    struct axys_process *proc;
    axys_uint64_t entry;
    axys_uint64_t rsp;
    axys_uint64_t arg_ptr;
    axys_uint64_t arg_len;
};

static void user_task_entry(void *raw)
{
    struct start_info info = *(struct start_info *)raw;

    axys_kfree(raw);
    axys_enter_user(info.entry, info.rsp, info.arg_ptr, info.arg_len);
}

static void install_std_fds(struct axys_process *proc)
{
    for (int i = 0; i < 3; ++i) {
        proc->fds[i].used = 1;
        proc->fds[i].console = 1;
        proc->fds[i].readable = 1;
        proc->fds[i].writable = 1;
    }
}

int axys_process_spawn_as(const char *path, const char *args, int parent,
                          axys_uint32_t uid, axys_uint32_t gid)
{
    axys_uint64_t flags;
    struct axys_process *proc;
    axys_vfs_node_t node;
    axys_int32_t size;
    axys_uint8_t *image;
    axys_uint64_t entry = 0;
    axys_uint64_t end = 0;
    axys_uint64_t stack_top;
    axys_uint64_t rsp;
    axys_uint64_t arg_ptr = 0;
    axys_size_t arg_len = 0;
    struct start_info *info;
    struct axys_task *task;
    int rc;

    node = axys_vfs_lookup(path);
    if (node < 0) {
        return -AXYS_ENOENT;
    }
    if (axys_vfs_type(node) != AXYS_VFS_FILE) {
        return -AXYS_EISDIR;
    }
    size = axys_vfs_size(node);
    if (size <= 0 || (axys_uint32_t)size > ELF_MAX_BYTES) {
        return -AXYS_ENOEXEC;
    }
    image = axys_kmalloc((axys_size_t)size);
    if (image == AXYS_NULL) {
        axys_printf("process: cannot allocate %d-byte image for %s (heap free %u, PMM free %u KiB)\n",
                    size, path, (axys_uint32_t)axys_heap_bytes_free(),
                    (axys_uint32_t)(axys_pmm_free_frames() * (AXYS_PMM_FRAME_SIZE / 1024u)));
        return -AXYS_ENOMEM;
    }
    if (axys_vfs_pread(node, 0, image, (axys_size_t)size) != size) {
        axys_kfree(image);
        return -AXYS_ENOEXEC;
    }

    flags = axys_cpu_save_flags();
    axys_cpu_disable_interrupts();
    proc = slot_alloc();
    axys_cpu_restore_flags(flags);
    if (proc == AXYS_NULL) {
        axys_kfree(image);
        return -AXYS_EAGAIN;
    }
    proc->parent = parent;
    proc->uid = uid;
    proc->gid = gid;
    {
        const char *base = path;

        for (const char *c = path; *c != '\0'; ++c) {
            if (*c == '/') {
                base = c + 1;
            }
        }
        axys_strlcpy(proc->name, base, sizeof(proc->name));
    }

    if (axys_aspace_create(&proc->space) != 0) {
        axys_printf("process: cannot allocate address space for %s\n", path);
        rc = -AXYS_ENOMEM;
        goto fail_image;
    }
    rc = load_elf(proc, image, (axys_size_t)size, &entry, &end);
    axys_kfree(image);
    image = AXYS_NULL;
    if (rc != 0) {
        goto fail_space;
    }

    /* Heap, then stack, both with a randomised offset. */
    proc->brk_start = align_up(end) + PAGE + random_pages(ASLR_HEAP_PAGES) * PAGE;
    proc->brk = proc->brk_start;
    stack_top = AXYS_USER_TOP - PAGE - random_pages(ASLR_STACK_PAGES) * PAGE;
    if (axys_aspace_map_zeroed(&proc->space, stack_top - AXYS_USER_STACK_PAGES * PAGE,
                               AXYS_USER_STACK_PAGES * PAGE, AXYS_MAP_WRITE) != 0) {
        axys_printf("process: cannot allocate stack for %s\n", path);
        rc = -AXYS_ENOMEM;
        goto fail_space;
    }
    rsp = stack_top;
    if (args != AXYS_NULL && args[0] != '\0') {
        arg_len = axys_strlen(args);
        if (arg_len >= AXYS_ARGS_MAX) {
            arg_len = AXYS_ARGS_MAX - 1;
        }
        rsp -= (arg_len + 16u) & ~15ULL;
        arg_ptr = rsp;
        {
            char buf[AXYS_ARGS_MAX];

            axys_memcpy(buf, args, arg_len);
            buf[arg_len] = '\0';
            if (axys_aspace_load(&proc->space, arg_ptr, buf, arg_len + 1) != 0) {
                rc = -AXYS_ENOMEM;
                goto fail_space;
            }
        }
    }
    rsp -= 8; /* SysV: rsp+8 is 16-byte aligned at function entry */
    install_std_fds(proc);

    info = axys_kmalloc(sizeof(*info));
    if (info == AXYS_NULL) {
        rc = -AXYS_ENOMEM;
        goto fail_space;
    }
    info->proc = proc;
    info->entry = entry;
    info->rsp = rsp;
    info->arg_ptr = arg_ptr;
    info->arg_len = arg_len;
    {
        /* A new task is runnable the moment it exists. If the timer preempted
         * us before the address space and process were attached, the task
         * would enter user mode on the kernel's page tables with no process:
         * it faults on its first instruction ("process -1"), and the real
         * process slot stays alive forever, so the parent's wait() never
         * returns. Keep the whole sequence atomic on this (UP) CPU. */
        axys_uint64_t irq_flags = axys_cpu_save_flags();

        axys_cpu_disable_interrupts();
        task = axys_task_create(proc->name, user_task_entry, info);
        if (task == AXYS_NULL) {
            axys_cpu_restore_flags(irq_flags);
            axys_kfree(info);
            rc = -AXYS_ENOMEM;
            goto fail_space;
        }
        axys_task_set_address_space(task, proc->space.pml4, proc);
        axys_task_detach(task); /* the process slot carries the exit status */
        axys_cpu_restore_flags(irq_flags);
    }
    return proc->pid;

fail_space:
    axys_aspace_destroy(&proc->space);
fail_image:
    axys_kfree(image);
    proc->used = 0;
    return rc;
}

int axys_process_spawn(const char *path, const char *args, int parent)
{
    return axys_process_spawn_as(path, args, parent, 0, 0);
}

/* Mark `proc` finished. Reparent its children; free zombies nobody will wait
 * for. Caller has interrupts disabled. */
static void retire(struct axys_process *proc, int code)
{
    for (int i = 0; i < AXYS_MAX_PROCS; ++i) {
        struct axys_process *child = &procs[i];

        if (child->used && child->parent == proc->pid) {
            child->parent = 0; /* orphan */
            if (child->zombie) {
                child->used = 0;
            }
        }
    }
    proc->exit_code = code;
    proc->zombie = 1;
    if (proc->parent == 0) {
        proc->used = 0; /* orphaned: nobody to collect it */
    }
}

AXYS_NORETURN void axys_process_exit(int code)
{
    struct axys_process *proc = axys_process_current();
    struct axys_task *task = axys_task_current();

    if (proc == AXYS_NULL) {
        axys_task_exit(code);
    }
    /* Leave the process address space before tearing it down. */
    axys_cpu_disable_interrupts();
    axys_task_set_address_space(task, 0, AXYS_NULL);
    axys_cpu_write_cr3((axys_uint64_t)(axys_uintptr_t)pml4_table);
    axys_aspace_destroy(&proc->space);
    retire(proc, code);
    axys_task_wake(&wait_channel);
    axys_task_exit(0);
}

AXYS_NORETURN void axys_process_fault(const struct axys_interrupt_frame *frame)
{
    struct axys_process *proc = axys_process_current();

    axys_printf("process %d (%s) killed: exception %u at rip=%p error=%p\n",
                proc != AXYS_NULL ? proc->pid : -1, proc != AXYS_NULL ? proc->name : "?",
                (axys_uint32_t)frame->vector, (void *)(axys_uintptr_t)frame->rip,
                (void *)(axys_uintptr_t)frame->error);
    axys_process_exit(AXYS_EXIT_FAULT_BASE + (int)frame->vector);
}

int axys_process_kill_checked(int pid, axys_uint32_t caller_uid)
{
    axys_uint64_t flags = axys_cpu_save_flags();
    struct axys_process *proc;
    int rc = -AXYS_ESRCH;

    axys_cpu_disable_interrupts();
    proc = find_pid(pid);
    if (proc != AXYS_NULL && !proc->zombie) {
        if (caller_uid != 0 && caller_uid != proc->uid) {
            rc = -AXYS_EPERM;
        } else {
            proc->killed = 1;
            rc = 0;
        }
    }
    axys_cpu_restore_flags(flags);
    return rc;
}

int axys_process_kill(int pid)
{
    axys_uint64_t flags = axys_cpu_save_flags();
    struct axys_process *proc;
    int rc = -AXYS_ESRCH;

    axys_cpu_disable_interrupts();
    proc = find_pid(pid);
    if (proc != AXYS_NULL && !proc->zombie) {
        proc->killed = 1;
        rc = 0;
    }
    axys_cpu_restore_flags(flags);
    return rc;
}

int axys_process_interrupt_foreground(void)
{
    struct axys_process *victim = AXYS_NULL;

    for (int i = 0; i < AXYS_MAX_PROCS; ++i) {
        struct axys_process *p = &procs[i];

        if (p->used && !p->zombie && p->parent > 0 && (victim == AXYS_NULL || p->pid > victim->pid)) {
            victim = p;
        }
    }
    if (victim == AXYS_NULL) {
        return 0;
    }
    victim->killed = 1;
    return 1;
}

void axys_process_check_kill(void)
{
    struct axys_process *proc = axys_process_current();

    if (proc != AXYS_NULL && proc->killed) {
        axys_process_exit(AXYS_EXIT_KILLED);
    }
}

int axys_process_wait(int pid, int parent, int *status)
{
    for (;;) {
        axys_uint64_t flags = axys_cpu_save_flags();
        struct axys_process *proc;

        axys_cpu_disable_interrupts();
        proc = find_pid(pid);
        if (proc == AXYS_NULL || proc->parent != parent) {
            axys_cpu_restore_flags(flags);
            return -AXYS_ECHILD;
        }
        if (proc->zombie) {
            if (status != AXYS_NULL) {
                *status = proc->exit_code;
            }
            proc->used = 0;
            axys_cpu_restore_flags(flags);
            return pid;
        }
        axys_task_block(&wait_channel);
        axys_cpu_restore_flags(flags);
    }
}

/* ------------------------------------------------------------------ */

struct proc_test {
    const char *path;
    const char *args;
    int expect;      /* exact exit status, or -1 for "any status >= 128" */
};

static int run_and_wait(const char *path, const char *args)
{
    int status = -1;
    int pid = axys_process_spawn(path, args, AXYS_KERNEL_PARENT);

    if (pid < 0) {
        axys_printf("process: spawn %s failed (%d)\n", path, pid);
        return -1000;
    }
    if (axys_process_wait(pid, AXYS_KERNEL_PARENT, &status) != pid) {
        return -1001;
    }
    return status;
}

void axys_process_selftest(void)
{
    static const struct proc_test tests[] = {
        {"/bin/hello", "world", 42},
        {"/bin/heap", 0, 0},
        {"/bin/fileio", 0, 0},
        {"/bin/crash", "badptr", 0},           /* hostile syscall pointers refused */
        {"/bin/crash", "null", AXYS_EXIT_FAULT_BASE + 14},
        {"/bin/crash", "kread", AXYS_EXIT_FAULT_BASE + 14},
        {"/bin/crash", "kwrite", AXYS_EXIT_FAULT_BASE + 14},
        {"/bin/crash", "kexec", AXYS_EXIT_FAULT_BASE + 14},
        {"/bin/crash", "stackexec", -1},       /* NX (or at least: it must die) */
        {"/bin/crash", "cli", AXYS_EXIT_FAULT_BASE + 13},
        {"/bin/crash", "hlt", AXYS_EXIT_FAULT_BASE + 13},
        {"/bin/crash", "inb", AXYS_EXIT_FAULT_BASE + 13},
        {"/bin/crash", "wrmsr", AXYS_EXIT_FAULT_BASE + 13},
        {"/bin/crash", "cr3", AXYS_EXIT_FAULT_BASE + 13},
        {"/bin/crash", "ud2", AXYS_EXIT_FAULT_BASE + 6},
        {"/bin/crash", "div0", AXYS_EXIT_FAULT_BASE + 0},
    };
    axys_uint64_t frames_before = 0;
    axys_uint64_t switches_before = 0;
    int failures = 0;
    int pid;
    int status = -1;

    /* Pass 0 warms up the kernel heap (it grows in whole arenas and never
     * shrinks, which is not a leak); pass 1 is the one whose frame accounting
     * and results are checked. Both passes must behave identically. */
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            axys_pit_sleep_ms(30); /* let the previous pass's last task be reaped */
            frames_before = axys_pmm_free_frames();
            switches_before = axys_sched_switches();
        }        for (axys_size_t i = 0; i < AXYS_ARRAY_SIZE(tests); ++i) {
            int got = run_and_wait(tests[i].path, tests[i].args);
            int ok = tests[i].expect < 0 ? got >= AXYS_EXIT_FAULT_BASE : got == tests[i].expect;

            if (!ok) {
                axys_printf("process: FAILED %s %s -> %d (wanted %d)\n", tests[i].path,
                            tests[i].args != AXYS_NULL ? tests[i].args : "", got, tests[i].expect);
                ++failures;
            }
        }
    }

    /* A process that never yields must still be stoppable: preemption + kill. */
    pid = axys_process_spawn("/bin/crash", "spin", AXYS_KERNEL_PARENT);
    if (pid < 0) {
        ++failures;
    } else {
        axys_pit_sleep_ms(80);
        if (axys_process_kill(pid) != 0 ||
            axys_process_wait(pid, AXYS_KERNEL_PARENT, &status) != pid || status != AXYS_EXIT_KILLED) {
            axys_printf("process: FAILED spin/kill (status %d)\n", status);
            ++failures;
        }
    }

    /* Privilege separation: an unprivileged user may not touch root's files,
     * processes or the power switch, and keeps what it is entitled to. */
    pid = axys_process_spawn("/bin/crash", "spin", AXYS_KERNEL_PARENT);
    if (pid < 0) {
        ++failures;
    } else {
        char victim[16];
        int perms_status;

        axys_snprintf(victim, sizeof(victim), "%d", pid);
        perms_status = 0;
        {
            int ppid = axys_process_spawn_as("/bin/perms", victim, AXYS_KERNEL_PARENT, 1000, 1000);

            if (ppid < 0 || axys_process_wait(ppid, AXYS_KERNEL_PARENT, &perms_status) != ppid ||
                perms_status != 0) {
                axys_printf("process: FAILED permission checks (perms exit %d, step %d)\n", ppid, perms_status);
                ++failures;
            }
        }
        (void)axys_process_kill(pid);
        (void)axys_process_wait(pid, AXYS_KERNEL_PARENT, &status);
    }

    /* Non-executables and garbage must be refused, not run. */
    {
        axys_vfs_node_t junk = axys_vfs_create("/tmp/notelf", AXYS_VFS_FILE);

        if (junk < 0 || axys_vfs_write(junk, "not an ELF file at all, just text..", 34) < 0 ||
            axys_process_spawn("/tmp/notelf", AXYS_NULL, AXYS_KERNEL_PARENT) != -AXYS_ENOEXEC ||
            axys_process_spawn("/tmp", AXYS_NULL, AXYS_KERNEL_PARENT) != -AXYS_EISDIR ||
            axys_process_spawn("/bin/missing", AXYS_NULL, AXYS_KERNEL_PARENT) != -AXYS_ENOENT) {
            axys_printf("process: FAILED bad-executable handling\n");
            ++failures;
        }
        (void)axys_vfs_unlink("/tmp/notelf");
    }

    /* Reaping happens on context switches, so a fixed sleep can sample
     * mid-teardown under scheduling variance (background tasks like the USB
     * poller shift timing): settle until the count is stable, bounded, then
     * judge. A genuine leak never converges and still fails. */
    for (int settle = 0; settle < 50; ++settle) {
        axys_pit_sleep_ms(10);
        if (axys_pmm_free_frames() == frames_before) {
            break;
        }
    }
    if (axys_pmm_free_frames() != frames_before) {
        axys_printf("process: FAILED frame leak (%u before, %u after)\n", (axys_uint32_t)frames_before,
                    (axys_uint32_t)axys_pmm_free_frames());
        ++failures;
    }
    if (failures != 0) {
        axys_panic("process selftest failed");
    }
    axys_printf("process: selftest passed (%u programs, %u switches, NX %s)\n",
                (axys_uint32_t)AXYS_ARRAY_SIZE(tests) * 2u + 4u,
                (axys_uint32_t)(axys_sched_switches() - switches_before),
                axys_aspace_nx_active() ? "active" : "unavailable");
}
