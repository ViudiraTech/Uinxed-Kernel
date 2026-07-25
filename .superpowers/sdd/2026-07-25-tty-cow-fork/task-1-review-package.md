# Task 1 review package

## Base
c0e5970

## Status
```text
 M .gitignore
 M Makefile
 M include/mem/frame.h
 M include/mem/page.h
 M ipc/sysv_ipc.c
 M kernel/process/process.c
 M kernel/syscall/mmap.c
 M kernel/syscall/syscall.c
 M mem/frame.c
 M mem/page.c
?? .claude/
?? .superpowers/
?? tools/vm_cow_test.c
```

## Diff stat
```text
 Makefile                 |  11 +-
 include/mem/frame.h      |  19 ++-
 include/mem/page.h       |  16 ++
 ipc/sysv_ipc.c           |   8 +-
 kernel/process/process.c |  98 ++++-------
 kernel/syscall/mmap.c    |  20 ++-
 kernel/syscall/syscall.c |   2 +-
 mem/frame.c              | 162 ++++++++++++-------
 mem/page.c               | 410 +++++++++++++++++++++++++++++++++++++++++++++--
 9 files changed, 598 insertions(+), 148 deletions(-)
```

## Tracked diff
```diff
diff --git a/Makefile b/Makefile
index d3ce281..6b58698 100644
--- a/Makefile
+++ b/Makefile
@@ -309,36 +309,43 @@ PWD            := $(shell pwd)
 HOST_CC        ?= $(CC)
 HOST_CFLAGS    := -Wall -Wextra -O2
 QEMU           := qemu-system-x86_64
 QEMU_FLAGS     := -machine q35 -bios assets/ovmf-code.fd -serial stdio
 
 AS             := $(CC)
 ASFLAGS        := -c -m64 -ffreestanding -nostdlib -fno-omit-frame-pointer -I include
 INIT_ELF       := assets/Limine/init
 
 # Automatically find all C source files in tools/ and generate their binary targets
-TOOL_C_SOURCES := $(filter-out tools/ps2_mouse_protocol_test.c,$(wildcard tools/*.c))
+TOOL_C_SOURCES := $(filter-out tools/ps2_mouse_protocol_test.c tools/vm_cow_test.c,$(wildcard tools/*.c))
 TOOL_TARGETS   := $(TOOL_C_SOURCES:%.c=%)
 PS2_MOUSE_TEST := .cache/ps2_mouse_protocol_test
+VM_COW_TEST    := .cache/vm_cow_test
 
 # If you want to get more details of `dump_stack`, you need to replace `-O3` with `-O0` or '-Os'.
 # `-fno-optimize-sibling-calls` is for `dump_stack` to work properly.
 C_FLAGS        := -Wall -Wextra -Wno-unused-function -O3 -g3 -m64 -fpie -ffreestanding -fno-optimize-sibling-calls -fno-stack-protector -fno-omit-frame-pointer -mstackrealign -mno-red-zone -I include -MMD
 LD_FLAGS       := -nostdlib -pie -T assets/linker.ld -m elf_x86_64
 
 all: Uinxed-x64.iso
 
 test-ps2-mouse:
 	$(Q)mkdir -p $(dir $(PS2_MOUSE_TEST))
 	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I include -o $(PS2_MOUSE_TEST) tools/ps2_mouse_protocol_test.c drivers/input/ps2_mouse_protocol.c
 	$(Q)./$(PS2_MOUSE_TEST)
 
+test-vm-cow:
+	$(Q)mkdir -p $(dir $(VM_COW_TEST))
+	$(Q)$(HOST_CC) $(HOST_CFLAGS) -fno-builtin -ffunction-sections -fdata-sections -DVM_COW_HOST_TEST -I include \
+		-Wl,--gc-sections -o $(VM_COW_TEST) tools/vm_cow_test.c mem/page.c
+	$(Q)./$(VM_COW_TEST)
+
 %.o: %.c
 	$(Q)printf "  CC      $@\n"
 	$(Q)$(CC) $(C_FLAGS) $(C_CONFIG) -MT $@ -c -o $@ $<
 
 $(INIT_ELF): assets/init.S assets/init.ld
 	$(Q)printf "  AS      assets/init.o\n"
 	$(Q)$(AS) $(ASFLAGS) -o assets/init.o $<
 	$(Q)printf "  LD      $@\n"
 	$(Q)$(LD) -nostdlib -static -T assets/init.ld -m elf_x86_64 -o $@ assets/init.o
 	$(Q)$(RM) assets/init.o
@@ -368,21 +375,21 @@ Uinxed-x64.iso: info UxImage $(INIT_ELF)
 	$(Q)cp $(word 2,$^) iso/EFI/Boot
 	$(Q)cp $(INIT_ELF) iso/
 	$(Q)xorriso -as mkisofs -R -r -J -b Limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
                 -hfsplus -apm-block-size 2048 -efi-boot-part --efi-boot-image --protective-msdos-label \
                 --efi-boot Limine/limine-uefi-cd.bin -o $@ iso
 	$(Q)$(RM) -rf iso
 	$(Q)printf "Kernel: $(word 2,$^) is ready.\n"
 	$(Q)printf "Image: $@ is ready.\n"
 	$(Q)printf "Compilation complete.\n"
 
-.PHONY: help run clean format check gen.clangd menuconfig diskimg
+.PHONY: help run clean format check gen.clangd menuconfig diskimg test-ps2-mouse test-vm-cow
 
 help: info
 	$(Q)printf "Uinxed-Kernel Makefile Usage:\n"
 	$(Q)printf "  make all         - Build the entire project.\n"
 	$(Q)printf "  make run         - Run the Uinxed-x64.iso in QEMU.\n"
 	$(Q)printf "  make disk.img    - Build a demo simplefs disk image.\n"
 	$(Q)printf "  make clean       - Clean all generated files.\n"
 	$(Q)printf "  make format      - Format all source files using clang-format.\n"
 	$(Q)printf "  make check       - Run static code checks using clang-tidy.\n"
 	$(Q)printf "  make gen.clangd  - Generate .clangd configuration file.\n"
diff --git a/include/mem/frame.h b/include/mem/frame.h
index ce75f69..b46e07f 100644
--- a/include/mem/frame.h
+++ b/include/mem/frame.h
@@ -7,42 +7,55 @@
  *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
  *
  */
 
 #ifndef INCLUDE_FRAME_H_
 #define INCLUDE_FRAME_H_
 
 #include <kernel/ringlog.h>
 #include <libs/std/stdint.h>
 #include <mem/bitmap.h>
+#include <sync/spin_lock.h>
 
 typedef struct {
-        bitmap_t bitmap;
-        size_t   origin_frames;
-        size_t   usable_frames;
+        bitmap_t   bitmap;
+        uint32_t  *refcounts;
+        size_t     frame_count;
+        size_t     origin_frames;
+        size_t     usable_frames;
+        spinlock_t lock;
 } frame_allocator_t;
 
 extern log_buffer_t      frame_log;
 extern frame_allocator_t frame_allocator;
 
 /* Initialize memory frame */
 void init_frame(void);
 
 /* Allocate memory frames */
 uint64_t alloc_frames(size_t count);
 
 /* Allocate 2M memory frames */
 uint64_t alloc_frames_2M(size_t count);
 
 /* Allocate 1G memory frames */
 uint64_t alloc_frames_1G(size_t count);
 
+/* Retain ownership of a contiguous range of 4 KiB frames. */
+int frame_retain_range(uint64_t addr, size_t count);
+
+/* Release ownership of a range, returning final references to the bitmap. */
+int frame_release_range(uint64_t addr, size_t count);
+
+/* Return the current ownership count of a 4 KiB physical frame. */
+uint32_t frame_refcount(uint64_t addr);
+
 /* Free a memory frame */
 void free_frame(uint64_t addr);
 
 /* Free memory frames */
 void free_frames(uint64_t addr, size_t count);
 
 /* Free 2M memory frames */
 void free_frames_2M(uint64_t addr);
 
 /* Free 1G memory frames */
diff --git a/include/mem/page.h b/include/mem/page.h
index c95bc1f..414fc83 100644
--- a/include/mem/page.h
+++ b/include/mem/page.h
@@ -6,29 +6,32 @@
  *      2025/2/16 By XIAOYI12
  *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
  *
  */
 
 #ifndef INCLUDE_PAGE_H_
 #define INCLUDE_PAGE_H_
 
 #include <libs/std/stddef.h>
 #include <libs/std/stdint.h>
+#include <sync/spin_lock.h>
 
 #define MSR_IA32_PAT 0x277
 
 #define PTE_PRESENT      (0x1 << 0)
 #define PTE_WRITEABLE    (0x1 << 1)
 #define PTE_USER         (0x1 << 2)
 #define PTE_PWT          (0x1 << 3) /* Page Write-Through */
 #define PTE_PCD          (0x1 << 4) /* Page Cache Disable    */
 #define PTE_HUGE         (0x1 << 7)
+#define PTE_COW          (0x1 << 9)  /* Software: private copy-on-write leaf */
+#define PTE_SHARED       (0x1 << 10) /* Software: shared mapping leaf */
 #define PTE_NO_EXECUTE   (((uint64_t)0x1) << 63)
 #define KERNEL_PTE_FLAGS (PTE_PRESENT | PTE_WRITEABLE | PTE_NO_EXECUTE)
 
 /* MMIO flags: uncacheable, no-execute — required for PCI BAR mappings */
 #define PTE_MMIO_FLAGS (PTE_PRESENT | PTE_WRITEABLE | PTE_PCD | PTE_NO_EXECUTE)
 
 /* Page size constants */
 #define PAGE_4K_SIZE 0x1000ULL     // (1ULL << 12)
 #define PAGE_2M_SIZE 0x200000ULL   // (1ULL << 21)
 #define PAGE_1G_SIZE 0x40000000ULL // (1ULL << 30)
@@ -41,20 +44,21 @@
 typedef struct {
         uint64_t value;
 } page_table_entry_t;
 
 typedef struct {
         page_table_entry_t entries[512];
 } page_table_t;
 
 typedef struct {
         page_table_t *table;
+        spinlock_t     lock;
 } page_directory_t;
 
 typedef struct {
         char    pat_str[64];
         uint8_t entries[8];
         uint8_t types[8];
 } pat_config_t;
 
 /* Determine whether the page table entry maps a huge page */
 int is_huge_page(page_table_entry_t *entry);
@@ -85,20 +89,23 @@ page_directory_t *clone_directory(page_directory_t *src);
 
 /* Free a page directory */
 void free_directory(page_directory_t *dir);
 
 /* Maps a virtual address to a physical frame using 4KB pages */
 void page_map_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags);
 
 /* Unmap a 4KB page and return its physical frame, or zero if unmapped */
 uint64_t page_unmap(page_directory_t *directory, uint64_t addr);
 
+/* Unmap a user leaf and release its physical ownership reference. */
+int page_unmap_release(page_directory_t *directory, uint64_t addr);
+
 /* Maps a virtual address to a physical frame using 2MB huge pages */
 void page_map_to_2M(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags);
 
 /* Maps a virtual address to a physical frame using 1GB huge pages */
 void page_map_to_1G(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags);
 
 /* Switch the page directory of the current process */
 void switch_page_directory(page_directory_t *dir);
 
 /* Maps a contiguous physical memory range to the specified virtual address range */
@@ -112,17 +119,26 @@ void page_map_range_to_random_4K(page_directory_t *directory, uint64_t addr, uin
 
 /* Maps random non-contiguous physical pages to the virtual address range using 2M page */
 void page_map_range_to_random_2M(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags);
 
 /* Maps random non-contiguous physical pages to the virtual address range using 1G page */
 void page_map_range_to_random_1G(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags);
 
 /* Intelligently maps random non-contiguous physical pages to the virtual address range */
 void page_map_range_to_random(page_directory_t *directory, uint64_t addr, uint64_t length, uint64_t flags);
 
+/* Clone the lower user half into an empty directory using COW leaves. */
+int page_clone_user_cow(page_directory_t *child, page_directory_t *parent);
+
+/* Resolve a write protection fault on a COW leaf. */
+int page_resolve_cow_fault(page_directory_t *directory, uintptr_t addr);
+
+/* Release all user leaves/tables and the PML4 frame, preserving kernel mappings. */
+void page_destroy_user_space(page_directory_t *directory);
+
 /* Get the PAT configuration */
 pat_config_t get_pat_config(void);
 
 /* Initialize memory page table */
 void page_init(void);
 
 #endif // INCLUDE_PAGE_H_
diff --git a/ipc/sysv_ipc.c b/ipc/sysv_ipc.c
index c6de1a1..9181c5b 100644
--- a/ipc/sysv_ipc.c
+++ b/ipc/sysv_ipc.c
@@ -913,24 +913,29 @@ int64_t sys_shmat(int shmid, const void *shmaddr, int shmflg)
         spin_unlock(&shm_global_lock);
     }
 
     /* Map the physical pages into the process */
     vm_flags_t flags = VM_SHARED;
     if (!(shmflg & SHM_RDONLY)) flags |= VM_WRITE;
     if (shmflg & SHM_EXEC) flags |= VM_EXEC;
     flags |= VM_READ;
 
     spin_lock(&seg->lock);
+    if (frame_retain_range(seg->phys_addr, seg->npages)) {
+        spin_unlock(&seg->lock);
+        return -ENOMEM;
+    }
     for (uint32_t i = 0; i < seg->npages; i++) {
         uint64_t frame     = seg->phys_addr + i * PAGE_4K_SIZE;
         uint64_t pte_flags = PTE_USER | PTE_PRESENT;
         if (flags & VM_WRITE) pte_flags |= PTE_WRITEABLE;
+        pte_flags |= PTE_SHARED;
         if (!(flags & VM_EXEC)) pte_flags |= PTE_NO_EXECUTE;
 
         page_map_to(proc->user_page_dir, vaddr + i * PAGE_4K_SIZE, frame, pte_flags);
     }
 
     seg->nattch++;
     seg->atime = sched_ticks();
     seg->lpid  = (uint32_t)proc->task->pid;
     spin_unlock(&seg->lock);
 
@@ -978,20 +983,21 @@ int64_t sys_shmdt(const void *shmaddr)
         }
         prev = &(*prev)->next;
     }
     spin_unlock(&proc->mmap_lock);
 
     if (found == NULL) return -EINVAL;
 
     size_t   length = found->end - found->start;
     uint32_t npages = (uint32_t)((length + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE);
     free(found);
+    for (uint32_t i = 0; i < npages; i++) (void)page_unmap_release(proc->user_page_dir, vaddr + i * PAGE_4K_SIZE);
 
     /* Find the matching shm segment by size */
     spin_lock(&shm_global_lock);
     for (int i = 0; i < SHM_MAX_SEGS; i++) {
         shm_seg_t *seg = shm_segs[i];
         if (seg == NULL) continue;
 
         spin_lock(&seg->lock);
         if (seg->npages == npages && seg->nattch > 0) {
             seg->nattch--;
@@ -1571,11 +1577,11 @@ void sysv_ipc_init(void)
     memset(sem_seq, 0, sizeof(sem_seq));
     memset(shm_segs, 0, sizeof(shm_segs));
     memset(shm_seq, 0, sizeof(shm_seq));
     memset(msg_queues, 0, sizeof(msg_queues));
     memset(msg_seq, 0, sizeof(msg_seq));
 
     sem_undo_list = NULL;
 
     plogk("sysv_ipc: System V IPC registered (sem=%d sets, shm=%d segments, msg=%d queues)\n", SEM_MAX_SETS, SHM_MAX_SEGS, MSG_MAX_QUEUES);
 #endif
-}
\ No newline at end of file
+}
diff --git a/kernel/process/process.c b/kernel/process/process.c
index 907d00c..296097b 100644
--- a/kernel/process/process.c
+++ b/kernel/process/process.c
@@ -415,89 +415,35 @@ int setup_process_page_dir(process_t *proc)
     if (!new_dir) return 1;
 
     uint64_t pml4_frame = alloc_frames(1);
     if (!pml4_frame) {
         free(new_dir);
         return 1;
     }
 
     page_table_t *pml4 = (page_table_t *)phys_to_virt(pml4_frame);
     page_table_clear(pml4);
-    new_dir->table = pml4;
+    new_dir->table       = pml4;
+    new_dir->lock.lock   = 0;
+    new_dir->lock.rflags = 0;
 
     page_directory_t *kern_dir  = get_kernel_pagedir();
     page_table_t     *kern_pml4 = kern_dir->table;
 
     for (int i = 256; i < 512; i++) { pml4->entries[i] = kern_pml4->entries[i]; }
 
     proc->kernel_page_dir      = kern_dir;
     proc->user_page_dir        = new_dir;
     proc->task->page_directory = new_dir;
     return 0;
 }
 
-static int clone_parent_mappings(process_t *child, const process_t *parent)
-{
-    page_table_t *src_pml4 = parent->user_page_dir->table;
-    page_table_t *dst_pml4 = child->user_page_dir->table;
-
-    for (int l4i = 0; l4i < 256; l4i++) {
-        uint64_t l4e = src_pml4->entries[l4i].value;
-        if (!(l4e & PTE_PRESENT)) continue;
-
-        if (l4e & PTE_HUGE) {
-            uint64_t frame = alloc_frames(512);
-            if (!frame) return 1;
-            memcpy(phys_to_virt(frame), phys_to_virt(l4e & PAGE_4K_MASK), PAGE_1G_SIZE);
-            dst_pml4->entries[l4i].value = frame | (l4e & 0xFFFULL);
-            continue;
-        }
-
-        page_table_t *src_l3   = (page_table_t *)phys_to_virt(l4e & PAGE_4K_MASK);
-        uint64_t      l3_frame = alloc_frames(1);
-        if (!l3_frame) return 1;
-        page_table_t *dst_l3 = (page_table_t *)phys_to_virt(l3_frame);
-        page_table_clear(dst_l3);
-        dst_pml4->entries[l4i].value = l3_frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;
-
-        for (int l3i = 0; l3i < 512; l3i++) {
-            uint64_t l3e = src_l3->entries[l3i].value;
-            if (!(l3e & PTE_PRESENT)) continue;
-
-            if (l3e & PTE_HUGE) {
-                uint64_t frame = alloc_frames(512);
-                if (!frame) return 1;
-                memcpy(phys_to_virt(frame), phys_to_virt(l3e & PAGE_4K_MASK), PAGE_2M_SIZE * 512);
-                dst_l3->entries[l3i].value = frame | (l3e & 0xFFFULL);
-                continue;
-            }
-
-            page_table_t *src_l2   = (page_table_t *)phys_to_virt(l3e & PAGE_4K_MASK);
-            uint64_t      l2_frame = alloc_frames(1);
-            if (!l2_frame) return 1;
-            page_table_t *dst_l2 = (page_table_t *)phys_to_virt(l2_frame);
-            page_table_clear(dst_l2);
-            dst_l3->entries[l3i].value = l2_frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;
-
-            for (int l2i = 0; l2i < 512; l2i++) {
-                uint64_t l2e = src_l2->entries[l2i].value;
-                if (!(l2e & PTE_PRESENT)) continue;
-                uint64_t frame = alloc_frames(1);
-                if (!frame) return 1;
-                memcpy(phys_to_virt(frame), phys_to_virt(l2e & PAGE_4K_MASK), PAGE_4K_SIZE);
-                dst_l2->entries[l2i].value = frame | (l2e & 0xFFFULL);
-            }
-        }
-    }
-    return 0;
-}
-
 vm_area_t *vm_area_alloc(uintptr_t start, uintptr_t end, vm_flags_t flags)
 {
     vm_area_t *vma = malloc(sizeof(vm_area_t));
     if (!vma) return NULL;
     vma->start = start;
     vma->end   = end;
     vma->flags = flags;
     vma->type  = VM_REGION_MMAP;
     vma->next  = NULL;
     return vma;
@@ -895,21 +841,21 @@ static void process_free(process_t *proc)
     if (!proc) return;
 
     task_t *task = proc->task;
     proc->task   = NULL;
     if (task) task->process = NULL;
 
     process_ctty_clear(proc);
     process_fd_table_close(proc);
     signal_state_free(&proc->signal);
     if (proc->user_page_dir) {
-        free_page_table_recursive(proc->user_page_dir->table, 4);
+        page_destroy_user_space(proc->user_page_dir);
         free(proc->user_page_dir);
     }
     mmap_list_free(proc);
     free(proc->kernel_stack);
     slist_destroy(&proc->children, NULL);
     free(proc);
     task_free(task);
 }
 
 void process_init(void)
@@ -1261,21 +1207,21 @@ process_t *process_fork_status(int *error)
     slist_init(&child->children);
 
     if (setup_process_page_dir(child)) {
         if (error) *error = -ENOMEM;
         process_free(child);
         spin_unlock(&parent->mmap_lock);
         spin_unlock(&scheduler.lock);
         return NULL;
     }
 
-    if (clone_parent_mappings(child, parent)) {
+    if (page_clone_user_cow(child->user_page_dir, parent->user_page_dir)) {
         if (error) *error = -ENOMEM;
         process_free(child);
         spin_unlock(&parent->mmap_lock);
         spin_unlock(&scheduler.lock);
         return NULL;
     }
 
     for (vm_area_t *vma = parent->mmap_list; vma; vma = vma->next) {
         vm_area_t *copy = vm_area_alloc(vma->start, vma->end, vma->flags);
         if (!copy) {
@@ -1350,50 +1296,62 @@ pid_t process_next_pid(void)
 
 int process_mmap(process_t *proc, uintptr_t addr, size_t length, vm_flags_t flags)
 {
     if (!proc || !length) return 1;
     size_t pages = ALIGN_UP(length, PAGE_4K_SIZE) / PAGE_4K_SIZE;
     for (size_t i = 0; i < pages; i++) {
         uint64_t frame = alloc_frames(1);
         if (!frame) return 1;
         uint64_t pte_flags = PTE_USER | PTE_PRESENT;
         if (flags & VM_WRITE) pte_flags |= PTE_WRITEABLE;
+        if (flags & VM_SHARED) pte_flags |= PTE_SHARED;
         if (!(flags & VM_EXEC)) pte_flags |= PTE_NO_EXECUTE;
         page_map_to(proc->user_page_dir, addr + i * PAGE_4K_SIZE, frame, pte_flags);
     }
     vm_area_t *vma = vm_area_alloc(addr, addr + pages * PAGE_4K_SIZE, flags);
     if (!vma) return 1;
     vma->type = VM_REGION_MMAP;
     vm_area_insert(proc, vma);
     return 0;
 }
 
 int process_munmap(process_t *proc, uintptr_t addr, size_t length)
 {
     vm_area_t *found = NULL;
+    uintptr_t  end   = 0;
 
     if (!proc || !length) return -EINVAL;
 
-    /* Remove the VMA covering @addr from the list. */
+    /* Keep VMA metadata intact until every mapped frame is released. */
     spin_lock(&proc->mmap_lock);
-    {
-        vm_area_t **prev = &proc->mmap_list;
-        while (*prev) {
-            vm_area_t *vma = *prev;
-            if (vma->start == addr) {
-                *prev = vma->next;
-                found = vma;
-                break;
-            }
-            prev = &vma->next;
+    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
+        if (vma->start == addr) {
+            end = vma->end;
+            break;
         }
     }
     spin_unlock(&proc->mmap_lock);
 
+    if (!end) return -ENOENT;
+    for (uintptr_t va = addr; va < end; va += PAGE_4K_SIZE) {
+        if (page_unmap_release(proc->user_page_dir, va) < 0) return -ENOMEM;
+    }
+
+    spin_lock(&proc->mmap_lock);
+    vm_area_t **prev = &proc->mmap_list;
+    while (*prev) {
+        if ((*prev)->start == addr) {
+            found = *prev;
+            *prev = found->next;
+            break;
+        }
+        prev = &(*prev)->next;
+    }
+    spin_unlock(&proc->mmap_lock);
     if (!found) return -ENOENT;
 
     /* Release file reference if this VMA is file-backed. */
     if (found->vm_file) vfs_close(found->vm_file);
 
     free(found);
     return 0;
 }
diff --git a/kernel/syscall/mmap.c b/kernel/syscall/mmap.c
index 117e393..2e4900e 100644
--- a/kernel/syscall/mmap.c
+++ b/kernel/syscall/mmap.c
@@ -39,20 +39,21 @@ static vm_flags_t prot_to_vm_flags(uint64_t prot)
     if (prot & PROT_WRITE) f |= VM_WRITE;
     if (prot & PROT_EXEC) f |= VM_EXEC;
     return f;
 }
 
 /* Convert vm_flags_t to PTE flags */
 static uint64_t vm_flags_to_pte(vm_flags_t flags)
 {
     uint64_t pte = PTE_USER | PTE_PRESENT;
     if (flags & VM_WRITE) pte |= PTE_WRITEABLE;
+    if (flags & VM_SHARED) pte |= PTE_SHARED;
     if (!(flags & VM_EXEC)) pte |= PTE_NO_EXECUTE;
     return pte;
 }
 
 /* Find a free virtual address range for mmap */
 static uintptr_t find_free_vma_range(process_t *proc, size_t length)
 {
     uintptr_t addr  = MMAP_BASE_ADDR;
     size_t    pages = ALIGN_UP(length, PAGE_4K_SIZE);
 
@@ -126,27 +127,27 @@ static int vma_remove_range(process_t *proc, uintptr_t start, uintptr_t end)
         right->next = vma->next;
         vma->end    = start;
         vma->next   = right;
         prev        = &vma->next;
     }
     spin_unlock(&proc->mmap_lock);
     return 0;
 }
 
 /* Unmap physical pages in a range from the page directory */
-static void unmap_physical_pages(process_t *proc, uintptr_t start, size_t length)
+static int unmap_physical_pages(process_t *proc, uintptr_t start, size_t length)
 {
     uintptr_t end = ALIGN_UP(start + length, PAGE_4K_SIZE);
     for (uintptr_t va = start; va < end; va += PAGE_4K_SIZE) {
-        uintptr_t phys = page_unmap(proc->user_page_dir, va);
-        if (phys) free_frame(phys);
+        if (page_unmap_release(proc->user_page_dir, va) < 0) return -ENOMEM;
     }
+    return EOK;
 }
 
 /* ---------- Full mmap syscall implementation ---------- */
 
 int64_t sys_mmap_pgoff(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset)
 {
     process_t *proc = process_current();
     if (!proc) return -ESRCH;
 
     if (!length) return -EINVAL;
@@ -163,21 +164,22 @@ int64_t sys_mmap_pgoff(uint64_t addr, uint64_t length, uint64_t prot, uint64_t f
             mmap_addr = addr;
         } else {
             mmap_addr = find_free_vma_range(proc, pages);
             if (!mmap_addr) return -ENOMEM;
         }
     } else if (flags & MAP_FIXED) {
         if (!addr) return -EINVAL;
         if (addr > UINT64_MAX - pages) return -EINVAL;
         if (addr + pages > PROCESS_USER_STACK_TOP) return -EINVAL;
         mmap_addr = addr;
-        unmap_physical_pages(proc, mmap_addr, pages);
+        int unmap_result = unmap_physical_pages(proc, mmap_addr, pages);
+        if (unmap_result) return unmap_result;
         int ret = vma_remove_range(proc, mmap_addr, mmap_addr + pages);
         if (ret) return ret;
     } else {
         mmap_addr = find_free_vma_range(proc, pages);
         if (!mmap_addr) return -ENOMEM;
     }
 
     vm_flags_t vm_flags = prot_to_vm_flags(prot);
     if (flags & MAP_SHARED) vm_flags |= VM_SHARED;
 
@@ -296,41 +298,44 @@ vma_done:
 }
 
 int sys_munmap_full(uint64_t addr, uint64_t length)
 {
     process_t *proc = process_current();
     if (!proc) return -ESRCH;
     if (!length || (addr & (PAGE_4K_SIZE - 1))) return -EINVAL;
     if (length > UINT64_MAX - PAGE_4K_SIZE) return -EINVAL;
 
     size_t pages = ALIGN_UP(length, PAGE_4K_SIZE);
-    unmap_physical_pages(proc, (uintptr_t)addr, pages);
+    int unmap_result = unmap_physical_pages(proc, (uintptr_t)addr, pages);
+    if (unmap_result) return unmap_result;
     return vma_remove_range(proc, (uintptr_t)addr, (uintptr_t)addr + pages);
 }
 
 int sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot)
 {
     process_t *proc = process_current();
     if (!proc) return -ESRCH;
     if (!length || (addr & (PAGE_4K_SIZE - 1))) return -EINVAL;
 
     if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;
 
     vm_flags_t vm_flags  = prot_to_vm_flags(prot);
     uint64_t   pte_flags = vm_flags_to_pte(vm_flags);
     size_t     pages     = ALIGN_UP(length, PAGE_4K_SIZE);
 
     /* Update VMA flags */
     spin_lock(&proc->mmap_lock);
     for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
         if (vma->start == (uintptr_t)addr) {
+            vm_flags |= vma->flags & VM_SHARED;
             vma->flags = vm_flags;
+            pte_flags   = vm_flags_to_pte(vm_flags);
             break;
         }
     }
     spin_unlock(&proc->mmap_lock);
 
     /* Update page table entries */
     for (size_t i = 0; i < pages; i += PAGE_4K_SIZE) {
         uintptr_t va   = (uintptr_t)addr + i;
         uintptr_t phys = walk_page_tables(proc->user_page_dir, va);
         if (phys && phys != (uintptr_t)-1) { page_map_to(proc->user_page_dir, va, phys, pte_flags); }
@@ -385,21 +390,24 @@ int64_t sys_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64
 {
     process_t *proc = process_current();
     if (!proc) return -ESRCH;
     if (!old_addr || !old_len) return -EINVAL;
     if (old_len > UINT64_MAX - PAGE_4K_SIZE || new_len > UINT64_MAX - PAGE_4K_SIZE) return -EINVAL;
 
     size_t old_pages = ALIGN_UP(old_len, PAGE_4K_SIZE);
     size_t new_pages = ALIGN_UP(new_len, PAGE_4K_SIZE);
 
     if (new_len <= old_len) {
-        if (new_len < old_len) { unmap_physical_pages(proc, (uintptr_t)old_addr + new_pages, old_pages - new_pages); }
+        if (new_len < old_len) {
+            int unmap_result = unmap_physical_pages(proc, (uintptr_t)old_addr + new_pages, old_pages - new_pages);
+            if (unmap_result) return unmap_result;
+        }
         return (int64_t)old_addr;
     }
 
     /* Expanding: try to extend in-place if possible */
     uintptr_t target = (uintptr_t)old_addr;
     if (flags & 0x1) { /* MREMAP_MAYMOVE */
         if (new_addr) {
             if (new_addr > UINT64_MAX - new_pages) return -EINVAL;
             if (new_addr + new_pages > PROCESS_USER_STACK_TOP) return -EINVAL;
             target = (uintptr_t)new_addr;
diff --git a/kernel/syscall/syscall.c b/kernel/syscall/syscall.c
index 796a830..ad17fd7 100644
--- a/kernel/syscall/syscall.c
+++ b/kernel/syscall/syscall.c
@@ -3039,21 +3039,21 @@ static int64_t do_execve(const char *path, char *const argv[], char *const envp[
     if (setup_process_page_dir(proc)) {
         free(elf_data);
         free_string_array(kargv);
         free_string_array(kenvp);
         return -ENOMEM;
     }
 
     switch_page_directory(proc->user_page_dir);
 
     if (old_dir) {
-        free_page_table_recursive(old_dir->table, 4);
+        page_destroy_user_space(old_dir);
         free(old_dir);
     }
     process_mmap_clear(proc);
 
     proc->heap_brk  = PROCESS_HEAP_START;
     proc->stack_brk = PROCESS_STACK_BASE - PROCESS_STACK_SIZE;
 
     uintptr_t entry = 0;
     uintptr_t rsp   = 0;
     int       ret   = elf_loader_load_user_process(proc, elf_data, total, kargv, kenvp, &entry, &rsp);
diff --git a/mem/frame.c b/mem/frame.c
index 26fd78b..4cd237b 100644
--- a/mem/frame.c
+++ b/mem/frame.c
@@ -6,190 +6,242 @@
  *      2025/2/16 By XIAOYI12
  *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
  *
  */
 
 #include <boot/limine.h>
 #include <chipset/common.h>
 #include <kernel/printk.h>
 #include <kernel/uinxed.h>
 #include <libs/std/stdlib.h>
+#include <libs/std/string.h>
 #include <mem/bitmap.h>
 #include <mem/frame.h>
 #include <mem/hhdm.h>
 #include <mem/page.h>
 
 log_buffer_t      frame_log;
 frame_allocator_t frame_allocator;
 uint64_t          memory_size = 0;
 
 /* Initialize memory frame */
 void init_frame(void)
 {
     struct limine_memmap_response *memory_map = memmap_request.response;
     if (!memory_map) krn_halt();
 
+    frame_allocator.lock.lock   = 0;
+    frame_allocator.lock.rflags = 0;
+
     for (uint64_t i = 0; i < memory_map->entry_count; i++) {
         struct limine_memmap_entry *region = memory_map->entries[i];
         if (region->type == LIMINE_MEMMAP_USABLE) {
             uint64_t region_end = region->base + region->length;
             if (region_end > memory_size) memory_size = region_end;
         }
     }
     log_buffer_write(&frame_log, "frame: Highest usable address is %p\n", memory_size);
-    size_t   bitmap_size    = ALIGN_UP((memory_size / PAGE_4K_SIZE + 7) / 8, PAGE_4K_SIZE);
-    uint64_t bitmap_address = 0;
+    size_t   frame_count      = ALIGN_UP(memory_size, PAGE_4K_SIZE) / PAGE_4K_SIZE;
+    size_t   bitmap_size      = ALIGN_UP((frame_count + 7) / 8, PAGE_4K_SIZE);
+    size_t   refcount_size    = ALIGN_UP(frame_count * sizeof(uint32_t), PAGE_4K_SIZE);
+    size_t   metadata_size    = bitmap_size + refcount_size;
+    uint64_t metadata_address = 0;
 
     for (uint64_t i = 0; i < memory_map->entry_count; i++) {
         struct limine_memmap_entry *region = memory_map->entries[i];
         if (region->type != LIMINE_MEMMAP_USABLE) continue;
 
         uint64_t region_start = ALIGN_UP(MAX(region->base, 0x100000ULL), PAGE_4K_SIZE);
         uint64_t region_end   = ALIGN_DOWN(region->base + region->length, PAGE_4K_SIZE);
-        if (region_start >= region_end || region_end - region_start < bitmap_size) continue;
+        if (region_start >= region_end || region_end - region_start < metadata_size) continue;
 
-        bitmap_address = ALIGN_DOWN(region_end - bitmap_size, PAGE_4K_SIZE);
+        metadata_address = ALIGN_DOWN(region_end - metadata_size, PAGE_4K_SIZE);
         break;
     }
-    if (bitmap_address) {
-        log_buffer_write(&frame_log, "frame: Bitmap allocated at %p (size: %llu KiB)\n", bitmap_address, bitmap_size / 1024);
+    if (metadata_address) {
+        log_buffer_write(&frame_log, "frame: Ownership metadata allocated at %p (size: %llu KiB)\n", metadata_address, metadata_size / 1024);
     } else {
-        log_buffer_write(&frame_log, "frame: Failed to allocate bitmap memory.\n");
+        log_buffer_write(&frame_log, "frame: Failed to allocate ownership metadata.\n");
         return;
     }
     bitmap_t *bitmap = &frame_allocator.bitmap;
-    bitmap_init(bitmap, phys_to_virt(bitmap_address), bitmap_size);
+    bitmap_init(bitmap, phys_to_virt(metadata_address), bitmap_size);
+    frame_allocator.refcounts = phys_to_virt(metadata_address + bitmap_size);
+    frame_allocator.frame_count = frame_count;
+    memset(frame_allocator.refcounts, 0, refcount_size);
     size_t origin_frames = 0;
 
     for (uint64_t i = 0; i < memory_map->entry_count; i++) {
         struct limine_memmap_entry *region = memory_map->entries[i];
         if (region->type == LIMINE_MEMMAP_USABLE) {
             size_t start_frame = region->base / 4096;
             size_t frame_count = region->length / 4096;
             origin_frames += frame_count;
             bitmap_set_range(bitmap, start_frame, start_frame + frame_count, 1);
             log_buffer_write(&frame_log, "frame: Marked   0x%08x frames from %p as usable.\n", frame_count, region->base);
         }
     }
-    size_t bitmap_frame_start = bitmap_address / 4096;
-    size_t bitmap_frame_count = (bitmap_size + 4095) / 4096;
-    size_t bitmap_frame_end   = bitmap_frame_start + bitmap_frame_count;
-    bitmap_set_range(bitmap, bitmap_frame_start, bitmap_frame_end, 0);
+    size_t metadata_frame_start = metadata_address / PAGE_4K_SIZE;
+    size_t metadata_frame_count = metadata_size / PAGE_4K_SIZE;
+    size_t metadata_frame_end   = metadata_frame_start + metadata_frame_count;
+    bitmap_set_range(bitmap, metadata_frame_start, metadata_frame_end, 0);
 
-    log_buffer_write(&frame_log, "frame: Reserved 0x%08x frames for bitmap at %p\n", bitmap_frame_count, bitmap_address);
+    log_buffer_write(&frame_log, "frame: Reserved 0x%08x frames for ownership metadata at %p\n", metadata_frame_count, metadata_address);
 
     frame_allocator.origin_frames = origin_frames;
-    frame_allocator.usable_frames = origin_frames - bitmap_frame_count;
+    frame_allocator.usable_frames = origin_frames - metadata_frame_count;
 
     log_buffer_write(&frame_log, "frame: Total physical frames = 0x%08x (%d KiB)\n", origin_frames, (origin_frames * 4096) >> 10);
     log_buffer_write(&frame_log, "frame: Available frames after deducting bitmap usage = 0x%08x (%d KiB)\n", frame_allocator.usable_frames,
                      (frame_allocator.usable_frames * 4096) >> 10);
 }
 
 /* Allocate memory frames */
 uint64_t alloc_frames(size_t count)
 {
+    if (!count) return 0;
+
+    spin_lock(&frame_allocator.lock);
     bitmap_t *bitmap      = &frame_allocator.bitmap;
     size_t    frame_index = bitmap_find_range(bitmap, count, 1);
-
-    if (frame_index == (size_t)-1) return 0;
+    if (frame_index == (size_t)-1 || frame_index + count > frame_allocator.frame_count) {
+        spin_unlock(&frame_allocator.lock);
+        return 0;
+    }
     bitmap_set_range(bitmap, frame_index, frame_index + count, 0);
+    for (size_t i = 0; i < count; i++) __atomic_store_n(&frame_allocator.refcounts[frame_index + i], 1, __ATOMIC_RELEASE);
     frame_allocator.usable_frames -= count;
-    return frame_index * 4096;
+    spin_unlock(&frame_allocator.lock);
+    return frame_index * PAGE_4K_SIZE;
 }
 
 /* Allocate 2M memory frames */
 uint64_t alloc_frames_2M(size_t count)
 {
+    if (!count || count > SIZE_MAX / 512) return 0;
+
+    spin_lock(&frame_allocator.lock);
     bitmap_t *bitmap         = &frame_allocator.bitmap;
     size_t    frames_per_2mb = 512;
     size_t    total_frames   = count * frames_per_2mb;
 
-    for (size_t i = 0; i < bitmap->length; i += frames_per_2mb) {
-        if (i + total_frames > bitmap->length) break;
+    for (size_t i = 0; i < frame_allocator.frame_count; i += frames_per_2mb) {
+        if (total_frames > frame_allocator.frame_count - i) break;
         if (bitmap_range_all(bitmap, i, i + total_frames, 1)) {
             bitmap_set_range(bitmap, i, i + total_frames, 0);
+            for (size_t j = 0; j < total_frames; j++) __atomic_store_n(&frame_allocator.refcounts[i + j], 1, __ATOMIC_RELEASE);
             frame_allocator.usable_frames -= total_frames;
-            return i * 4096;
+            spin_unlock(&frame_allocator.lock);
+            return i * PAGE_4K_SIZE;
         }
     }
+    spin_unlock(&frame_allocator.lock);
     return 0;
 }
 
 /* Allocate 1G memory frames */
 uint64_t alloc_frames_1G(size_t count)
 {
+    if (!count || count > SIZE_MAX / 262144) return 0;
+
+    spin_lock(&frame_allocator.lock);
     bitmap_t *bitmap         = &frame_allocator.bitmap;
     size_t    frames_per_1gb = 262144;
     size_t    total_frames   = count * frames_per_1gb;
 
-    for (size_t i = 0; i < bitmap->length; i += frames_per_1gb) {
-        if (i + total_frames > bitmap->length) break;
+    for (size_t i = 0; i < frame_allocator.frame_count; i += frames_per_1gb) {
+        if (total_frames > frame_allocator.frame_count - i) break;
         if (bitmap_range_all(bitmap, i, i + total_frames, 1)) {
             bitmap_set_range(bitmap, i, i + total_frames, 0);
+            for (size_t j = 0; j < total_frames; j++) __atomic_store_n(&frame_allocator.refcounts[i + j], 1, __ATOMIC_RELEASE);
             frame_allocator.usable_frames -= total_frames;
-            return i * 4096;
+            spin_unlock(&frame_allocator.lock);
+            return i * PAGE_4K_SIZE;
         }
     }
+    spin_unlock(&frame_allocator.lock);
     return 0;
 }
 
+int frame_retain_range(uint64_t addr, size_t count)
+{
+    if (!addr || !count || (addr & (PAGE_4K_SIZE - 1))) return -1;
+    size_t frame_index = addr / PAGE_4K_SIZE;
+    if (frame_index >= frame_allocator.frame_count || count > frame_allocator.frame_count - frame_index) return -1;
+
+    spin_lock(&frame_allocator.lock);
+    for (size_t i = 0; i < count; i++) {
+        uint32_t refs = __atomic_load_n(&frame_allocator.refcounts[frame_index + i], __ATOMIC_ACQUIRE);
+        if (!refs || refs == UINT32_MAX) {
+            spin_unlock(&frame_allocator.lock);
+            return -1;
+        }
+    }
+    for (size_t i = 0; i < count; i++) __atomic_add_fetch(&frame_allocator.refcounts[frame_index + i], 1, __ATOMIC_RELEASE);
+    spin_unlock(&frame_allocator.lock);
+    return 0;
+}
+
+int frame_release_range(uint64_t addr, size_t count)
+{
+    if (!addr || !count || (addr & (PAGE_4K_SIZE - 1))) return -1;
+    size_t frame_index = addr / PAGE_4K_SIZE;
+    if (frame_index >= frame_allocator.frame_count || count > frame_allocator.frame_count - frame_index) return -1;
+
+    spin_lock(&frame_allocator.lock);
+    for (size_t i = 0; i < count; i++) {
+        if (!__atomic_load_n(&frame_allocator.refcounts[frame_index + i], __ATOMIC_ACQUIRE)) {
+            spin_unlock(&frame_allocator.lock);
+            return -1;
+        }
+    }
+    for (size_t i = 0; i < count; i++) {
+        size_t index = frame_index + i;
+        if (__atomic_sub_fetch(&frame_allocator.refcounts[index], 1, __ATOMIC_ACQ_REL) == 0) {
+            bitmap_set(&frame_allocator.bitmap, index, 1);
+            frame_allocator.usable_frames++;
+        }
+    }
+    spin_unlock(&frame_allocator.lock);
+    return 0;
+}
+
+uint32_t frame_refcount(uint64_t addr)
+{
+    if (!addr || (addr & (PAGE_4K_SIZE - 1))) return 0;
+    size_t frame_index = addr / PAGE_4K_SIZE;
+    if (frame_index >= frame_allocator.frame_count) return 0;
+    return __atomic_load_n(&frame_allocator.refcounts[frame_index], __ATOMIC_ACQUIRE);
+}
+
 /* Free a memory frame */
 void free_frame(uint64_t addr)
 {
-    if (!addr) return;
-    size_t frame_index = addr / 4096;
-
-    if (!frame_index) return;
-    bitmap_t *bitmap = &frame_allocator.bitmap;
-    if (bitmap_get(bitmap, frame_index)) return;
-    bitmap_set(bitmap, frame_index, 1);
-    frame_allocator.usable_frames++;
+    (void)frame_release_range(addr, 1);
 }
 
 /* Free memory frames */
 void free_frames(uint64_t addr, size_t count)
 {
-    if (!addr || !count) return;
-    size_t frame_index = addr / 4096;
-
-    if (!frame_index) return;
-    bitmap_t *bitmap = &frame_allocator.bitmap;
-    if (bitmap_range_all(bitmap, frame_index, frame_index + count, 1)) return;
-    bitmap_set_range(bitmap, frame_index, frame_index + count, 1);
-    frame_allocator.usable_frames += count;
+    (void)frame_release_range(addr, count);
 }
 
 /* Free 2M memory frames */
 void free_frames_2M(uint64_t addr)
 {
-    if (!addr) return;
-    size_t frame_index = addr / 4096;
-
-    if (!frame_index) return;
-    bitmap_t *bitmap = &frame_allocator.bitmap;
-    if (bitmap_range_all(bitmap, frame_index, frame_index + 512, 1)) return;
-    for (size_t i = 0; i < 512; i++) bitmap_set(bitmap, frame_index + i, 1);
-    frame_allocator.usable_frames += 512;
+    (void)frame_release_range(addr, PAGE_2M_SIZE / PAGE_4K_SIZE);
 }
 
 /* Free 1G memory frames */
 void free_frames_1G(uint64_t addr)
 {
-    if (!addr) return;
-    size_t frame_index = addr / 4096;
-
-    if (!frame_index) return;
-    bitmap_t *bitmap = &frame_allocator.bitmap;
-    if (bitmap_range_all(bitmap, frame_index, frame_index + 262144, 1)) return;
-    for (size_t i = 0; i < 262144; i++) bitmap_set(bitmap, frame_index + i, 1);
-    frame_allocator.usable_frames += 262144;
+    (void)frame_release_range(addr, PAGE_1G_SIZE / PAGE_4K_SIZE);
 }
 
 /* Print memory map */
 void print_memory_map(void)
 {
     if (!memmap_request.response) return;
     plogk("Physical RAM map:\n");
     plogk(" <MEMMAP>\n");
 
     for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
diff --git a/mem/page.c b/mem/page.c
index 764ca4a..23654d1 100644
--- a/mem/page.c
+++ b/mem/page.c
@@ -48,20 +48,22 @@ INTERRUPT_BEGIN void page_fault_handle(interrupt_frame_t *frame, uint64_t error_
     else if (id)
         pf_msg = "InstructionFetch";
     else if (rw && present)
         pf_msg = "ReadOnly";
 
     carry_error_code = 1; // carry error code
 
     if (us) {
         process_t *proc = process_current();
         if (proc) {
+            if (present && rw && !reserved && proc->user_page_dir && page_resolve_cow_fault(proc->user_page_dir, faulting_address) == 0) return;
+
             siginfo_t info = {0};
             info.si_signo  = SIGSEGV;
             info.si_code   = present ? SEGV_ACCERR : SEGV_MAPERR;
             info.si_addr   = (void *)faulting_address;
 
             plogk("#PF (pid=%llu): Segmentation fault at 0x%016llx\n", proc->task->pid, faulting_address);
             signal_send_thread(proc->task, SIGSEGV, &info);
 
             syscall_frame_t sigframe = {0};
             sigframe.rip             = frame->rip;
@@ -112,20 +114,21 @@ void enable_paging(uintptr_t page_directory_phys)
 void page_table_clear(page_table_t *table)
 {
     for (int i = 0; i < 512; i++) table->entries[i].value = 0;
 }
 
 /* Create a memory page table */
 page_table_t *page_table_create(page_table_entry_t *entry)
 {
     if (!entry->value) {
         uint64_t frame      = alloc_frames(1);
+        if (!frame) return NULL;
         entry->value        = frame | PTE_PRESENT | PTE_WRITEABLE | PTE_USER;
         page_table_t *table = (page_table_t *)phys_to_virt(entry->value & PAGE_4K_MASK);
         page_table_clear(table);
         return table;
     }
     page_table_t *table = (page_table_t *)phys_to_virt(entry->value & PAGE_4K_MASK);
     return table;
 }
 
 /* Returns the kernel's page directory */
@@ -133,20 +136,282 @@ page_directory_t *get_kernel_pagedir(void)
 {
     return &kernel_page_dir;
 }
 
 /* Returns the page directory of the current process */
 page_directory_t *get_current_directory(void)
 {
     return current_directory;
 }
 
+static uint64_t leaf_address_mask(int level)
+{
+    if (level == 3) return PAGE_1G_MASK;
+    if (level == 2) return PAGE_2M_MASK;
+    return PAGE_4K_MASK;
+}
+
+static size_t leaf_frame_count(int level)
+{
+    if (level == 3) return PAGE_1G_SIZE / PAGE_4K_SIZE;
+    if (level == 2) return PAGE_2M_SIZE / PAGE_4K_SIZE;
+    return 1;
+}
+
+static uint64_t cow_leaf_value(uint64_t value)
+{
+    if ((value & PTE_WRITEABLE) && !(value & PTE_SHARED)) return (value & ~PTE_WRITEABLE) | PTE_COW;
+    return value;
+}
+
+static void destroy_table(page_table_t *table, int level)
+{
+    for (int i = 0; i < 512; i++) {
+        uint64_t value = table->entries[i].value;
+        if (!(value & PTE_PRESENT)) continue;
+
+        if (level == 1 || (value & PTE_HUGE)) {
+            uint64_t mask = leaf_address_mask(level);
+            (void)frame_release_range(value & mask, leaf_frame_count(level));
+        } else {
+            page_table_t *next = phys_to_virt(value & PAGE_4K_MASK);
+            destroy_table(next, level - 1);
+        }
+        table->entries[i].value = 0;
+    }
+
+    (void)frame_release_range((uint64_t)virt_to_phys((uint64_t)table) & PAGE_4K_MASK, 1);
+}
+
+static void destroy_user_entries(page_directory_t *directory)
+{
+    page_table_t *pml4 = directory->table;
+    if (!pml4) return;
+
+    for (int i = 0; i < 256; i++) {
+        uint64_t value = pml4->entries[i].value;
+        if (!(value & PTE_PRESENT)) {
+            pml4->entries[i].value = 0;
+            continue;
+        }
+        if (!(value & PTE_HUGE)) {
+            page_table_t *pdpt = phys_to_virt(value & PAGE_4K_MASK);
+            destroy_table(pdpt, 3);
+        }
+        pml4->entries[i].value = 0;
+    }
+}
+
+static int clone_table_cow(page_table_t *destination, const page_table_t *source, int level)
+{
+    for (int i = 0; i < 512; i++) {
+        uint64_t value = __atomic_load_n(&source->entries[i].value, __ATOMIC_ACQUIRE);
+        if (!(value & PTE_PRESENT)) continue;
+
+        if (level == 1 || (value & PTE_HUGE)) {
+            uint64_t mask  = leaf_address_mask(level);
+            size_t   count = leaf_frame_count(level);
+            if (frame_retain_range(value & mask, count)) return -1;
+            destination->entries[i].value = cow_leaf_value(value);
+            continue;
+        }
+
+        uint64_t table_frame = alloc_frames(1);
+        if (!table_frame) return -1;
+        page_table_t *next = phys_to_virt(table_frame);
+        page_table_clear(next);
+        destination->entries[i].value = table_frame | (value & ~PAGE_4K_MASK);
+
+        if (clone_table_cow(next, phys_to_virt(value & PAGE_4K_MASK), level - 1)) return -1;
+    }
+    return 0;
+}
+
+static void mark_parent_table_cow(page_table_t *table, int level, uintptr_t base)
+{
+    uint64_t shift = level == 3 ? 30 : (level == 2 ? 21 : 12);
+
+    for (int i = 0; i < 512; i++) {
+        page_table_entry_t *entry = &table->entries[i];
+        uint64_t            value = __atomic_load_n(&entry->value, __ATOMIC_ACQUIRE);
+        if (!(value & PTE_PRESENT)) continue;
+
+        uintptr_t leaf_base = base | ((uintptr_t)i << shift);
+        if (level == 1 || (value & PTE_HUGE)) {
+            uint64_t replacement = cow_leaf_value(value);
+            if (replacement != value) {
+                __atomic_store_n(&entry->value, replacement, __ATOMIC_RELEASE);
+                flush_tlb(leaf_base);
+            }
+        } else {
+            mark_parent_table_cow(phys_to_virt(value & PAGE_4K_MASK), level - 1, leaf_base);
+        }
+    }
+}
+
+int page_clone_user_cow(page_directory_t *child, page_directory_t *parent)
+{
+    if (!child || !child->table || !parent || !parent->table || child == parent) return -1;
+
+    spin_lock(&parent->lock);
+    spin_lock(&child->lock);
+
+    for (int i = 0; i < 256; i++) {
+        if (child->table->entries[i].value) {
+            spin_unlock(&child->lock);
+            spin_unlock(&parent->lock);
+            return -1;
+        }
+    }
+
+    for (int i = 0; i < 256; i++) {
+        uint64_t value = __atomic_load_n(&parent->table->entries[i].value, __ATOMIC_ACQUIRE);
+        if (!(value & PTE_PRESENT)) continue;
+        if (value & PTE_HUGE) goto rollback;
+
+        uint64_t table_frame = alloc_frames(1);
+        if (!table_frame) goto rollback;
+        page_table_t *pdpt = phys_to_virt(table_frame);
+        page_table_clear(pdpt);
+        child->table->entries[i].value = table_frame | (value & ~PAGE_4K_MASK);
+        if (clone_table_cow(pdpt, phys_to_virt(value & PAGE_4K_MASK), 3)) goto rollback;
+    }
+
+    for (int i = 0; i < 256; i++) {
+        uint64_t value = parent->table->entries[i].value;
+        if (!(value & PTE_PRESENT) || (value & PTE_HUGE)) continue;
+        mark_parent_table_cow(phys_to_virt(value & PAGE_4K_MASK), 3, (uintptr_t)i << 39);
+    }
+
+    spin_unlock(&child->lock);
+    spin_unlock(&parent->lock);
+    return 0;
+
+rollback:
+    destroy_user_entries(child);
+    spin_unlock(&child->lock);
+    spin_unlock(&parent->lock);
+    return -1;
+}
+
+typedef struct {
+        page_table_entry_t *entry;
+        uint64_t            value;
+        uint64_t            mask;
+        size_t              size;
+        size_t              frame_count;
+        uintptr_t           base;
+} cow_fault_leaf_t;
+
+static int find_cow_leaf(page_directory_t *directory, uintptr_t addr, cow_fault_leaf_t *leaf)
+{
+    if (((addr >> 39) & 0x1ff) >= 256) return -1;
+
+    page_table_t *table = directory->table;
+    uint64_t      value = table->entries[(addr >> 39) & 0x1ff].value;
+    if (!(value & PTE_PRESENT) || (value & PTE_HUGE)) return -1;
+    table = phys_to_virt(value & PAGE_4K_MASK);
+
+    leaf->entry = &table->entries[(addr >> 30) & 0x1ff];
+    leaf->value = __atomic_load_n(&leaf->entry->value, __ATOMIC_ACQUIRE);
+    if (!(leaf->value & PTE_PRESENT)) return -1;
+    if (leaf->value & PTE_HUGE) {
+        leaf->mask        = PAGE_1G_MASK;
+        leaf->size        = PAGE_1G_SIZE;
+        leaf->frame_count = PAGE_1G_SIZE / PAGE_4K_SIZE;
+        leaf->base        = ALIGN_DOWN(addr, PAGE_1G_SIZE);
+        return 0;
+    }
+    table = phys_to_virt(leaf->value & PAGE_4K_MASK);
+
+    leaf->entry = &table->entries[(addr >> 21) & 0x1ff];
+    leaf->value = __atomic_load_n(&leaf->entry->value, __ATOMIC_ACQUIRE);
+    if (!(leaf->value & PTE_PRESENT)) return -1;
+    if (leaf->value & PTE_HUGE) {
+        leaf->mask        = PAGE_2M_MASK;
+        leaf->size        = PAGE_2M_SIZE;
+        leaf->frame_count = PAGE_2M_SIZE / PAGE_4K_SIZE;
+        leaf->base        = ALIGN_DOWN(addr, PAGE_2M_SIZE);
+        return 0;
+    }
+    table = phys_to_virt(leaf->value & PAGE_4K_MASK);
+
+    leaf->entry       = &table->entries[(addr >> 12) & 0x1ff];
+    leaf->value       = __atomic_load_n(&leaf->entry->value, __ATOMIC_ACQUIRE);
+    leaf->mask        = PAGE_4K_MASK;
+    leaf->size        = PAGE_4K_SIZE;
+    leaf->frame_count = 1;
+    leaf->base        = ALIGN_DOWN(addr, PAGE_4K_SIZE);
+    return (leaf->value & PTE_PRESENT) ? 0 : -1;
+}
+
+int page_resolve_cow_fault(page_directory_t *directory, uintptr_t addr)
+{
+    if (!directory || !directory->table) return -1;
+
+    spin_lock(&directory->lock);
+    cow_fault_leaf_t leaf;
+    if (find_cow_leaf(directory, addr, &leaf) || !(leaf.value & PTE_COW) || (leaf.value & PTE_WRITEABLE)) {
+        spin_unlock(&directory->lock);
+        return -1;
+    }
+
+    uint64_t old_frame = leaf.value & leaf.mask;
+    int      sole      = 1;
+    for (size_t i = 0; i < leaf.frame_count; i++) {
+        if (frame_refcount(old_frame + i * PAGE_4K_SIZE) != 1) {
+            sole = 0;
+            break;
+        }
+    }
+
+    uint64_t replacement_flags = (leaf.value & ~leaf.mask & ~PTE_COW) | PTE_WRITEABLE;
+    if (sole) {
+        __atomic_store_n(&leaf.entry->value, old_frame | replacement_flags, __ATOMIC_RELEASE);
+        flush_tlb(leaf.base);
+        spin_unlock(&directory->lock);
+        return 0;
+    }
+
+    uint64_t new_frame;
+    if (leaf.size == PAGE_1G_SIZE)
+        new_frame = alloc_frames_1G(1);
+    else if (leaf.size == PAGE_2M_SIZE)
+        new_frame = alloc_frames_2M(1);
+    else
+        new_frame = alloc_frames(1);
+
+    if (!new_frame) {
+        spin_unlock(&directory->lock);
+        return -1;
+    }
+
+    memcpy(phys_to_virt(new_frame), phys_to_virt(old_frame), leaf.size);
+    __atomic_exchange_n(&leaf.entry->value, (new_frame & leaf.mask) | replacement_flags, __ATOMIC_ACQ_REL);
+    flush_tlb(leaf.base);
+    (void)frame_release_range(old_frame, leaf.frame_count);
+    spin_unlock(&directory->lock);
+    return 0;
+}
+
+void page_destroy_user_space(page_directory_t *directory)
+{
+    if (!directory || !directory->table) return;
+
+    spin_lock(&directory->lock);
+    page_table_t *root = directory->table;
+    destroy_user_entries(directory);
+    directory->table = NULL;
+    (void)frame_release_range((uint64_t)virt_to_phys((uint64_t)root) & PAGE_4K_MASK, 1);
+    spin_unlock(&directory->lock);
+}
+
 /* Recursively copy memory page tables */
 void copy_page_table_recursive(page_table_t *source_table, page_table_t *new_table, int level)
 {
     if (!level) {
         for (int i = 0; i < 512; i++) new_table->entries[i].value = source_table->entries[i].value;
         return;
     }
     for (int i = 0; i < 512; i++) {
         if (!source_table->entries[i].value) {
             new_table->entries[i].value = 0;
@@ -179,108 +444,233 @@ void free_page_table_recursive(page_table_t *table, int level)
         } else {
             free_page_table_recursive(phys_to_virt(entry->value & PAGE_4K_MASK), level - 1);
         }
     }
     free_frame(physical_address & PAGE_4K_MASK);
 }
 
 /* Clone a page directory */
 page_directory_t *clone_directory(page_directory_t *src)
 {
+    if (!src || !src->table) return NULL;
+
     page_directory_t *new_directory = malloc(sizeof(page_directory_t));
+    if (!new_directory) return NULL;
+
     uint64_t          frame         = alloc_frames(1);
     if (frame == 0) {
         free(new_directory);
         return 0;
     }
-    new_directory->table = (page_table_t *)phys_to_virt(frame);
-    memset(new_directory->table, 0, sizeof(page_table_t));
-    copy_page_table_recursive(src->table, new_directory->table, 3);
+    new_directory->table       = (page_table_t *)phys_to_virt(frame);
+    new_directory->lock.lock   = 0;
+    new_directory->lock.rflags = 0;
+    page_table_clear(new_directory->table);
+    for (int i = 256; i < 512; i++) new_directory->table->entries[i] = src->table->entries[i];
+
+    if (page_clone_user_cow(new_directory, src)) {
+        page_destroy_user_space(new_directory);
+        free(new_directory);
+        return NULL;
+    }
     return new_directory;
 }
 
 /* Free a page directory */
 void free_directory(page_directory_t *dir)
 {
-    free_page_table_recursive(dir->table, 3);
-    free_frame((uint64_t)virt_to_phys((uint64_t)dir->table));
+    if (!dir) return;
+    page_destroy_user_space(dir);
     free(dir);
 }
 
 /* Maps a virtual address to a physical frame using 4KB pages */
 void page_map_to(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
 {
+    if (!directory || !directory->table || !frame) return;
+    spin_lock(&directory->lock);
+
     uint64_t l4_index = (((addr >> 39)) & 0x1ff);
     uint64_t l3_index = (((addr >> 30)) & 0x1ff);
     uint64_t l2_index = (((addr >> 21)) & 0x1ff);
     uint64_t l1_index = (((addr >> 12)) & 0x1ff);
 
     page_table_t *l4_table = directory->table;
     page_table_t *l3_table = page_table_create(&(l4_table->entries[l4_index]));
+    if (!l3_table) goto out;
     page_table_t *l2_table = page_table_create(&(l3_table->entries[l3_index]));
+    if (!l2_table) goto out;
     page_table_t *l1_table = page_table_create(&(l2_table->entries[l2_index]));
-
+    if (!l1_table) goto out;
+
+    uint64_t old_value = l1_table->entries[l1_index].value;
+    if ((old_value & PTE_PRESENT) && (old_value & PAGE_4K_MASK) == (frame & PAGE_4K_MASK)) {
+        if (old_value & PTE_SHARED) flags |= PTE_SHARED;
+        if ((old_value & PTE_COW) && !(flags & PTE_SHARED)) flags = (flags & ~PTE_WRITEABLE) | PTE_COW;
+        if ((flags & PTE_WRITEABLE) && !(flags & PTE_SHARED) && frame_refcount(frame & PAGE_4K_MASK) > 1) {
+            flags = (flags & ~PTE_WRITEABLE) | PTE_COW;
+        }
+    }
     l1_table->entries[l1_index].value = (frame & PAGE_4K_MASK) | flags;
     flush_tlb(addr);
+out:
+    spin_unlock(&directory->lock);
 }
 
 uint64_t page_unmap(page_directory_t *directory, uint64_t addr)
 {
+    if (!directory || !directory->table) return 0;
+    spin_lock(&directory->lock);
+
     uint64_t l4_index = (addr >> 39) & 0x1ff;
     uint64_t l3_index = (addr >> 30) & 0x1ff;
     uint64_t l2_index = (addr >> 21) & 0x1ff;
     uint64_t l1_index = (addr >> 12) & 0x1ff;
 
     page_table_t *l4  = directory->table;
     uint64_t      l4e = l4->entries[l4_index].value;
-    if (!(l4e & PTE_PRESENT) || (l4e & PTE_HUGE)) return 0;
+    if (!(l4e & PTE_PRESENT) || (l4e & PTE_HUGE)) goto not_mapped;
     page_table_t *l3  = phys_to_virt(l4e & PAGE_4K_MASK);
     uint64_t      l3e = l3->entries[l3_index].value;
-    if (!(l3e & PTE_PRESENT) || (l3e & PTE_HUGE)) return 0;
+    if (!(l3e & PTE_PRESENT) || (l3e & PTE_HUGE)) goto not_mapped;
     page_table_t *l2  = phys_to_virt(l3e & PAGE_4K_MASK);
     uint64_t      l2e = l2->entries[l2_index].value;
-    if (!(l2e & PTE_PRESENT) || (l2e & PTE_HUGE)) return 0;
+    if (!(l2e & PTE_PRESENT) || (l2e & PTE_HUGE)) goto not_mapped;
     page_table_t *l1  = phys_to_virt(l2e & PAGE_4K_MASK);
     uint64_t      l1e = l1->entries[l1_index].value;
-    if (!(l1e & PTE_PRESENT)) return 0;
+    if (!(l1e & PTE_PRESENT)) goto not_mapped;
 
     l1->entries[l1_index].value = 0;
     flush_tlb(addr);
+    spin_unlock(&directory->lock);
     return l1e & PAGE_4K_MASK;
+
+not_mapped:
+    spin_unlock(&directory->lock);
+    return 0;
+}
+
+int page_unmap_release(page_directory_t *directory, uint64_t addr)
+{
+    if (!directory || !directory->table || ((addr >> 39) & 0x1ff) >= 256) return -1;
+
+    spin_lock(&directory->lock);
+    cow_fault_leaf_t leaf;
+    if (find_cow_leaf(directory, addr, &leaf)) {
+        spin_unlock(&directory->lock);
+        return 1;
+    }
+
+    if (leaf.size != PAGE_4K_SIZE) {
+        uint64_t first_table_frame  = alloc_frames(1);
+        uint64_t second_table_frame = 0;
+        if (!first_table_frame) {
+            spin_unlock(&directory->lock);
+            return -1;
+        }
+        if (leaf.size == PAGE_1G_SIZE) {
+            second_table_frame = alloc_frames(1);
+            if (!second_table_frame) {
+                (void)frame_release_range(first_table_frame, 1);
+                spin_unlock(&directory->lock);
+                return -1;
+            }
+        }
+
+        page_table_t *first_table = phys_to_virt(first_table_frame);
+        page_table_clear(first_table);
+        uint64_t old_frame  = leaf.value & leaf.mask;
+        uint64_t leaf_flags = leaf.value & ~leaf.mask;
+        uint64_t table_flags = PTE_PRESENT | PTE_WRITEABLE;
+        table_flags |= leaf.value & (PTE_USER | PTE_PWT | PTE_PCD);
+
+        if (leaf.size == PAGE_1G_SIZE) {
+            for (size_t i = 0; i < 512; i++) {
+                first_table->entries[i].value = (old_frame + i * PAGE_2M_SIZE) | leaf_flags;
+            }
+
+            size_t        target_2m   = (addr >> 21) & 0x1ff;
+            page_table_t *second_table = phys_to_virt(second_table_frame);
+            page_table_clear(second_table);
+            uint64_t target_frame = old_frame + target_2m * PAGE_2M_SIZE;
+            const uint64_t huge_pat = 1ULL << 12;
+            int            pat      = (leaf_flags & huge_pat) != 0;
+            uint64_t       pte_flags = leaf_flags & ~(PTE_HUGE | huge_pat);
+            if (pat) pte_flags |= PTE_HUGE;
+            for (size_t i = 0; i < 512; i++) {
+                second_table->entries[i].value = (target_frame + i * PAGE_4K_SIZE) | pte_flags;
+            }
+            first_table->entries[target_2m].value = second_table_frame | table_flags;
+        } else {
+            const uint64_t huge_pat = 1ULL << 12;
+            int            pat      = (leaf_flags & huge_pat) != 0;
+            leaf_flags &= ~(PTE_HUGE | huge_pat);
+            if (pat) leaf_flags |= PTE_HUGE; /* Bit 7 is PAT in a 4 KiB PTE. */
+            for (size_t i = 0; i < 512; i++) {
+                first_table->entries[i].value = (old_frame + i * PAGE_4K_SIZE) | leaf_flags;
+            }
+        }
+
+        __atomic_exchange_n(&leaf.entry->value, first_table_frame | table_flags, __ATOMIC_ACQ_REL);
+        flush_tlb(leaf.base);
+        if (find_cow_leaf(directory, addr, &leaf) || leaf.size != PAGE_4K_SIZE) {
+            spin_unlock(&directory->lock);
+            return -1;
+        }
+    }
+
+    __atomic_store_n(&leaf.entry->value, 0, __ATOMIC_RELEASE);
+    flush_tlb(leaf.base);
+    int result = frame_release_range(leaf.value & leaf.mask, leaf.frame_count);
+    spin_unlock(&directory->lock);
+    return result;
 }
 
 /* Maps a virtual address to a physical frame using 2MB huge pages */
 void page_map_to_2M(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
 {
+    if (!directory || !directory->table || !frame) return;
+    spin_lock(&directory->lock);
+
     uint64_t l4_index = (addr >> 39) & 0x1FF;
     uint64_t l3_index = (addr >> 30) & 0x1FF;
     uint64_t l2_index = (addr >> 21) & 0x1FF;
 
     page_table_t *l4_table = directory->table;
     page_table_t *l3_table = page_table_create(&l4_table->entries[l4_index]);
+    if (!l3_table) goto out;
     page_table_t *l2_table = page_table_create(&l3_table->entries[l3_index]);
+    if (!l2_table) goto out;
 
     l2_table->entries[l2_index].value = (frame & PAGE_2M_MASK) | flags | PTE_HUGE;
     flush_tlb(addr);
+out:
+    spin_unlock(&directory->lock);
 }
 
 /* Maps a virtual address to a physical frame using 1GB huge pages */
 void page_map_to_1G(page_directory_t *directory, uint64_t addr, uint64_t frame, uint64_t flags)
 {
+    if (!directory || !directory->table || !frame) return;
+    spin_lock(&directory->lock);
+
     uint64_t l4_index = (addr >> 39) & 0x1FF;
     uint64_t l3_index = (addr >> 30) & 0x1FF;
 
     page_table_t *l4_table = directory->table;
     page_table_t *l3_table = page_table_create(&l4_table->entries[l4_index]);
+    if (!l3_table) goto out;
 
     l3_table->entries[l3_index].value = (frame & PAGE_1G_MASK) | flags | PTE_HUGE;
     flush_tlb(addr);
+out:
+    spin_unlock(&directory->lock);
 }
 
 /* Switch the page directory of the current process */
 void switch_page_directory(page_directory_t *dir)
 {
     current_directory            = dir;
     page_table_t *physical_table = virt_to_phys((uint64_t)dir->table);
     __asm__ volatile("mov %0, %%cr3" ::"r"(physical_table));
 }
 
```

## New test
```diff
diff --git a/tools/vm_cow_test.c b/tools/vm_cow_test.c
new file mode 100644
index 0000000..5ef5105
--- /dev/null
+++ b/tools/vm_cow_test.c
@@ -0,0 +1,350 @@
+/*
+ * Host regression for the real page-table COW implementation.
+ *
+ * The fake frame backend deliberately models ownership independently of
+ * page.c so wrong retain/release sizes and rollback leaks remain observable.
+ */
+
+#include <mem/frame.h>
+#include <mem/page.h>
+#include <sync/spin_lock.h>
+
+#define TEST_POOL_SIZE   (32 * 1024 * 1024)
+#define TEST_FRAME_COUNT (2 * PAGE_1G_SIZE / PAGE_4K_SIZE + 4096)
+
+static unsigned char test_pool[TEST_POOL_SIZE] __attribute__((aligned(PAGE_4K_SIZE)));
+static uint32_t      test_refs[TEST_FRAME_COUNT];
+static uint64_t      next_frame = PAGE_2M_SIZE;
+static size_t        allocation_attempts;
+static size_t        allocation_limit = SIZE_MAX;
+static size_t        live_allocated_frames;
+
+static int test_failures;
+
+#define CHECK(expr)                \
+    do {                           \
+        if (!(expr)) test_failures++; \
+    } while (0)
+
+static void bytes_set(void *dst, unsigned char value, size_t length)
+{
+    unsigned char *out = dst;
+    while (length--) *out++ = value;
+}
+
+static int bytes_equal(const void *left, const void *right, size_t length)
+{
+    const unsigned char *a = left;
+    const unsigned char *b = right;
+    while (length--) {
+        if (*a++ != *b++) return 0;
+    }
+    return 1;
+}
+
+void *memset(void *dst, int value, size_t length)
+{
+    bytes_set(dst, (unsigned char)value, length);
+    return dst;
+}
+
+void *memcpy(void *dst, const void *src, size_t length)
+{
+    unsigned char       *out = dst;
+    const unsigned char *in  = src;
+    while (length--) *out++ = *in++;
+    return dst;
+}
+
+void *phys_to_virt(uint64_t address)
+{
+    if (address >= TEST_POOL_SIZE) return 0;
+    return test_pool + address;
+}
+
+void *virt_to_phys(uint64_t address)
+{
+    return (void *)(address - (uint64_t)test_pool);
+}
+
+void flush_tlb(uint64_t address)
+{
+    (void)address;
+}
+
+void spin_lock(spinlock_t *lock)
+{
+    (void)lock;
+}
+
+void spin_unlock(spinlock_t *lock)
+{
+    (void)lock;
+}
+
+uint64_t alloc_frames(size_t count)
+{
+    allocation_attempts++;
+    if (allocation_attempts > allocation_limit || !count || next_frame + count * PAGE_4K_SIZE > TEST_POOL_SIZE) return 0;
+
+    uint64_t result = next_frame;
+    next_frame += count * PAGE_4K_SIZE;
+    for (size_t i = 0; i < count; i++) test_refs[result / PAGE_4K_SIZE + i] = 1;
+    live_allocated_frames += count;
+    return result;
+}
+
+uint64_t alloc_frames_2M(size_t count)
+{
+    next_frame = (next_frame + PAGE_2M_SIZE - 1) & ~(PAGE_2M_SIZE - 1);
+    return alloc_frames(count * (PAGE_2M_SIZE / PAGE_4K_SIZE));
+}
+
+uint64_t alloc_frames_1G(size_t count)
+{
+    return alloc_frames(count * (PAGE_1G_SIZE / PAGE_4K_SIZE));
+}
+
+int frame_retain_range(uint64_t address, size_t count)
+{
+    size_t first = address / PAGE_4K_SIZE;
+    if (!address || !count || first + count > TEST_FRAME_COUNT) return -1;
+    for (size_t i = 0; i < count; i++) {
+        if (!test_refs[first + i]) return -1;
+    }
+    for (size_t i = 0; i < count; i++) test_refs[first + i]++;
+    return 0;
+}
+
+int frame_release_range(uint64_t address, size_t count)
+{
+    size_t first = address / PAGE_4K_SIZE;
+    if (!address || !count || first + count > TEST_FRAME_COUNT) return -1;
+    for (size_t i = 0; i < count; i++) {
+        if (!test_refs[first + i]) return -1;
+    }
+    for (size_t i = 0; i < count; i++) {
+        if (--test_refs[first + i] == 0) live_allocated_frames--;
+    }
+    return 0;
+}
+
+uint32_t frame_refcount(uint64_t address)
+{
+    size_t frame = address / PAGE_4K_SIZE;
+    return frame < TEST_FRAME_COUNT ? test_refs[frame] : 0;
+}
+
+void free_frame(uint64_t address)
+{
+    (void)frame_release_range(address, 1);
+}
+
+void free_frames(uint64_t address, size_t count)
+{
+    (void)frame_release_range(address, count);
+}
+
+void free_frames_2M(uint64_t address)
+{
+    (void)frame_release_range(address, PAGE_2M_SIZE / PAGE_4K_SIZE);
+}
+
+void free_frames_1G(uint64_t address)
+{
+    (void)frame_release_range(address, PAGE_1G_SIZE / PAGE_4K_SIZE);
+}
+
+static page_directory_t make_directory(void)
+{
+    page_directory_t directory = {0};
+    uint64_t         root      = alloc_frames(1);
+    directory.table            = phys_to_virt(root);
+    page_table_clear(directory.table);
+    return directory;
+}
+
+static page_table_entry_t *leaf_for(page_directory_t *directory, uintptr_t address)
+{
+    page_table_t *table = directory->table;
+    uint64_t      entry = table->entries[(address >> 39) & 0x1ff].value;
+    if (!(entry & PTE_PRESENT)) return 0;
+    table = phys_to_virt(entry & PAGE_4K_MASK);
+
+    page_table_entry_t *leaf = &table->entries[(address >> 30) & 0x1ff];
+    if (leaf->value & PTE_HUGE) return leaf;
+    if (!(leaf->value & PTE_PRESENT)) return 0;
+    table = phys_to_virt(leaf->value & PAGE_4K_MASK);
+
+    leaf = &table->entries[(address >> 21) & 0x1ff];
+    if (leaf->value & PTE_HUGE) return leaf;
+    if (!(leaf->value & PTE_PRESENT)) return 0;
+    table = phys_to_virt(leaf->value & PAGE_4K_MASK);
+    return &table->entries[(address >> 12) & 0x1ff];
+}
+
+static void mark_owned_range(uint64_t address, size_t count)
+{
+    size_t first = address / PAGE_4K_SIZE;
+    for (size_t i = 0; i < count; i++) {
+        test_refs[first + i] = 1;
+        live_allocated_frames++;
+    }
+}
+
+static void reset_backend(void)
+{
+    bytes_set(test_pool, 0, sizeof(test_pool));
+    bytes_set(test_refs, 0, sizeof(test_refs));
+    next_frame            = PAGE_2M_SIZE;
+    allocation_attempts   = 0;
+    allocation_limit      = SIZE_MAX;
+    live_allocated_frames = 0;
+}
+
+static void test_clone_fault_and_lifetime(void)
+{
+    reset_backend();
+    page_directory_t parent = make_directory();
+    page_directory_t child  = make_directory();
+
+    const uintptr_t private_va = 0x400000;
+    const uintptr_t shared_va  = 0x800000;
+    const uintptr_t readonly_va = 0xc00000;
+    uint64_t        private_frame = alloc_frames(1);
+    uint64_t        shared_frame  = alloc_frames(1);
+    uint64_t        readonly_frame = alloc_frames(1);
+
+    bytes_set(phys_to_virt(private_frame), 0x5a, PAGE_4K_SIZE);
+    page_map_to(&parent, private_va, private_frame, PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_NO_EXECUTE);
+    page_map_to(&parent, shared_va, shared_frame, PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_SHARED);
+    page_map_to(&parent, readonly_va, readonly_frame, PTE_PRESENT | PTE_USER | PTE_NO_EXECUTE);
+
+    CHECK(page_clone_user_cow(&child, &parent) == 0);
+
+    page_table_entry_t *parent_private = leaf_for(&parent, private_va);
+    page_table_entry_t *child_private  = leaf_for(&child, private_va);
+    page_table_entry_t *child_shared   = leaf_for(&child, shared_va);
+    page_table_entry_t *child_readonly = leaf_for(&child, readonly_va);
+    CHECK((parent_private->value & (PTE_COW | PTE_WRITEABLE)) == PTE_COW);
+    CHECK((child_private->value & (PTE_COW | PTE_WRITEABLE)) == PTE_COW);
+    CHECK((child_shared->value & (PTE_SHARED | PTE_WRITEABLE | PTE_COW)) == (PTE_SHARED | PTE_WRITEABLE));
+    CHECK((child_readonly->value & (PTE_WRITEABLE | PTE_COW)) == 0);
+    CHECK(frame_refcount(private_frame) == 2);
+    CHECK(frame_refcount(shared_frame) == 2);
+    CHECK(frame_refcount(readonly_frame) == 2);
+
+    page_map_to(&parent, readonly_va, readonly_frame, PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_NO_EXECUTE);
+    CHECK((leaf_for(&parent, readonly_va)->value & (PTE_COW | PTE_WRITEABLE)) == PTE_COW);
+
+    CHECK(page_resolve_cow_fault(&child, private_va) == 0);
+    child_private             = leaf_for(&child, private_va);
+    uint64_t child_frame      = child_private->value & PAGE_4K_MASK;
+    CHECK(child_frame != private_frame);
+    CHECK((child_private->value & PTE_WRITEABLE) != 0);
+    CHECK((child_private->value & PTE_COW) == 0);
+    CHECK(bytes_equal(phys_to_virt(child_frame), phys_to_virt(private_frame), PAGE_4K_SIZE));
+    ((unsigned char *)phys_to_virt(child_frame))[0] = 0xa5;
+    CHECK(((unsigned char *)phys_to_virt(private_frame))[0] == 0x5a);
+    CHECK(frame_refcount(private_frame) == 1);
+
+    page_destroy_user_space(&child);
+    CHECK(frame_refcount(child_frame) == 0);
+    CHECK(frame_refcount(shared_frame) == 1);
+    CHECK(frame_refcount(readonly_frame) == 1);
+
+    size_t before_fault_allocations = allocation_attempts;
+    CHECK(page_resolve_cow_fault(&parent, private_va) == 0);
+    CHECK(allocation_attempts == before_fault_allocations);
+    CHECK((leaf_for(&parent, private_va)->value & (PTE_WRITEABLE | PTE_COW)) == PTE_WRITEABLE);
+
+    page_destroy_user_space(&parent);
+    CHECK(frame_refcount(private_frame) == 0);
+    CHECK(frame_refcount(shared_frame) == 0);
+    CHECK(frame_refcount(readonly_frame) == 0);
+    CHECK(live_allocated_frames == 0);
+}
+
+static void test_huge_leaf_retain_release(void)
+{
+    reset_backend();
+    page_directory_t parent = make_directory();
+    page_directory_t child  = make_directory();
+
+    const uintptr_t va_2m = 0x20000000;
+    const uintptr_t va_1g = 0x40000000;
+    const uint64_t   frame_2m = 0x1000000;
+    const uint64_t   frame_1g = 0x40000000;
+    mark_owned_range(frame_2m, PAGE_2M_SIZE / PAGE_4K_SIZE);
+    mark_owned_range(frame_1g, PAGE_1G_SIZE / PAGE_4K_SIZE);
+    bytes_set(phys_to_virt(frame_2m), 0x33, PAGE_2M_SIZE);
+
+    page_map_to_2M(&parent, va_2m, frame_2m, PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_NO_EXECUTE);
+    page_map_to_1G(&parent, va_1g, frame_1g, PTE_PRESENT | PTE_USER | PTE_WRITEABLE | PTE_PCD);
+    CHECK(page_clone_user_cow(&child, &parent) == 0);
+    CHECK(frame_refcount(frame_2m) == 2);
+    CHECK(frame_refcount(frame_2m + PAGE_2M_SIZE - PAGE_4K_SIZE) == 2);
+    CHECK(frame_refcount(frame_1g) == 2);
+    CHECK(frame_refcount(frame_1g + PAGE_1G_SIZE - PAGE_4K_SIZE) == 2);
+    CHECK((leaf_for(&child, va_2m)->value & (PTE_COW | PTE_WRITEABLE | PTE_NO_EXECUTE)) == (PTE_COW | PTE_NO_EXECUTE));
+    CHECK((leaf_for(&child, va_1g)->value & (PTE_COW | PTE_WRITEABLE | PTE_PCD)) == (PTE_COW | PTE_PCD));
+
+    CHECK(page_resolve_cow_fault(&child, va_2m) == 0);
+    uint64_t child_2m = leaf_for(&child, va_2m)->value & PAGE_2M_MASK;
+    CHECK(child_2m != frame_2m);
+    CHECK(((unsigned char *)phys_to_virt(child_2m))[0] == 0x33);
+    CHECK(((unsigned char *)phys_to_virt(child_2m))[PAGE_2M_SIZE - 1] == 0x33);
+    CHECK(frame_refcount(frame_2m) == 1);
+
+    allocation_limit = allocation_attempts;
+    CHECK(page_resolve_cow_fault(&child, va_1g) != 0);
+    CHECK((leaf_for(&child, va_1g)->value & (PTE_COW | PTE_WRITEABLE)) == PTE_COW);
+    allocation_limit = SIZE_MAX;
+
+    page_destroy_user_space(&child);
+    size_t attempts_before_1g = allocation_attempts;
+    CHECK(page_resolve_cow_fault(&parent, va_1g) == 0);
+    CHECK(allocation_attempts == attempts_before_1g);
+    CHECK((leaf_for(&parent, va_1g)->value & (PTE_COW | PTE_WRITEABLE)) == PTE_WRITEABLE);
+    CHECK(page_unmap_release(&parent, va_2m + PAGE_4K_SIZE) == 0);
+    CHECK(frame_refcount(frame_2m) == 1);
+    CHECK(frame_refcount(frame_2m + PAGE_4K_SIZE) == 0);
+    CHECK(frame_refcount(frame_2m + PAGE_2M_SIZE - PAGE_4K_SIZE) == 1);
+    page_destroy_user_space(&parent);
+    CHECK(frame_refcount(frame_2m) == 0);
+    CHECK(frame_refcount(frame_1g + PAGE_1G_SIZE - PAGE_4K_SIZE) == 0);
+    CHECK(live_allocated_frames == 0);
+}
+
+static void test_clone_allocation_rollback(void)
+{
+    reset_backend();
+    page_directory_t parent = make_directory();
+    page_directory_t child  = make_directory();
+    uint64_t         frame  = alloc_frames(1);
+    page_map_to(&parent, 0x400000, frame, PTE_PRESENT | PTE_USER | PTE_WRITEABLE);
+
+    uint32_t original_refcount = frame_refcount(frame);
+    size_t   baseline_live     = live_allocated_frames;
+    allocation_limit           = allocation_attempts + 2;
+
+    CHECK(page_clone_user_cow(&child, &parent) != 0);
+    CHECK(frame_refcount(frame) == original_refcount);
+    CHECK(leaf_for(&parent, 0x400000)->value & PTE_WRITEABLE);
+    CHECK(!(leaf_for(&parent, 0x400000)->value & PTE_COW));
+    CHECK(child.table->entries[0].value == 0);
+    CHECK(live_allocated_frames == baseline_live);
+
+    allocation_limit = SIZE_MAX;
+    page_destroy_user_space(&child);
+    page_destroy_user_space(&parent);
+    CHECK(live_allocated_frames == 0);
+}
+
+int main(void)
+{
+    test_clone_fault_and_lifetime();
+    test_huge_leaf_retain_release();
+    test_clone_allocation_rollback();
+    return test_failures ? 1 : 0;
+}
```
