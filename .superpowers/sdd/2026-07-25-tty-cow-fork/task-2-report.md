# Task 2 Report: Boot console ownership and Bash job control

## Status

`DONE_WITH_CONCERNS`

The requested boot-console/session and TTY job-control repairs are implemented. The focused host regression and full kernel build pass. No QEMU boot with an interactive Bash workload was run.

## Changed files and exact behavior

- `Makefile`
  - Excludes the focused TTY regression from ordinary host tools.
  - Adds `test-tty-job-control` and marks it phony.
  - Preserves the existing Task 1 `test-vm-cow` changes.
- `tools/tty_job_control_test.c`
  - Exercises the real process controlling-terminal helpers and real TTY core.
  - Covers PID 1 `sid == pgid == 1`, explicit acquisition for a non-current process, `O_NOCTTY` suppression of implicit current-process acquisition, fork-style controlling-terminal inheritance, `TIOCGPGRP`, valid and invalid `TIOCSPGRP`, session membership, foreground-only `SIGINT`/`SIGQUIT`/`SIGTSTP`, scoped background `SIGTTIN`/`SIGTTOU`, and blocked/ignored `SIGTTOU`.
- `include/drivers/tty.h`
  - Declares `tty_console_acquire(struct process *, uint64_t)`.
- `drivers/char/tty.c`
  - Implements explicit console acquisition for a supplied session/process-group leader without consulting `process_current()`.
- `drivers/char/tty_core.c`
  - Sends background job-control stops through `signal_send_pgrp_session()`.
  - Permits background writes and terminal-changing ioctls when `SIGTTOU` is blocked or ignored; read-side blocked/ignored `SIGTTIN` and orphaned-group behavior still return `-EIO`.
- `include/kernel/elf_loader.h`
  - Declares the boot-only `elf_loader_load_initial_process()` entry point.
- `kernel/process/elf_loader.c`
  - Separates one-time boot console setup from ordinary exec image loading.
  - Installs `/dev/console` with `O_NOCTTY`, duplicates it to fds 1 and 2, then explicitly acquires it for the target process.
  - Fails the initial load if console open, fd installation/duplication, or controlling-terminal acquisition fails.
  - Ordinary `elf_loader_load_user_process()` no longer opens or reacquires a console, preserving inherited fds, controlling terminal, and foreground group across exec.
- `init/main.c`
  - Calls `process_setsid()` and verifies PID/sid/pgid identity before invoking the initial-process loader.

## Red/green test evidence

Initial RED:

```text
Command: wsl make test-tty-job-control
Result: exit 1
Evidence: implicit declaration of function 'tty_console_acquire'
```

Behavioral RED after adding the explicit API:

```text
Command: wsl make test-tty-job-control
Result: exit 229
Evidence: first failing assertion was scoped_signal_count == 1 for background SIGTTIN; production used unscoped signal_send_pgrp().
```

Reviewer-edge-case RED:

```text
Command: wsl make test-tty-job-control
Result: exit 3
Evidence: blocked/ignored SIGTTOU write/ioctl assertions failed against the old shared SIGTTIN/SIGTTOU helper behavior.
```

Final GREEN:

```text
Command: wsl make test-tty-job-control
Result: exit 0
```

Changed-file formatting:

```text
Command: wsl clang-format --dry-run --Werror include/drivers/tty.h drivers/char/tty.c drivers/char/tty_core.c include/kernel/elf_loader.h kernel/process/elf_loader.c init/main.c tools/tty_job_control_test.c
Result: exit 0
```

## Full build evidence

Final command:

```text
wsl make -j8
```

Final result: exit 0.

The build compiled `kernel/process/elf_loader.o`, linked `UxImage`, generated `Uinxed-x64.iso`, and ended with:

```text
Kernel: UxImage is ready.
Image: Uinxed-x64.iso is ready.
Compilation complete.
```

An immediately preceding build correctly caught the missing direct `<syscall/fcntl.h>` include for `O_NOCTTY`; adding that dependency and rerunning produced the successful result above.

## Session/TTY self-review

- Initial identity is established before any ELF-loader standard-I/O open: PID 1 becomes both session leader and process-group leader.
- Boot console setup is an explicit initial-loader mode, not inferred from PID during every exec.
- The VFS file-open callback receives `O_NOCTTY`, so it cannot acquire on behalf of `process_current()`; acquisition then names the target process explicitly.
- `process_fork_status()` already copies `sid`/`pgid` and calls `process_ctty_inherit()`.
- Ordinary exec does not alter `sid`, `pgid`, or `controlling_tty`, and no longer opens an extra console fd or resets the foreground group.
- Foreground pgids must be positive and `process_ctty_set_foreground()` verifies a member exists in the terminal session.
- `TIOCGPGRP`, `TIOCSPGRP`, background read/write/ioctl checks, keyboard control signals, resize signals, hangup signals, and disassociation signals are all tied to the recorded terminal session and positive foreground group.
- Background `SIGTTIN` blocked/ignored behavior remains `-EIO`; blocked/ignored `SIGTTOU` permits write/ioctl as required.

## Concerns

- No interactive QEMU/Bash boot test was run; validation is the focused host regression, static lifecycle review, and full kernel/ISO build.
- The host regression stubs scheduler waiting and final signal delivery while exercising the production process-table/TTY state-transition logic.

## Fix Round 1

### Reviewer findings addressed

- Boot PID ordering:
  - Added `boot_start_init_before_debug()` as the production boot-process sequencer.
  - `kernel_entry()` now creates/loads user init before calling the scheduler debug initializer, so `CONFIG_SCHED_DEBUG_DEMO=y` cannot consume PID 1 first.
  - `swapper_run_init()` explicitly panics unless the allocated init task has PID 1, then requires `sid == pgid == 1`.
- Initial loader defense and exec split:
  - Moved the public loader entrypoints into `kernel/process/elf_loader_entry.c`.
  - `elf_loader_load_initial_process()` rejects null, taskless, and non-PID1 targets before entering the ELF core and dispatches with console bootstrap enabled only for PID 1.
  - `elf_loader_load_user_process()` dispatches with console bootstrap disabled, so ordinary exec cannot reopen stdio, reacquire the console, or reset its foreground process group.
- Focused behavior coverage:
  - The host regression invokes the production boot sequencer with PID-allocating init/debug callbacks and requires init PID 1 followed by debug PID 2.
  - It invokes the real public initial/ordinary loader entrypoints against a stubbed heavy ELF core, verifies non-PID1 rejection, and verifies initial load requests console acquisition while ordinary exec never does.
  - Fork ctty preservation continues to exercise the production `process_ctty_inherit()` path; the full scheduler/MM `process_fork()` path remains covered by static review because it is not host-runnable in this focused harness.
- Whitespace:
  - Removed the trailing whitespace previously reported in the `kernel/process/elf_loader.c` file header.
  - `git diff --check` now exits 0.

### Fix-round RED evidence

```text
Command: wsl make test-tty-job-control
Result: exit 1
Evidence:
tools/tty_job_control_test.c: fatal error: kernel/boot_process.h: No such file or directory
cc1: fatal error: kernel/process/boot_process.c: No such file or directory
cc1: fatal error: kernel/process/elf_loader_entry.c: No such file or directory
```

This failure was caused by the missing production boot-order and loader-entry seams required by the new regression.

### Fix-round GREEN evidence

```text
Command: wsl make test-tty-job-control
Result: exit 0
```

The passing regression now fails under either realistic mutation: reversing the production callback order makes debug consume simulated PID 1, and dispatching ordinary exec with bootstrap enabled violates the recorded loader-mode assertion.

### Fix-round full build evidence

```text
Command: wsl make -j8
Result: exit 0
```

The build compiled:

```text
CC      init/main.o
CC      kernel/process/boot_process.o
CC      kernel/process/elf_loader.o
CC      kernel/process/elf_loader_entry.o
LD      UxImage
XORRISO Uinxed-x64.iso
```

It ended with:

```text
Kernel: UxImage is ready.
Image: Uinxed-x64.iso is ready.
Compilation complete.
```

### Fix-round concerns

- No QEMU boot with `CONFIG_SCHED_DEBUG_DEMO=y` and interactive Bash was run.
- The focused test exercises the production ordering and public loader entrypoints, but substitutes the heavy ELF mapping core and directly exercises the production ctty inheritance helper rather than running a complete host-side `process_fork()`.
