/*
 *
 *      task.h
 *      Task (thread/process) management header file
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TASK_H_
#define INCLUDE_TASK_H_

#include <libs/list/intrusive_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/util/rbtree.h>
#include <mem/page.h>
#include <process/ptrace.h>
#include <sync/spin_lock.h>

typedef struct process process_t;
typedef struct cgroup  cgroup_t;
struct seccomp_filter;

#define TASK_NAME_LEN      32
#define TASK_KERNEL_STACK  0x10000
#define TASK_DEFAULT_SLICE 5

/*
 * Upper bound of the PID space.  Must match PROCESS_TABLE_SIZE (process.h):
 * the process table is indexed by pid, so a pid at or beyond this value can
 * never be registered.  PIDs 1..TASK_PID_MAX-1 are allocatable; 0 is reserved
 * for the idle/swapper tasks.
 */
#define TASK_PID_MAX 4096

/*
 * PF_KTHREAD marks a kernel thread (Linux PF_KTHREAD).  Kernel threads have
 * no user address space, are children of kthreadd, and take a distinct exit
 * path that skips user-only teardown (ptrace, controlling tty, vfork).
 */
#define PF_KTHREAD 0x00200000ULL

typedef struct wait_queue {
        ilist_node_t tasks;
        spinlock_t   lock;
} wait_queue_t;

typedef struct task task_t;

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_SLEEPING,
    TASK_STOPPED,
    TASK_ZOMBIE,
    TASK_IDLE,
} task_state_t;

typedef enum {
    TASK_WAKE_NONE,
    TASK_WAKE_NORMAL,
    TASK_WAKE_TIMEOUT,
} task_wake_reason_t;

typedef struct {
        uint64_t fs_base;
        uint64_t gs_base;
        void    *fpu_state;       // 64-byte aligned FXSAVE/XSAVE area
        uint8_t  fpu_initialized; // state contains this thread's FP registers
        uint8_t  fpu_active;      // state is currently live in this CPU
} thread_struct_t;

typedef struct {
        uint64_t rsp;
        uint64_t rbx;
        uint64_t rbp;
        uint64_t r12;
        uint64_t r13;
        uint64_t r14;
        uint64_t r15;
        uint64_t rflags;
        uint64_t rdi;
} task_context_t;

/*
 * Kernel-thread lifecycle state (valid only when PF_KTHREAD is set).
 * `data` is the argument passed to the thread function; `should_stop` is set
 * by kthread_stop() and observed via kthread_should_stop(); `exited`/
 * `exit_code` are published by kthread_exit() and consumed by kthread_stop().
 */
typedef struct kthread_info {
        void        *data;
        bool         should_stop;
        bool         exited;
        int          exit_code;
        wait_queue_t exit_wait;
} kthread_info_t;

struct task {
        uint64_t           pid;
        uint64_t           tgid;
        task_state_t       state;
        volatile uint64_t  on_cpu; // cleared only after switching off this task's stack
        task_context_t     context;
        thread_struct_t    thread;     // per-thread arch state (fs_base, gs_base)
        rb_node_t          run_node;   // EEVDF red-black tree node
        ilist_node_t       sched_node; // sleep / wait_queue linkage
        ilist_node_t       timer_node; // timed-waiter linkage (scheduler.timer_queue)
        page_directory_t  *page_directory;
        uint8_t           *kernel_stack;
        uint64_t           time_slice;
        uint64_t           wake_tick;
        wait_queue_t      *wait_queue;
        task_wake_reason_t wake_reason;
        uint32_t           cpu_id;
        uint32_t           last_cpu;          // previous CPU before migration
        uint64_t           last_wake_tick;    // scheduler tick of last wakeup
        uint64_t           last_migrate_tick; // anti-ping-pong migration stamp
        uint32_t           migration_count;   // scheduler migration statistic
        uint64_t           start_tick;        // scheduler tick at creation
        uint64_t           user_ticks;
        uint64_t           system_ticks;
        uint64_t           voluntary_switches;
        uint64_t           involuntary_switches;

        /* POSIX signal mask, directed pending set and alternate stack are per-thread. */
        sigset_t     signal_blocked;
        sigset_t     signal_saved_mask;
        sigset_t     signal_pending;
        bool         signal_restore_mask;
        stack_t      signal_altstack;
        char         name[TASK_NAME_LEN];
        process_t   *process;
        uint64_t     clear_child_tid;
        ilist_node_t thread_node;
        cgroup_t    *cgroup;
        ilist_node_t cgroup_node;

        /* EEVDF scheduling fields */
        uint64_t vruntime;            // virtual runtime
        uint64_t deadline;            // virtual deadline
        int64_t  vlag;                // virtual lag for placement
        uint32_t weight;              // scheduling weight (NICE_0_LOAD = 1024)
                                      /* PI (Priority Inheritance) fields */
        uint32_t         base_weight; // original weight before PI boost
        uint32_t         pi_weight;   // effective weight for PI waiter ordering
        rb_node_t        pi_node;     // rbtree node for pi_waiters
        struct rt_mutex *blocked_on;  // mutex this task is blocked on, or NULL

        /*
         * rt_mutexes currently owned by this task.  futex_pi_owner_exit()
         * walks this list instead of the global futex hash, so task exit is
         * O(owned mutexes) rather than O(futex buckets).  Guarded by
         * pi_owned_lock; lock order is mutex->lock -> pi_owned_lock.
         */
        spinlock_t   pi_owned_lock;
        ilist_node_t pi_owned;

        /*
         * Active copy_{to,from}_user() exception fixup.  Keeping this in the
         * task (rather than a CPU global) makes it survive preemption and
         * keeps simultaneous uaccess operations on different CPUs separate.
         */
        uintptr_t uaccess_fault_resume;
        uint8_t   uaccess_fault_nofault;

        /* Linux seccomp and no_new_privs are per-thread and survive exec. */
        struct seccomp_filter *seccomp_filter;
        uint8_t                seccomp_mode;
        bool                   no_new_privs;
        ptrace_state_t         ptrace;  // Linux ptrace state is per-thread
        uint64_t               flags;   // PF_KTHREAD etc.
        kthread_info_t         kthread; // kernel-thread lifecycle (PF_KTHREAD only)

        /*
         * Reference count.  task_alloc_status() starts it at 1; task_free()
         * drops that base reference after removing the task from the PID hash
         * and tearing down scheduler-independent resources.  A cross-context
         * holder (e.g. a futex PI owner pointer obtained via
         * pid_find_task_get()) takes an extra reference with task_ref() and
         * releases it with task_put(); the task_t and its kernel stack are
         * freed only when the count reaches zero.
         */
        uint32_t refcount;
};

/* Initialize a wait queue */
void wait_queue_init(wait_queue_t *queue);

/* Block the current task on a wait queue */
void wait_queue_wait(wait_queue_t *queue);

/*
 * Two-phase wait: prepare adds the current task to the wait queue
 * (caller must hold the external lock that protects the condition),
 * sleep actually blocks.  This eliminates the lost-wakeup window.
 *
 * Usage:
 *   spin_lock(&external_lock);
 *   // ... check condition, create entry ...
 *   wait_queue_prepare(&wq);   // add task to queue under lock
 *   spin_unlock(&external_lock);
 *   wait_queue_sleep();        // block
 *   // ... after wakeup, re-check condition ...
 */
void wait_queue_prepare(wait_queue_t *queue);
void wait_queue_sleep(void);

/* Remove a prepared-but-not-yet-slept current task from this queue. */
void wait_queue_cancel(wait_queue_t *queue);

/*
 * Two-phase wait with timeout: prepare under lock, then sleep with
 * a deadline.  The scheduler will wake the task when either
 * wait_queue_wake_one() is called or the deadline (in ticks) expires.
 *
 * Returns 0 if woken normally, -ETIMEDOUT if the deadline expired.
 * The caller must re-check the condition under the external lock after
 * this function returns.
 */
int wait_queue_wait_timed(wait_queue_t *queue, uint64_t deadline_ticks);

/* Wake one task from a wait queue */
task_t *wait_queue_wake_one(wait_queue_t *queue);

/*
 * Wake one task with Linux WF_SYNC-like affinity: a fully sleeping wakee may
 * be placed on the waker's CPU to keep producer/consumer cache state local.
 */
task_t *wait_queue_wake_one_sync(wait_queue_t *queue);

/* Wake every task from a wait queue */
uint64_t wait_queue_wake_all(wait_queue_t *queue);

/* Allocate a task structure */
task_t *task_alloc(const char *name);

/* Allocate a task and preserve the Linux errno for admission failures. */
task_t *task_alloc_status(const char *name, int *error);

/* Take a reference on a task that is known to be alive. */
void task_ref(task_t *task);

/* Drop a reference on a task, freeing it at zero. */
void task_put(task_t *task);

/* Free a task structure (drops the base reference; frees at zero). */
void task_free(task_t *task);

/* Copy a name into a task's name field */
void task_name_copy(task_t *task, const char *name);

/* Find a task by PID (for PI futex) */
task_t *pid_find_task(uint64_t pid);

/*
 * Find a task by PID and take a reference on it, or NULL if it is no longer
 * in the PID hash (i.e. already freed or dying).  The reference must be
 * released with task_put() when the caller is done with the pointer.
 */
task_t *pid_find_task_get(uint64_t pid);

#endif // INCLUDE_TASK_H_
