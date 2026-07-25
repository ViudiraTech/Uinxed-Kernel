# Task 2: Boot console ownership and Bash job control

Implement Task 2 from `docs/superpowers/plans/2026-07-25-tty-cow-fork.md`.

## Binding requirements

- Preserve `.claude/` and unrelated worktree changes.
- Establish PID 1 as session leader and process-group leader before its ELF loader opens standard I/O.
- Do not depend on `process_current()` when opening `/dev/console` on behalf of a process under construction. Add an explicit console acquisition API taking the target process and open flags.
- Make loader failure explicit if PID 1 cannot obtain a valid controlling console.
- Preserve the controlling terminal across fork/exec and validate that foreground process groups are positive and belong to the terminal session.
- `TIOCGPGRP`, `TIOCSPGRP`, background read/write/ioctl stop behavior, and keyboard-generated `SIGINT`/`SIGQUIT`/`SIGTSTP` must follow the terminal session and foreground group.
- Add a focused regression/self-test before production changes, observe it fail, then make it pass.
- Run a focused regression and `make -j8`; report exact commands/results.
- Do not commit: `.git` is read-only in this managed workspace.

## Expected files

`include/drivers/tty.h`, `drivers/char/tty.c`, `kernel/process/elf_loader.c`, `init/main.c`, and `kernel/process/process.c` only if POSIX transition hardening requires it.

## Report contract

Write `.superpowers/sdd/2026-07-25-tty-cow-fork/task-2-report.md` with:

- status (`DONE`, `DONE_WITH_CONCERNS`, `NEEDS_CONTEXT`, or `BLOCKED`);
- changed files and exact behavior;
- red/green test evidence;
- full build evidence;
- session/TTY self-review;
- concerns.

