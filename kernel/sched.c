#include "axys/sched.h"
#include "axys/cpu.h"
#include "axys/gdt.h"
#include "axys/heap.h"
#include "axys/panic.h"
#include "axys/pit.h"
#include "axys/pmm.h"
#include "axys/printf.h"
#include "axys/string.h"

extern void axys_context_switch(axys_uint64_t *save_rsp, axys_uint64_t new_rsp);
extern void axys_task_trampoline(void);
/* Updated on every switch so SYSCALL entry can find the kernel stack. */
axys_uint64_t axys_syscall_kernel_rsp;

static struct axys_task boot_task; /* the thread that ran kmain */
static struct axys_task *idle_task; /* runs only when nothing else can */
static struct axys_task *current;
static struct axys_task *task_list; /* circular; boot_task is the anchor */
static int next_id = 1;
static axys_uint32_t slice_left;
static int need_resched;
static axys_uint64_t switch_count;
static int sched_ready;
static axys_uint64_t kernel_cr3;

struct axys_task *axys_task_current(void)
{
    return current;
}

int axys_task_id(void)
{
    return current != AXYS_NULL ? current->id : 0;
}

axys_uint64_t axys_sched_switches(void)
{
    return switch_count;
}

static void idle_loop(void *arg)
{
    (void)arg;
    for (;;) {
        axys_cpu_idle();
    }
}

void axys_sched_init(void)
{
    axys_memset(&boot_task, 0, sizeof(boot_task));
    axys_strlcpy(boot_task.name, "kmain", sizeof(boot_task.name));
    boot_task.state = AXYS_TASK_RUNNING;
    boot_task.next = &boot_task;
    kernel_cr3 = axys_cpu_read_cr3();
    boot_task.cr3 = kernel_cr3;
    task_list = &boot_task;
    current = &boot_task;
    slice_left = AXYS_SCHED_SLICE_TICKS;
    sched_ready = 1;

    idle_task = axys_task_create("idle", idle_loop, AXYS_NULL);
    if (idle_task == AXYS_NULL) {
        axys_panic("sched: cannot create the idle task");
    }
    idle_task->reapable = 0; /* never exits */
}

static void list_remove(struct axys_task *task)
{
    struct axys_task *prev = task_list;

    while (prev->next != task) {
        prev = prev->next;
    }
    prev->next = task->next;
}

/* Free tasks that have exited. Never frees the running task: its stack is the
 * one we are standing on. Caller has interrupts disabled. */
static void reap_dead(void)
{
    struct axys_task *task = task_list->next;

    while (task != task_list) {
        struct axys_task *next = task->next;

        if (task->state == AXYS_TASK_DEAD && task->reapable && task != current) {
            list_remove(task);
            axys_pmm_free_contiguous(task->kstack_phys, AXYS_TASK_STACK_FRAMES);
            axys_kfree(task);
        }
        task = next;
    }
}

/* Pick the next runnable task after `current`, round-robin; idle if none. */
static struct axys_task *pick_next(void)
{
    struct axys_task *candidate = current->next;

    while (candidate != current) {
        if (candidate != idle_task && candidate->state == AXYS_TASK_READY) {
            return candidate;
        }
        candidate = candidate->next;
    }
    if (current != idle_task && current->state == AXYS_TASK_RUNNING) {
        return current; /* nobody else wants the CPU */
    }
    return idle_task;
}

/* Core switch. Caller has interrupts disabled. */
static void schedule(void)
{
    struct axys_task *previous = current;
    struct axys_task *next;

    reap_dead();
    next = pick_next();
    need_resched = 0;
    slice_left = AXYS_SCHED_SLICE_TICKS;
    if (next == previous) {
        previous->state = AXYS_TASK_RUNNING;
        return;
    }
    if (previous->state == AXYS_TASK_RUNNING) {
        previous->state = AXYS_TASK_READY;
    }
    next->state = AXYS_TASK_RUNNING;
    current = next;
    ++switch_count;

    if (next->kstack_top != 0) {
        axys_gdt_set_kernel_stack((void *)(axys_uintptr_t)next->kstack_top);
        axys_syscall_kernel_rsp = next->kstack_top;
    }
    if (next->cr3 != previous->cr3) {
        axys_cpu_write_cr3(next->cr3);
    }
    axys_context_switch(&previous->rsp, next->rsp);
    /* Resumed: `current` is us again (set by whoever switched to us). */
}

struct axys_task *axys_task_create(const char *name, void (*entry)(void *), void *arg)
{
    struct axys_task *task = axys_kzalloc(sizeof(*task));
    axys_uint64_t stack;
    axys_uint64_t *sp;
    axys_uint64_t flags;

    if (task == AXYS_NULL || !sched_ready) {
        axys_kfree(task);
        return AXYS_NULL;
    }
    stack = axys_pmm_alloc_contiguous(AXYS_TASK_STACK_FRAMES);
    if (stack == 0) {
        axys_kfree(task);
        return AXYS_NULL;
    }
    axys_memset((void *)(axys_uintptr_t)stack, 0, AXYS_TASK_STACK_BYTES);
    axys_strlcpy(task->name, name != AXYS_NULL ? name : "task", sizeof(task->name));
    task->kstack_phys = stack;
    task->kstack_top = stack + AXYS_TASK_STACK_BYTES;
    task->cr3 = kernel_cr3;
    task->state = AXYS_TASK_READY;

    /* Initial frame, popped by axys_context_switch: r15 r14 r13 r12 rbx rbp ret.
     * `ret` lands on the trampoline with rsp == kstack_top, so the trampoline's
     * call sees the ABI-mandated 16-byte alignment. */
    sp = (axys_uint64_t *)(axys_uintptr_t)task->kstack_top;
    *--sp = (axys_uint64_t)(axys_uintptr_t)axys_task_trampoline; /* ret */
    *--sp = 0;                                                   /* rbp */
    *--sp = 0;                                                   /* rbx */
    *--sp = (axys_uint64_t)(axys_uintptr_t)entry;                /* r12 */
    *--sp = (axys_uint64_t)(axys_uintptr_t)arg;                  /* r13 */
    *--sp = 0;                                                   /* r14 */
    *--sp = 0;                                                   /* r15 */
    task->rsp = (axys_uint64_t)(axys_uintptr_t)sp;

    flags = axys_cpu_save_flags();
    axys_cpu_disable_interrupts();
    task->id = next_id++;
    task->next = task_list->next;
    task_list->next = task;
    axys_cpu_restore_flags(flags);
    return task;
}

void axys_task_set_address_space(struct axys_task *task, axys_uint64_t cr3, void *process)
{
    axys_uint64_t flags = axys_cpu_save_flags();

    axys_cpu_disable_interrupts();
    task->cr3 = cr3 != 0 ? cr3 : kernel_cr3;
    task->process = process;
    axys_cpu_restore_flags(flags);
}

void axys_yield(void)
{
    axys_uint64_t flags = axys_cpu_save_flags();

    axys_cpu_disable_interrupts();
    schedule();
    axys_cpu_restore_flags(flags);
}

void axys_task_sleep_ms(axys_uint64_t ms)
{
    axys_uint64_t flags = axys_cpu_save_flags();
    axys_uint64_t ticks = (ms * axys_pit_hz() + 999u) / 1000u;

    axys_cpu_disable_interrupts();
    if (current == idle_task || ticks == 0) {
        axys_cpu_restore_flags(flags); /* idle never sleeps; 0 ms == yield */
        axys_yield();
        return;
    }
    current->wake_tick = axys_pit_ticks() + ticks;
    current->state = AXYS_TASK_SLEEPING;
    schedule();
    axys_cpu_restore_flags(flags);
}

void axys_task_block(void *channel)
{
    if (current == idle_task) {
        return; /* idle must stay runnable; callers loop and re-check */
    }
    current->wait_channel = channel;
    current->state = AXYS_TASK_BLOCKED;
    schedule();
}

void axys_task_wake(void *channel)
{
    axys_uint64_t flags = axys_cpu_save_flags();
    struct axys_task *task;

    axys_cpu_disable_interrupts();
    task = task_list;
    do {
        if (task->state == AXYS_TASK_BLOCKED && task->wait_channel == channel) {
            task->state = AXYS_TASK_READY;
            task->wait_channel = AXYS_NULL;
            need_resched = 1;
        }
        task = task->next;
    } while (task != task_list);
    axys_cpu_restore_flags(flags);
}

void axys_task_detach(struct axys_task *task)
{
    axys_uint64_t flags = axys_cpu_save_flags();

    axys_cpu_disable_interrupts();
    task->reapable = 1;
    axys_cpu_restore_flags(flags);
}

AXYS_NORETURN void axys_task_exit(int code)
{
    axys_cpu_disable_interrupts();
    current->exit_code = code;
    current->state = AXYS_TASK_DEAD;
    schedule();
    axys_panic("sched: dead task was resumed");
}

int axys_task_join(int id)
{
    for (;;) {
        axys_uint64_t flags = axys_cpu_save_flags();
        struct axys_task *task;
        int found = 0;
        int code = -1;

        axys_cpu_disable_interrupts();
        task = task_list;
        do {
            if (task->id == id) {
                found = 1;
                if (task->state == AXYS_TASK_DEAD) {
                    code = task->exit_code;
                    task->reapable = 1;
                    axys_cpu_restore_flags(flags);
                    return code;
                }
            }
            task = task->next;
        } while (task != task_list);
        axys_cpu_restore_flags(flags);
        if (!found) {
            return -1; /* already reaped: exit status is gone */
        }
        axys_task_sleep_ms(10);
    }
}

/* PIT interrupt context. */
void axys_sched_tick(void)
{
    struct axys_task *task;
    axys_uint64_t now;

    if (!sched_ready) {
        return;
    }
    now = axys_pit_ticks();
    task = task_list;
    do {
        if (task->state == AXYS_TASK_SLEEPING && (axys_int64_t)(now - task->wake_tick) >= 0) {
            task->state = AXYS_TASK_READY;
            need_resched = 1;
        }
        task = task->next;
    } while (task != task_list);

    if (slice_left != 0 && --slice_left == 0) {
        need_resched = 1;
    }
}

/* Called from the IRQ dispatcher after the EOI, interrupts still disabled. */
void axys_sched_preempt(void)
{
    if (sched_ready && need_resched) {
        schedule();
    }
}

/* ------------------------------------------------------------------ */

static volatile axys_uint64_t st_busy_count;
static volatile axys_uint64_t st_sleepy_count;
static volatile int st_stop;
static int st_channel;
static volatile int st_flag;

static void st_busy(void *arg)
{
    (void)arg;
    while (!st_stop) {
        ++st_busy_count; /* never yields: only the timer can take the CPU away */
    }
}

static void st_sleepy(void *arg)
{
    (void)arg;
    while (!st_stop) {
        ++st_sleepy_count;
        axys_task_sleep_ms(10);
    }
    axys_task_exit(7);
}

static void st_waiter(void *arg)
{
    axys_uint64_t flags;

    (void)arg;
    flags = axys_cpu_save_flags();
    axys_cpu_disable_interrupts();
    while (!st_flag) {
        axys_task_block(&st_channel);
    }
    axys_cpu_restore_flags(flags);
    axys_task_exit(42);
}

void axys_sched_selftest(void)
{
    axys_uint64_t frames_before = axys_pmm_free_frames();
    struct axys_task *busy = axys_task_create("st-busy", st_busy, AXYS_NULL);
    struct axys_task *sleepy = axys_task_create("st-sleepy", st_sleepy, AXYS_NULL);
    struct axys_task *waiter = axys_task_create("st-wait", st_waiter, AXYS_NULL);
    int sleepy_id;
    int waiter_id;

    if (busy == AXYS_NULL || sleepy == AXYS_NULL || waiter == AXYS_NULL) {
        axys_panic("sched: selftest could not create tasks");
    }
    sleepy_id = sleepy->id;
    waiter_id = waiter->id;
    axys_task_detach(busy); /* nobody joins it; freed as soon as it exits */
    axys_pit_sleep_ms(300);

    if (st_busy_count == 0 || st_sleepy_count < 5) {
        axys_panic("sched: selftest tasks did not all make progress");
    }
    if (switch_count < 10) {
        axys_panic("sched: selftest saw too few context switches");
    }
    st_flag = 1;
    axys_task_wake(&st_channel);
    st_stop = 1;
    if (axys_task_join(waiter_id) != 42 || axys_task_join(sleepy_id) != 7) {
        axys_panic("sched: selftest exit codes wrong");
    }
    /* The busy thread observes st_stop and returns through the trampoline;
     * once every task is reaped their stacks must all be back in the PMM. */
    axys_pit_sleep_ms(100);
    if (axys_pmm_free_frames() != frames_before) {
        axys_panic("sched: frames leaked after tasks exited");
    }
    axys_printf("sched: selftest passed (%u switches, busy=%u sleepy=%u)\n",
                (axys_uint32_t)switch_count, (axys_uint32_t)st_busy_count,
                (axys_uint32_t)st_sleepy_count);
}
