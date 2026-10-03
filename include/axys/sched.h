#ifndef AXYS_SCHED_H
#define AXYS_SCHED_H

#include "axys/types.h"

/*
 * Preemptive round-robin scheduler (single CPU).
 *
 * The boot thread becomes task 0, the idle task: it is only picked when no
 * other task is runnable. Every other task owns a 16 KiB kernel stack taken
 * from contiguous physical frames. The PIT tick preempts the running task
 * after each time slice; blocking primitives (sleep, wait channels) let a task
 * give up the CPU voluntarily. All scheduler state is protected by disabling
 * local interrupts, which is sufficient until SMP bring-up.
 */

#define AXYS_TASK_NAME_MAX 16
#define AXYS_TASK_STACK_FRAMES 4u
#define AXYS_TASK_STACK_BYTES (AXYS_TASK_STACK_FRAMES * 4096u)
#define AXYS_SCHED_SLICE_TICKS 2u /* 20 ms at the default 100 Hz */

enum axys_task_state {
    AXYS_TASK_READY = 0,
    AXYS_TASK_RUNNING,
    AXYS_TASK_SLEEPING,
    AXYS_TASK_BLOCKED,
    AXYS_TASK_DEAD,
};

struct axys_task {
    axys_uint64_t rsp;           /* saved kernel stack pointer (must stay first) */
    axys_uint64_t kstack_top;    /* top of the kernel stack (TSS.rsp0 / syscall stack) */
    axys_uint64_t kstack_phys;   /* base of the frame run backing the stack */
    axys_uint64_t cr3;           /* address space root loaded when the task runs */
    axys_uint64_t wake_tick;     /* SLEEPING: PIT tick at which to wake */
    void *wait_channel;          /* BLOCKED: what the task waits on */
    void *process;               /* user process owning this task, or NULL */
    struct axys_task *next;      /* circular list of all tasks */
    int state;
    int id;
    int exit_code;
    int reapable;                /* set by join/detach: DEAD task may be freed */
    char name[AXYS_TASK_NAME_MAX];
};

/* Adopt the calling (boot) thread as the idle task. Call once, after the heap,
 * PMM and IDT exist and before the timer is started. */
void axys_sched_init(void);

/* Create a runnable kernel thread. Returns NULL when out of memory. */
struct axys_task *axys_task_create(const char *name, void (*entry)(void *), void *arg);

struct axys_task *axys_task_current(void);
int axys_task_id(void);

void axys_yield(void);
void axys_task_sleep_ms(axys_uint64_t ms);
AXYS_NORETURN void axys_task_exit(int code);

/* Block the caller on `channel` until axys_task_wake(channel). Must be called
 * with interrupts disabled and the wake-up condition re-checked in a loop by
 * the caller (axys_spin_lock_irqsave / axys_cpu_save_flags provide the
 * critical section). Wake-ups are broadcast to every waiter on the channel. */
void axys_task_block(void *channel);
void axys_task_wake(void *channel);

/* A finished task stays around (as a zombie) until it is joined or detached,
 * so its exit code cannot be lost. Every created task must eventually be
 * passed to exactly one of these. */
int axys_task_join(int id);              /* waits, returns the exit code, -1 if unknown */
void axys_task_detach(struct axys_task *task); /* fire-and-forget: free on exit */

/* Timer hooks. axys_sched_tick() runs from the PIT handler; the interrupt
 * dispatcher calls axys_sched_preempt() after the EOI so a task switch never
 * leaves the interrupt controller un-acknowledged. */
void axys_sched_tick(void);
void axys_sched_preempt(void);

/* Address-space + kernel-stack bookkeeping for user tasks. */
void axys_task_set_address_space(struct axys_task *task, axys_uint64_t cr3, void *process);

axys_uint64_t axys_sched_switches(void);
void axys_sched_selftest(void);

#endif
