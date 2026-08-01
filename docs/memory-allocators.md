# Buddy and slab memory allocators

Uinxed uses two allocator layers. The physical frame allocator is a binary
buddy, and the kernel object allocator is a slab allocator backed by a second
page buddy inside the mapped kernel-heap arena. `liballoc-x86_64.a` is not part
of the link.

## Physical frame buddy

`mem/buddy.c` is a lock-agnostic indexed buddy core. Every order has an
intrusive free list and an exact block count. Allocation splits the smallest
available higher-order block; release immediately coalesces a free buddy of the
same order. The core rejects wrong-order and duplicate frees and provides an
expensive structural validator.

`mem/frame.c` builds this allocator from Limine usable ranges. Its metadata is
reserved from usable physical memory before any page is released to the buddy.
The compatibility APIs still allocate exact contiguous page counts: a rounded
power-of-two block is split, its requested prefix becomes independently owned
4 KiB frames, and the unused suffix is returned immediately. This preserves
per-page reference counting and permits partial final releases. The 2 MiB and
1 GiB APIs request the corresponding minimum buddy order, so their returned
physical addresses retain huge-page alignment.

## Kernel slab allocator

`mem/alloc.c` owns the mapped heap arena passed to `heap_init()`. Arena metadata
is stored at its beginning and permanently reserved. Small allocations use 17
size caches from 16 bytes through 8 KiB. Each cache has its own IRQ-safe lock
and separate partial, full, and empty slab lists. Slab order is selected from
object density and waste, spare bytes rotate the object start for cache
coloring, and one empty slab stays warm while surplus empty slabs are reclaimed.

Objects carry allocation state and cookies. Freed payloads are poisoned, exact
slot boundaries are checked, and duplicate/interior-pointer releases are
rejected through `heap_onerror()`. Requests larger than 8 KiB or aligned above
16 bytes use a power-of-two page block with an aligned user pointer. The same
ownership tags make `free()` and `usable_size()` independent of caller-supplied
sizes.

Typed caches are available through `mem/slab.h`; they support alignment,
constructors/destructors, statistics, shrinking, and safe destruction after all
objects have been returned. `heap_get_stats()`, `heap_validate()`,
`frame_get_stats()`, and `frame_validate()` provide accounting and diagnostics.

## Tests

Run:

```sh
make allocator-test
```

The native suite checks split/coalesce invariants, exact non-power-of-two frame
ranges, wrong and duplicate frees, all heap size/alignment paths, typed caches,
reallocation data preservation, deterministic randomized stress, and concurrent
allocation/free across four host threads.

The design follows the binary buddy and slab relationships documented in the
Linux kernel memory-management documentation:

- <https://docs.kernel.org/core-api/mm-api.html>
- <https://www.kernel.org/doc/gorman/html/understand/understand009.html>
- <https://www.kernel.org/doc/gorman/html/understand/understand011.html>
