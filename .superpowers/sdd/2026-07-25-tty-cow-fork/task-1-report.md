# Task 1 Report: Reference-counted COW address spaces

## Status

`DONE_WITH_CONCERNS`

The requested COW implementation, focused regression, rollback coverage, and full kernel/ISO build are complete. The remaining concerns are verification limitations rather than known functional failures: no QEMU boot/runtime stress was requested or run, and the architecture has no cross-CPU TLB-shootdown facility.

## Changed files and behavior

- `Makefile`
  - Adds the host-executable `test-vm-cow` target and excludes its source from normal kernel tool discovery.
- `tools/vm_cow_test.c`
  - Runs the real `mem/page.c` clone, fault, unmap, and destruction code against a deterministic fake physical-frame backend.
  - Covers private writable COW divergence and parent-data preservation; shared-writable and read-only retention; read-only-at-fork followed by write-enable; sole-owner promotion; 2 MiB copy faults; 1 GiB allocation failure and sole-owner promotion; 2 MiB/1 GiB retain/release; partial huge unmap; final-reference release; and clone-allocation rollback.
- `include/mem/frame.h`, `mem/frame.c`
  - Reserves per-4-KiB `uint32_t` reference metadata beside the bitmap during `init_frame()`.
  - Serializes bitmap/reference mutations with the allocator lock and uses atomic reference loads/stores/add/sub operations.
  - Initializes every constituent frame of 4 KiB, 2 MiB, and 1 GiB allocations to one owner.
  - Adds checked `frame_retain_range()`, `frame_release_range()`, and `frame_refcount()`; legacy free APIs now release references and return frames to the bitmap only at zero.
- `include/mem/page.h`, `mem/page.c`
  - Adds software `PTE_COW` and `PTE_SHARED` bits and a per-directory lock.
  - Adds lower-half-only `page_clone_user_cow()`, allocation-rollback cleanup, `page_resolve_cow_fault()`, `page_unmap_release()`, and `page_destroy_user_space()`.
  - Retains 4 KiB/2 MiB/1 GiB leaf ranges; preserves leaf flags; converts only private writable leaves to read-only COW; and flushes parent translations after conversion.
  - Resolves COW faults before SIGSEGV: sole owners regain write permission without allocation, while shared owners receive an equal-sized copied range followed by atomic leaf replacement, TLB invalidation, and old-range release.
  - Splits huge leaves transactionally for partial 4 KiB unmap, including huge-PAT to 4 KiB-PAT translation.
  - Makes the general directory clone/free entry points use the COW user-space lifecycle.
- `kernel/process/process.c`
  - Removes `clone_parent_mappings()` and uses `page_clone_user_cow()`.
  - Initializes directory locks, destroys address spaces with reference-aware teardown, marks `VM_SHARED` PTEs, and propagates unmap allocation failures before dropping VMA metadata.
- `kernel/syscall/syscall.c`
  - Exec teardown now destroys only the old user half and balances leaf/table references.
- `kernel/syscall/mmap.c`
  - Encodes `VM_SHARED` in PTEs, uses reference-aware unmap, propagates huge-split failures, preserves shared state through `mprotect`, and prevents multiply owned private read-only pages from becoming directly writable.
- `ipc/sysv_ipc.c`
  - Marks System V shared-memory PTEs shared, retains a mapping reference on attach, and releases mapped leaves on detach while keeping the segment's backing-owner reference separate.

The unrelated modified `.gitignore` and untracked `.claude/` directory were not changed.

## Red/green test evidence

Initial RED, before production COW APIs:

```text
Command: wsl make test-vm-cow
Result: exit 1
Evidence: PTE_SHARED/PTE_COW undeclared and page_clone_user_cow(),
page_resolve_cow_fault(), and page_destroy_user_space() missing.
```

Additional focused RED cycles:

```text
Command: wsl make test-vm-cow
Result: exit 1
Reason: partial huge-leaf munmap dropped the whole huge mapping.

Command: wsl make test-vm-cow
Result: exit 1
Reason: reviewer regression exposed read-only-at-fork then write-enable
without establishing COW; expanded huge-fault expectations were active.
```

Final GREEN:

```text
Command: wsl make test-vm-cow
Result: exit 0
```

## Full build evidence

```text
Command: wsl make -j8
Result: exit 0
Evidence:
  CC      kernel/process/process.o
  CC      kernel/syscall/mmap.o
  CC      mem/frame.o
  CC      mem/page.o
  LD      UxImage
  XORRISO Uinxed-x64.iso
  Kernel: UxImage is ready.
  Image: Uinxed-x64.iso is ready.
  Compilation complete.
```

`git diff --check` reported no whitespace errors; it emitted only the workspace's CRLF-conversion warnings. A read-only `clang-format --dry-run --Werror` check failed on extensive pre-existing formatting differences throughout touched files, including unchanged regions, so no whole-file formatter rewrite was applied.

## Lifecycle and concurrency self-review

- Allocation: allocator lock serializes bitmap selection and ownership initialization; every allocated 4 KiB constituent begins at reference 1.
- Fork success: child page-table frames own their allocations; every shared leaf range is retained exactly once; parent conversion happens only after the child clone is complete.
- Fork failure: parent leaves are not modified until clone success; rollback recursively releases every retained child leaf and allocated child table, while `process_free()` releases the child root.
- Fault: the address-space lock serializes leaf lookup/replacement. Sole-owner promotion changes flags only. Shared faults allocate/copy first, atomically exchange the leaf, invalidate the translation, then release the old range.
- Huge leaves: retain/release counts are 1, 512, and 262144 frames. Partial unmap preallocates all needed split tables before publishing the replacement.
- Shared mappings: `PTE_SHARED` keeps writable shared mappings writable across fork; System V mappings have distinct backing and per-mapping ownership references.
- Read-only mappings: retained unchanged at fork; a later private write-enable establishes COW when the backing frame has multiple owners.
- Exec/exit/directory destruction: only PML4 indices 0-255 are descended and released. Shared kernel-half entries 256-511 are never freed.
- Munmap: leaf ownership is released before VMA metadata is removed; huge-split allocation failure is returned instead of silently losing tracking.
- Lock order: address-space operations take the directory lock and then the frame allocator lock. Fork already holds the process mmap lock before the directory lock. No reviewed path takes these locks in the reverse order.

## Independent review

An independent read-only code review found no Critical issues. Its Important findings were incorporated:

1. private read-only forked mappings now become COW if later write-enabled;
2. huge partial-unmap allocation is transactional and failures propagate before VMA removal;
3. focused coverage now executes 2 MiB copy faults and 1 GiB failure/sole-owner fault paths.

## Concerns

- TLB invalidation is local (`invlpg`/`flush_tlb`); the repository does not currently expose an address-space-aware cross-CPU shootdown mechanism. Multi-threaded processes concurrently running the same CR3 on different CPUs would need that facility for complete SMP invalidation.
- The focused test executes real page-table logic with a fake frame backend; it does not boot the kernel or exercise Limine metadata placement under QEMU.
- Existing page mapping APIs are `void`; an intermediate page-table allocation failure during ordinary non-COW mapping can still be reported only indirectly by higher layers. The required fork rollback and COW-fault allocation paths do return failure and are covered.

## Fix Round 1

Status remains `DONE_WITH_CONCERNS`: all requested reviewer fixes are implemented and verified. The only retained concern is the architecture-wide lack of cross-CPU TLB shootdown described above.

### Changes

- Added `page_map_new_to()`, a status-returning new-leaf mapping API. It rejects occupied leaves and transactionally rolls back every intermediate page-table frame allocated before a failure. Existing `page_map_to()` callers remain source-compatible through a `void` wrapper.
- Reworked System V attach into a transaction:
  - validate and acquire the segment attachment while protected from concurrent `IPC_RMID`;
  - allocate and populate the SHM VMA before publishing any PTE;
  - retain the backing range once;
  - publish each leaf with `page_map_new_to()`;
  - on failure, unmap/release every published leaf, release every remaining retained frame, decrement the attachment, and free the unpublished VMA.
- `SHM_REMAP` now removes only wholly covered VMAs through a reference-aware range unmap before installing the new attachment. Partial VMA replacement is rejected, so an existing mapping is never silently overwritten or leaked.
- Added `VM_REGION_SHM` and stores the exact segment identity in `vm_private_data`. Fork increments that segment's attachment count; fork rollback, `shmdt`, exec, and exit decrement it through one VMA release path. A removed segment is destroyed only when its last attachment is released.
- VMA lists are detached under `mmap_lock` and release SHM/file identities after unlocking. Reviewed lock order is `mmap_lock -> page-directory lock -> frame lock`, with segment attachment release outside `mmap_lock`; attach lookup uses `shm_global_lock -> segment lock`.
- Shared-owner COW copies no longer hold the IRQ-disabling page-directory lock during a 2 MiB or 1 GiB `memcpy`. The old frame range receives a temporary ownership pin before unlocking, is revalidated before atomic leaf replacement, and is released correctly on success, allocation failure, or a race.
- Expanded the host regression with:
  - successful status-returning mapping;
  - occupied-leaf rejection without replacement;
  - intermediate-table allocation failure without a table-frame leak;
  - a successful shared-owner 1 GiB COW fault using sparse backing, validating the full copy length, replacement frame, marker propagation, old-range decrement, and final release.

### Red/green and build evidence

```text
RED command: wsl make test-vm-cow
Result: exit 1
Evidence: page_map_new_to() was undeclared/undefined while the new
mapping-status, collision, and allocation-rollback regression was active.

GREEN command: wsl make test-vm-cow
Result: exit 0
Evidence: mapping transaction/collision coverage and the successful sparse
1 GiB shared-owner copy path all passed.

Final GREEN command after adding the unlocked-copy ownership pin:
wsl make test-vm-cow
Result: exit 0

Full build command: wsl make -j8
Result: exit 0
Evidence:
  CC      kernel/process/process.o
  CC      kernel/syscall/mmap.o
  CC      mem/page.o
  LD      UxImage
  XORRISO Uinxed-x64.iso
  Kernel: UxImage is ready.
  Image: Uinxed-x64.iso is ready.
  Compilation complete.

Whitespace command: git diff --check
Result: exit 0; only CRLF-conversion warnings were emitted.
```
