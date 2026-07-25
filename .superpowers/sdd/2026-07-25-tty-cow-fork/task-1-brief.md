# Task 1: Reference-counted COW address spaces

Implement Task 1 from `docs/superpowers/plans/2026-07-25-tty-cow-fork.md`.

## Binding requirements

- Preserve `.claude/` and unrelated worktree changes.
- Replace eager copying; no eager-copy fallback.
- Add concurrency-safe per-4-KiB physical frame ownership initialized with the allocator metadata.
- New allocations own one reference per constituent 4-KiB frame. Shared 4-KiB, 2-MiB, and 1-GiB leaf mappings retain/release the corresponding frame ranges.
- Clone only the lower PML4 user half. Never modify or free the shared kernel half.
- Preserve user, NX, cache, huge, accessed/dirty, and shared semantics. Private writable mappings become read-only COW in parent and child; shared-writable mappings stay shared writable; already read-only mappings stay read-only but are retained.
- Handle COW write faults before SIGSEGV. A sole owner regains write access without copying; a shared owner allocates an equal-sized backing range, copies bytes, atomically replaces the leaf, flushes the translation, and releases the old range.
- Fork allocation failure must leave parent valid and destroy every child table/reference already created.
- Exec, exit, munmap, fork rollback, and page-directory destruction must balance references.
- Remove the existing `clone_parent_mappings()` implementation, whose huge-page size handling is invalid.
- Add a focused regression/self-test before production changes, observe it fail for the missing COW behavior, then make it pass.
- Run a focused regression and `make -j8`; report exact commands/results.
- Do not commit: `.git` is read-only in this managed workspace.

## Expected files

`include/mem/frame.h`, `mem/frame.c`, `include/mem/page.h`, `mem/page.c`, `kernel/process/process.c`, `kernel/syscall/syscall.c`, `kernel/syscall/mmap.c`, plus a focused test if feasible under repository conventions.

## Report contract

Write `.superpowers/sdd/2026-07-25-tty-cow-fork/task-1-report.md` with:

- status (`DONE`, `DONE_WITH_CONCERNS`, `NEEDS_CONTEXT`, or `BLOCKED`);
- changed files and exact behavior;
- red/green test evidence;
- full build evidence;
- lifecycle/concurrency self-review;
- concerns.

