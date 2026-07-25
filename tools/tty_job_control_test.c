/*
 * Host regression for boot-console ownership and TTY job control.
 *
 * Include the real process implementation so the harness can populate its
 * private process table, then exercise the production session/ctty helpers.
 */

#include "../kernel/process/process.c"
#include <drivers/tty.h>
#include <drivers/tty_core.h>
#include <kernel/boot_process.h>
#include <kernel/elf_loader.h>
#include <kernel/termios.h>
#include <syscall/fcntl.h>

static task_t *host_current_task;
static int     test_failures;
static int     test_failure_line;
static int     scoped_signal_count;
static int     unscoped_signal_count;
static pid_t   last_signal_pgid;
static pid_t   last_signal_sid;
static int     last_signal;
static bool    host_signal_blocked_or_ignored;
static pid_t   simulated_next_pid;
static pid_t   simulated_init_pid;
static pid_t   simulated_debug_pid;
static int     loader_call_count;
static bool    loader_requested_console;

#define CHECK(expr)                                               \
    do {                                                          \
        if (!(expr)) {                                            \
            test_failures++;                                      \
            if (!test_failure_line) test_failure_line = __LINE__; \
        }                                                         \
    } while (0)

void spin_lock(spinlock_t *lock)
{
    (void)lock;
}

void spin_unlock(spinlock_t *lock)
{
    (void)lock;
}

void wait_queue_init(wait_queue_t *queue)
{
    memset(queue, 0, sizeof(*queue));
}

uint64_t wait_queue_wake_all(wait_queue_t *queue)
{
    (void)queue;
    return 0;
}

void wait_queue_prepare(wait_queue_t *queue)
{
    (void)queue;
}

void wait_queue_sleep(void)
{
}

int wait_queue_wait_timed(wait_queue_t *queue, uint64_t deadline_ticks)
{
    (void)queue;
    (void)deadline_ticks;
    return 0;
}

task_t *current_task(void)
{
    return host_current_task;
}

uint64_t sched_ticks(void)
{
    return 0;
}

int copy_from_user(void *dst, const void *src, size_t size)
{
    memcpy(dst, src, size);
    return 0;
}

int copy_to_user(void *dst, const void *src, size_t size)
{
    memcpy(dst, src, size);
    return 0;
}

bool signal_is_blocked_or_ignored(process_t *proc, int sig)
{
    (void)proc;
    (void)sig;
    return host_signal_blocked_or_ignored;
}

int signal_has_pending(signal_state_t *state)
{
    (void)state;
    return 0;
}

int signal_send_pgrp(int64_t pgid, int sig)
{
    unscoped_signal_count++;
    last_signal_pgid = pgid;
    last_signal      = sig;
    return 0;
}

int signal_send_pgrp_session(int64_t pgid, int64_t sid, int sig)
{
    scoped_signal_count++;
    last_signal_pgid = pgid;
    last_signal_sid  = sid;
    last_signal      = sig;
    return 0;
}

const char *get_cmdline(void)
{
    return NULL;
}

void write_serial(uint16_t port, char data)
{
    (void)port;
    (void)data;
}

void fbcon_put_string(const char *string)
{
    (void)string;
}

int elf_loader_load_process_internal(process_t *proc, const uint8_t *elf_data, size_t elf_size, char *const argv[], char *const envp[],
                                     uintptr_t *entry_out, uintptr_t *rsp_out, bool acquire_console)
{
    (void)proc;
    (void)elf_data;
    (void)elf_size;
    (void)argv;
    (void)envp;
    (void)entry_out;
    (void)rsp_out;
    loader_call_count++;
    loader_requested_console = acquire_console;
    return 0;
}

static void simulate_start_init(void)
{
    simulated_init_pid = simulated_next_pid++;
}

static void simulate_start_debug(void)
{
    simulated_debug_pid = simulated_next_pid++;
}

static void reset_signal_record(void)
{
    scoped_signal_count   = 0;
    unscoped_signal_count = 0;
    last_signal_pgid      = 0;
    last_signal_sid       = 0;
    last_signal           = 0;
}

static void install_process(process_t *proc, task_t *task, pid_t pid, pid_t sid, pid_t pgid)
{
    memset(proc, 0, sizeof(*proc));
    memset(task, 0, sizeof(*task));
    task->pid          = (uint64_t)pid;
    task->process      = proc;
    proc->task         = task;
    proc->sid          = sid;
    proc->pgid         = pgid;
    proc->refcount     = 1;
    process_table[pid] = proc;
}

static void test_boot_console_and_job_control(void)
{
    process_t init;
    process_t foreground;
    process_t inherited;
    process_t wrong_session;
    process_t kernel_current;
    task_t    init_task;
    task_t    foreground_task;
    task_t    inherited_task;
    task_t    wrong_session_task;
    task_t    kernel_task;

    memset(process_table, 0, sizeof(process_table));
    install_process(&init, &init_task, 1, 0, 0);
    install_process(&kernel_current, &kernel_task, 4, 4, 4);
    host_current_task = &kernel_task;

    CHECK(process_setsid(&init, NULL) == 0);
    CHECK(init.sid == 1);
    CHECK(init.pgid == 1);

    void *open_private = NULL;
    CHECK(tty_dev_file_open(NULL, O_RDWR | O_NOCTTY, &open_private) == 0);
    CHECK(kernel_current.controlling_tty == NULL);
    CHECK(tty_console_acquire(&init, O_RDWR) == 0);
    tty_core_t *console = process_ctty_get(&init);
    CHECK(console != NULL);
    CHECK(init.controlling_tty == console);
    CHECK(kernel_current.controlling_tty == NULL);
    CHECK(console->session == 1);
    CHECK(console->foreground_pgid == 1);

    install_process(&foreground, &foreground_task, 2, 1, 2);
    foreground.parent = &init;
    process_ctty_inherit(&foreground, &init);
    install_process(&inherited, &inherited_task, 3, 1, 2);
    inherited.parent = &foreground;
    process_ctty_inherit(&inherited, &init);
    CHECK(inherited.controlling_tty == console);

    host_current_task = &init_task;
    int value         = 2;
    CHECK(tty_core_ioctl(console, O_RDWR, TIOCSPGRP, &value) == 0);
    CHECK(console->foreground_pgid == 2);
    value = 0;
    CHECK(tty_core_ioctl(console, O_RDWR, TIOCGPGRP, &value) == 0);
    CHECK(value == 2);

    host_current_task = &foreground_task;
    value             = 0;
    CHECK(tty_core_ioctl(console, O_RDWR, TIOCSPGRP, &value) == -EPERM);
    install_process(&wrong_session, &wrong_session_task, 5, 5, 5);
    value = 5;
    CHECK(tty_core_ioctl(console, O_RDWR, TIOCSPGRP, &value) == -EPERM);

    console->termios.c_lflag &= ~ECHO;
    const uint8_t controls[] = {3, 28, 26};
    const int     signals[]  = {SIGINT, SIGQUIT, SIGTSTP};
    for (size_t i = 0; i < sizeof(controls); i++) {
        reset_signal_record();
        CHECK(tty_core_receive(console, &controls[i], 1, O_NONBLOCK) == 1);
        CHECK(scoped_signal_count == 1);
        CHECK(unscoped_signal_count == 0);
        CHECK(last_signal_pgid == 2);
        CHECK(last_signal_sid == 1);
        CHECK(last_signal == signals[i]);
    }

    reset_signal_record();
    init.parent       = &foreground;
    host_current_task = &init_task;
    uint8_t byte      = 0;
    CHECK(tty_core_read(console, &byte, 1, O_NONBLOCK) == -EINTR);
    CHECK(scoped_signal_count == 1);
    CHECK(unscoped_signal_count == 0);
    CHECK(last_signal_pgid == 1);
    CHECK(last_signal_sid == 1);
    CHECK(last_signal == SIGTTIN);

    reset_signal_record();
    console->termios.c_lflag |= TOSTOP;
    CHECK(tty_core_write(console, &byte, 1, O_NONBLOCK) == -EINTR);
    CHECK(scoped_signal_count == 1);
    CHECK(unscoped_signal_count == 0);
    CHECK(last_signal_pgid == 1);
    CHECK(last_signal_sid == 1);
    CHECK(last_signal == SIGTTOU);

    reset_signal_record();
    value = 2;
    CHECK(tty_core_ioctl(console, O_RDWR, TIOCSPGRP, &value) == -EINTR);
    CHECK(scoped_signal_count == 1);
    CHECK(unscoped_signal_count == 0);
    CHECK(last_signal_pgid == 1);
    CHECK(last_signal_sid == 1);
    CHECK(last_signal == SIGTTOU);

    host_signal_blocked_or_ignored = true;
    reset_signal_record();
    CHECK(tty_core_write(console, &byte, 1, O_NONBLOCK) == 1);
    CHECK(scoped_signal_count == 0);
    CHECK(unscoped_signal_count == 0);

    reset_signal_record();
    value = 2;
    CHECK(tty_core_ioctl(console, O_RDWR, TIOCSPGRP, &value) == 0);
    CHECK(scoped_signal_count == 0);
    CHECK(unscoped_signal_count == 0);
    host_signal_blocked_or_ignored = false;

    tty_core_release(console);
}

static void test_boot_order_and_loader_modes(void)
{
    simulated_next_pid  = 1;
    simulated_init_pid  = 0;
    simulated_debug_pid = 0;
    boot_start_init_before_debug(simulate_start_init, simulate_start_debug);
    CHECK(simulated_init_pid == 1);
    CHECK(simulated_debug_pid == 2);

    process_t init;
    process_t other;
    task_t    init_task;
    task_t    other_task;
    install_process(&init, &init_task, 1, 1, 1);
    install_process(&other, &other_task, 2, 2, 2);

    loader_call_count        = 0;
    loader_requested_console = false;
    CHECK(elf_loader_load_initial_process(&init, NULL, 0, NULL, NULL) == 0);
    CHECK(loader_call_count == 1);
    CHECK(loader_requested_console);

    CHECK(elf_loader_load_initial_process(&other, NULL, 0, NULL, NULL) != 0);
    CHECK(loader_call_count == 1);

    loader_requested_console = true;
    CHECK(elf_loader_load_user_process(&init, NULL, 0, NULL, NULL, NULL, NULL) == 0);
    CHECK(loader_call_count == 2);
    CHECK(!loader_requested_console);
}

int main(void)
{
    test_boot_console_and_job_control();
    test_boot_order_and_loader_modes();
    return test_failures ? (test_failure_line & 0xff) : 0;
}
