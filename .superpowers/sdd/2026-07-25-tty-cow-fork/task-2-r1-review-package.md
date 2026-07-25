# Task 2 fix round 1 review package

Managed checkout note: no commits/index writes are possible; full current task diff follows. Scope to named findings and new fix breakage.

## Diff stat
```text
 Makefile                    | 19 +++++++++++++++++--
 drivers/char/tty.c          | 11 +++++++++++
 drivers/char/tty_core.c     |  6 ++++--
 include/drivers/tty.h       |  2 ++
 include/kernel/elf_loader.h |  1 +
 init/main.c                 | 11 +++++++----
 kernel/process/elf_loader.c | 42 ++++++++++++++++++++++++++++++++----------
 7 files changed, 74 insertions(+), 18 deletions(-)
```

## Tracked diff
```diff
diff --git a/Makefile b/Makefile
index d3ce281..7c13588 100644
--- a/Makefile
+++ b/Makefile
@@ -309,36 +309,51 @@ PWD            := $(shell pwd)
 HOST_CC        ?= $(CC)
 HOST_CFLAGS    := -Wall -Wextra -O2
 QEMU           := qemu-system-x86_64
 QEMU_FLAGS     := -machine q35 -bios assets/ovmf-code.fd -serial stdio
 
 AS             := $(CC)
 ASFLAGS        := -c -m64 -ffreestanding -nostdlib -fno-omit-frame-pointer -I include
 INIT_ELF       := assets/Limine/init
 
 # Automatically find all C source files in tools/ and generate their binary targets
-TOOL_C_SOURCES := $(filter-out tools/ps2_mouse_protocol_test.c,$(wildcard tools/*.c))
+TOOL_C_SOURCES := $(filter-out tools/ps2_mouse_protocol_test.c tools/vm_cow_test.c tools/tty_job_control_test.c,$(wildcard tools/*.c))
 TOOL_TARGETS   := $(TOOL_C_SOURCES:%.c=%)
 PS2_MOUSE_TEST := .cache/ps2_mouse_protocol_test
+VM_COW_TEST    := .cache/vm_cow_test
+TTY_JOB_CONTROL_TEST := .cache/tty_job_control_test
 
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
+test-tty-job-control:
+	$(Q)mkdir -p $(dir $(TTY_JOB_CONTROL_TEST))
+	$(Q)$(HOST_CC) $(HOST_CFLAGS) -fno-builtin -ffunction-sections -fdata-sections -I include \
+		-Wl,--gc-sections -o $(TTY_JOB_CONTROL_TEST) tools/tty_job_control_test.c drivers/char/tty.c drivers/char/tty_core.c \
+		kernel/process/boot_process.c kernel/process/elf_loader_entry.c
+	$(Q)./$(TTY_JOB_CONTROL_TEST)
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
@@ -368,21 +383,21 @@ Uinxed-x64.iso: info UxImage $(INIT_ELF)
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
+.PHONY: help run clean format check gen.clangd menuconfig diskimg test-ps2-mouse test-vm-cow test-tty-job-control
 
 help: info
 	$(Q)printf "Uinxed-Kernel Makefile Usage:\n"
 	$(Q)printf "  make all         - Build the entire project.\n"
 	$(Q)printf "  make run         - Run the Uinxed-x64.iso in QEMU.\n"
 	$(Q)printf "  make disk.img    - Build a demo simplefs disk image.\n"
 	$(Q)printf "  make clean       - Clean all generated files.\n"
 	$(Q)printf "  make format      - Format all source files using clang-format.\n"
 	$(Q)printf "  make check       - Run static code checks using clang-tidy.\n"
 	$(Q)printf "  make gen.clangd  - Generate .clangd configuration file.\n"
diff --git a/drivers/char/tty.c b/drivers/char/tty.c
index 5483a63..27d38c7 100644
--- a/drivers/char/tty.c
+++ b/drivers/char/tty.c
@@ -504,20 +504,31 @@ int tty_dev_poll(void *ctx, size_t events)
 
 int tty_dev_file_open(struct vfs_node *node, uint64_t flags, void **private_data)
 {
     (void)node;
     tty_input_lazy_init();
     tty_core_auto_acquire(&console_tty, flags);
     *private_data = NULL;
     return 0;
 }
 
+int tty_console_acquire(struct process *proc, uint64_t flags)
+{
+    if (!proc || !proc->task || (flags & (O_NOCTTY | O_PATH)) || (flags & O_ACCMODE) == O_WRONLY) return -EINVAL;
+
+    pid_t pid = (pid_t)proc->task->pid;
+    if (pid <= 0 || proc->sid != pid || proc->pgid != pid) return -EPERM;
+
+    tty_input_lazy_init();
+    return process_ctty_acquire(proc, &console_tty, false, NULL, NULL);
+}
+
 int64_t tty_dev_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
 {
     (void)ctx;
     (void)private_data;
     (void)offset;
     tty_input_lazy_init();
     return tty_core_read(&console_tty, addr, size, flags);
 }
 
 int64_t tty_dev_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
diff --git a/drivers/char/tty_core.c b/drivers/char/tty_core.c
index 411a178..78959eb 100644
--- a/drivers/char/tty_core.c
+++ b/drivers/char/tty_core.c
@@ -65,22 +65,24 @@ static bool tty_current_associated(tty_core_t *tty, process_t *current)
 static int tty_job_control_check(tty_core_t *tty, int signal)
 {
     process_t *current = process_current();
     if (!current || current->pgid <= 0 || !tty_current_associated(tty, current)) return 0;
 
     spin_lock(&tty->lock);
     bool background = tty->session == current->sid && tty->foreground_pgid != current->pgid;
     spin_unlock(&tty->lock);
     if (!background) return 0;
 
-    if (signal_is_blocked_or_ignored(current, signal) || process_pgrp_is_orphaned(current->pgid, current->sid)) return -EIO;
-    signal_send_pgrp(current->pgid, signal);
+    bool blocked_or_ignored = signal_is_blocked_or_ignored(current, signal);
+    if (signal == SIGTTOU && blocked_or_ignored) return 0;
+    if (blocked_or_ignored || process_pgrp_is_orphaned(current->pgid, current->sid)) return -EIO;
+    signal_send_pgrp_session(current->pgid, current->sid, signal);
     return -EINTR;
 }
 
 static bool tty_signal_pending(process_t *current)
 {
     if (!current) return false;
     spin_lock(&current->signal.lock);
     bool pending = signal_has_pending(&current->signal);
     spin_unlock(&current->signal.lock);
     return pending;
diff --git a/include/drivers/tty.h b/include/drivers/tty.h
index 49b561e..ebf5720 100644
--- a/include/drivers/tty.h
+++ b/include/drivers/tty.h
@@ -9,20 +9,21 @@
  */
 
 #ifndef INCLUDE_TTY_H_
 #define INCLUDE_TTY_H_
 
 #include <libs/std/stdbool.h>
 #include <libs/std/stdint.h>
 #include <libs/std/stdlib.h>
 
 struct vfs_node;
+struct process;
 
 #define MAX_ARGC    1024
 #define MAX_CMDLINE 256
 
 #ifndef TTY_BUF_SIZE
 #    define TTY_BUF_SIZE 4096
 #endif
 
 #ifndef TTY_DEFAULT_DEV
 #    define TTY_DEFAULT_DEV "tty0"
@@ -78,20 +79,21 @@ size_t tty_dev_write(void *ctx, const void *addr, size_t offset, size_t size);
 size_t tty_dev_read(void *ctx, void *addr, size_t offset, size_t size);
 
 /* Poll TTY device for write readiness */
 int tty_dev_poll(void *ctx, size_t events);
 
 int     tty_dev_file_open(struct vfs_node *node, uint64_t flags, void **private_data);
 int64_t tty_dev_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size);
 int64_t tty_dev_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size);
 int     tty_dev_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events);
 int     tty_dev_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t request, void *arg);
+int     tty_console_acquire(struct process *proc, uint64_t flags);
 int     tty_ctty_file_open(struct vfs_node *node, uint64_t flags, void **private_data);
 void    tty_ctty_file_release(struct vfs_node *node, void *private_data);
 int64_t tty_ctty_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size);
 int64_t tty_ctty_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size);
 int     tty_ctty_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events);
 int     tty_ctty_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t request, void *arg);
 
 /* Feed a scancode from a keyboard into the TTY input line discipline */
 void tty_handle_scancode(uint8_t scancode, bool pressed);
 
diff --git a/include/kernel/elf_loader.h b/include/kernel/elf_loader.h
index c4a3192..ed0bf05 100644
--- a/include/kernel/elf_loader.h
+++ b/include/kernel/elf_loader.h
@@ -29,14 +29,15 @@ typedef struct {
         int        has_interp;
         char       interp_path[256];
         Elf64_Addr tls_align;
         Elf64_Addr tls_size;
         Elf64_Addr tls_vaddr;
         Elf64_Addr pt_dynamic_vaddr;
 } elf_load_info_t;
 
 int elf_loader_load_user_process(struct process *proc, const uint8_t *elf_data, size_t elf_size, char *const argv[], char *const envp[],
                                  uintptr_t *entry_out, uintptr_t *rsp_out);
+int elf_loader_load_initial_process(struct process *proc, const uint8_t *elf_data, size_t elf_size, char *const argv[], char *const envp[]);
 int elf_loader_parse_elf_info(const uint8_t *elf_data, size_t elf_size, elf_load_info_t *info);
 int elf_loader_load_interpreter(struct process *proc, const char *interp_path, Elf64_Addr *base_out, Elf64_Addr *entry_out);
 
 #endif /* INCLUDE_ELF_LOADER_H_ */
diff --git a/init/main.c b/init/main.c
index 7942445..607322c 100644
--- a/init/main.c
+++ b/init/main.c
@@ -42,20 +42,21 @@
 #include <fs/procfs.h>
 #include <fs/sysfs.h>
 #include <fs/tmpfs.h>
 #include <fs/vfs.h>
 #include <ipc/epoll.h>
 #include <ipc/futex.h>
 #include <ipc/netlink.h>
 #include <ipc/posix_mq.h>
 #include <ipc/socket.h>
 #include <ipc/sysv_ipc.h>
+#include <kernel/boot_process.h>
 #include <kernel/cmdline.h>
 #include <kernel/debug.h>
 #include <kernel/device.h>
 #include <kernel/elf_loader.h>
 #include <kernel/errno.h>
 #include <kernel/interrupt.h>
 #include <kernel/printk.h>
 #include <kernel/uinxed.h>
 #include <mem/frame.h>
 #include <mem/heap.h>
@@ -83,24 +84,27 @@ extern void       tty_sysfs_init(void);
 
 /* Create init process */
 void swapper_run_init(void)
 {
     lmodule_t *init_mod = get_lmodule("init");
     if (!init_mod || !init_mod->data || init_mod->size == 0) panic("No working init found.");
     plogk("swapper/0: Found init module at %p, size %zu bytes.\n", init_mod->data, init_mod->size);
 
     process_t *init = process_create("init", NULL, NULL);
     if (!init) panic("Failed to create init process.");
-    init_process = init;
+    if (!init->task || init->task->pid != 1) panic("User init did not receive PID 1.");
+    init_process   = init;
+    pid_t init_sid = 0;
+    if (process_setsid(init, &init_sid) || init_sid != 1 || init->pgid != 1) { panic("Failed to establish init session."); }
 
     char *init_argv[] = {"/init", NULL};
-    if (elf_loader_load_user_process(init, init_mod->data, init_mod->size, init_argv, NULL, NULL, NULL)) panic("Failed to load init ELF!");
+    if (elf_loader_load_initial_process(init, init_mod->data, init_mod->size, init_argv, NULL)) panic("Failed to load init ELF!");
 
     spin_lock(&scheduler.lock);
     enqueue_task(init->task);
     spin_unlock(&scheduler.lock);
     request_task_cpu(init->task);
 
     for (uint32_t i = 0; i < sched_cpu_count(); i++) {
         if (cpu_rqs[i].idle) cpu_rqs[i].idle->process = init;
     }
     plogk("swapper/0: Init process (pid=1) ready.\n");
@@ -229,16 +233,15 @@ void kernel_entry(void)
     pipe_init();     // Pipes
     sysv_ipc_init(); // System V IPC
     posix_mq_init(); // POSIX Message Queues
     epoll_init();    // Epoll
     futex_init();    // Futexes
     eventfd_init();  // Event File Descriptor
     timerfd_init();  // Timer File Descriptor
     signalfd_init(); // Signal File Descriptor
     mmap_init();     // Memory Map
 
-    sched_test_init();
-    swapper_run_init();
+    boot_start_init_before_debug(swapper_run_init, sched_test_init);
 
     enable_intr();
     sched_start();
 }
diff --git a/kernel/process/elf_loader.c b/kernel/process/elf_loader.c
index 2173362..5dae829 100644
--- a/kernel/process/elf_loader.c
+++ b/kernel/process/elf_loader.c
@@ -1,24 +1,34 @@
+/*
+ * ELF64 process-image loader.
+ *      elf_loader.c
+ *      ELF loader for user processes
+ *      2026/7/21 By Rainy101112
+ *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
+ */
+// 上面那个是原作者的注释，codex把他给删了我加上了 不知道对不对（
+#include <drivers/tty.h>
 #include <fs/vfs.h>
 #include <kernel/elf.h>
 #include <kernel/elf_loader.h>
 #include <kernel/printk.h>
 #include <libs/std/stdlib.h>
 #include <libs/std/string.h>
 #include <mem/alloc.h>
 #include <mem/frame.h>
 #include <mem/heap.h>
 #include <mem/hhdm.h>
 #include <mem/page.h>
 #include <proc/process.h>
 #include <proc/sched.h>
 #include <proc/uaccess.h>
+#include <syscall/fcntl.h>
 #include <syscall/syscall.h>
 
 #define INTERP_LOAD_BASE 0x7f0000000000ULL
 #define INTERP_LOAD_END  0x7f0001000000ULL
 
 static int validate_elf(const uint8_t *data, size_t size, Elf64_Ehdr **ehdr_out)
 {
     if (size < sizeof(Elf64_Ehdr)) return -1;
     Elf64_Ehdr *ehdr = (Elf64_Ehdr *)data;
     if (*(const uint32_t *)ehdr->e_ident != ELF_MAGIC) return -1;
@@ -429,22 +439,22 @@ __attribute__((naked)) static void user_process_enter(void)
                      "xorl %edx, %edx\n\t"
                      "xorl %esi, %esi\n\t"
                      "xorl %edi, %edi\n\t"
                      "xorl %r8d, %r8d\n\t"
                      "xorl %r9d, %r9d\n\t"
                      "xorl %r10d, %r10d\n\t"
                      "xorl %r11d, %r11d\n\t"
                      "iretq\n\t");
 }
 
-int elf_loader_load_user_process(process_t *proc, const uint8_t *elf_data, size_t elf_size, char *const argv[], char *const envp[],
-                                 uintptr_t *entry_out, uintptr_t *rsp_out)
+int elf_loader_load_process_internal(process_t *proc, const uint8_t *elf_data, size_t elf_size, char *const argv[], char *const envp[],
+                                     uintptr_t *entry_out, uintptr_t *rsp_out, bool acquire_console)
 {
     Elf64_Ehdr *ehdr = NULL;
     if (validate_elf(elf_data, elf_size, &ehdr)) {
         plogk("elf_loader: Invalid ELF binary.\n");
         return 1;
     }
 
     elf_load_info_t info;
     if (elf_loader_parse_elf_info(elf_data, elf_size, &info)) {
         plogk("elf_loader: Failed to parse ELF info.\n");
@@ -480,29 +490,41 @@ int elf_loader_load_user_process(process_t *proc, const uint8_t *elf_data, size_
     if (load_elf_segments(proc, ehdr, elf_data, elf_size, load_bias, 1)) {
         plogk("elf_loader: Failed to load ELF segments.\n");
         return 1;
     }
 
     if (process_mmap(proc, proc->stack_brk, PROCESS_STACK_SIZE, VM_READ | VM_WRITE)) {
         plogk("elf_loader: Failed to allocate user stack.\n");
         return 1;
     }
 
-    vfs_node_t console = vfs_open("/dev/console");
-    if (console) {
-        int std_fd = process_fd_install(proc, console, O_RDWR);
-        if (std_fd == 0) {
-            process_fd_dup2(proc, 0, 1);
-            process_fd_dup2(proc, 0, 2);
+    if (acquire_console) {
+        vfs_node_t console = vfs_open("/dev/console");
+        if (!console) {
+            plogk("elf_loader: PID 1 cannot open /dev/console.\n");
+            return 1;
+        }
+
+        int std_fd = process_fd_install(proc, console, O_RDWR | O_NOCTTY);
+        if (std_fd != 0) {
+            if (std_fd < 0) vfs_close(console);
+            plogk("elf_loader: PID 1 failed to install /dev/console on standard input.\n");
+            return 1;
+        }
+
+        int stdout_fd = process_fd_dup2(proc, 0, 1);
+        int stderr_fd = process_fd_dup2(proc, 0, 2);
+        int ctty      = tty_console_acquire(proc, O_RDWR);
+        if (stdout_fd != 1 || stderr_fd != 2 || ctty) {
+            plogk("elf_loader: PID 1 failed to acquire /dev/console as its controlling terminal.\n");
+            return 1;
         }
-    } else {
-        plogk("elf_loader: warning - /dev/console not found.\n");
     }
 
     proc->task->thread.fs_base = 0;
     proc->task->thread.gs_base = 0;
 
     Elf64_Addr interpreter_base  = 0;
     Elf64_Addr interpreter_entry = 0;
     uintptr_t  actual_entry      = ehdr->e_entry + load_bias;
 
     if (info.has_interp) {
```

## New test
```diff
diff --git a/tools/tty_job_control_test.c b/tools/tty_job_control_test.c
new file mode 100644
index 0000000..985f288
--- /dev/null
+++ b/tools/tty_job_control_test.c
@@ -0,0 +1,341 @@
+/*
+ * Host regression for boot-console ownership and TTY job control.
+ *
+ * Include the real process implementation so the harness can populate its
+ * private process table, then exercise the production session/ctty helpers.
+ */
+
+#include "../kernel/process/process.c"
+#include <drivers/tty.h>
+#include <drivers/tty_core.h>
+#include <kernel/boot_process.h>
+#include <kernel/elf_loader.h>
+#include <kernel/termios.h>
+#include <syscall/fcntl.h>
+
+static task_t *host_current_task;
+static int     test_failures;
+static int     test_failure_line;
+static int     scoped_signal_count;
+static int     unscoped_signal_count;
+static pid_t   last_signal_pgid;
+static pid_t   last_signal_sid;
+static int     last_signal;
+static bool    host_signal_blocked_or_ignored;
+static pid_t   simulated_next_pid;
+static pid_t   simulated_init_pid;
+static pid_t   simulated_debug_pid;
+static int     loader_call_count;
+static bool    loader_requested_console;
+
+#define CHECK(expr)                                               \
+    do {                                                          \
+        if (!(expr)) {                                            \
+            test_failures++;                                      \
+            if (!test_failure_line) test_failure_line = __LINE__; \
+        }                                                         \
+    } while (0)
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
+void wait_queue_init(wait_queue_t *queue)
+{
+    memset(queue, 0, sizeof(*queue));
+}
+
+uint64_t wait_queue_wake_all(wait_queue_t *queue)
+{
+    (void)queue;
+    return 0;
+}
+
+void wait_queue_prepare(wait_queue_t *queue)
+{
+    (void)queue;
+}
+
+void wait_queue_sleep(void)
+{
+}
+
+int wait_queue_wait_timed(wait_queue_t *queue, uint64_t deadline_ticks)
+{
+    (void)queue;
+    (void)deadline_ticks;
+    return 0;
+}
+
+task_t *current_task(void)
+{
+    return host_current_task;
+}
+
+uint64_t sched_ticks(void)
+{
+    return 0;
+}
+
+int copy_from_user(void *dst, const void *src, size_t size)
+{
+    memcpy(dst, src, size);
+    return 0;
+}
+
+int copy_to_user(void *dst, const void *src, size_t size)
+{
+    memcpy(dst, src, size);
+    return 0;
+}
+
+bool signal_is_blocked_or_ignored(process_t *proc, int sig)
+{
+    (void)proc;
+    (void)sig;
+    return host_signal_blocked_or_ignored;
+}
+
+int signal_has_pending(signal_state_t *state)
+{
+    (void)state;
+    return 0;
+}
+
+int signal_send_pgrp(int64_t pgid, int sig)
+{
+    unscoped_signal_count++;
+    last_signal_pgid = pgid;
+    last_signal      = sig;
+    return 0;
+}
+
+int signal_send_pgrp_session(int64_t pgid, int64_t sid, int sig)
+{
+    scoped_signal_count++;
+    last_signal_pgid = pgid;
+    last_signal_sid  = sid;
+    last_signal      = sig;
+    return 0;
+}
+
+const char *get_cmdline(void)
+{
+    return NULL;
+}
+
+void write_serial(uint16_t port, char data)
+{
+    (void)port;
+    (void)data;
+}
+
+void fbcon_put_string(const char *string)
+{
+    (void)string;
+}
+
+int elf_loader_load_process_internal(process_t *proc, const uint8_t *elf_data, size_t elf_size, char *const argv[], char *const envp[],
+                                     uintptr_t *entry_out, uintptr_t *rsp_out, bool acquire_console)
+{
+    (void)proc;
+    (void)elf_data;
+    (void)elf_size;
+    (void)argv;
+    (void)envp;
+    (void)entry_out;
+    (void)rsp_out;
+    loader_call_count++;
+    loader_requested_console = acquire_console;
+    return 0;
+}
+
+static void simulate_start_init(void)
+{
+    simulated_init_pid = simulated_next_pid++;
+}
+
+static void simulate_start_debug(void)
+{
+    simulated_debug_pid = simulated_next_pid++;
+}
+
+static void reset_signal_record(void)
+{
+    scoped_signal_count   = 0;
+    unscoped_signal_count = 0;
+    last_signal_pgid      = 0;
+    last_signal_sid       = 0;
+    last_signal           = 0;
+}
+
+static void install_process(process_t *proc, task_t *task, pid_t pid, pid_t sid, pid_t pgid)
+{
+    memset(proc, 0, sizeof(*proc));
+    memset(task, 0, sizeof(*task));
+    task->pid          = (uint64_t)pid;
+    task->process      = proc;
+    proc->task         = task;
+    proc->sid          = sid;
+    proc->pgid         = pgid;
+    proc->refcount     = 1;
+    process_table[pid] = proc;
+}
+
+static void test_boot_console_and_job_control(void)
+{
+    process_t init;
+    process_t foreground;
+    process_t inherited;
+    process_t wrong_session;
+    process_t kernel_current;
+    task_t    init_task;
+    task_t    foreground_task;
+    task_t    inherited_task;
+    task_t    wrong_session_task;
+    task_t    kernel_task;
+
+    memset(process_table, 0, sizeof(process_table));
+    install_process(&init, &init_task, 1, 0, 0);
+    install_process(&kernel_current, &kernel_task, 4, 4, 4);
+    host_current_task = &kernel_task;
+
+    CHECK(process_setsid(&init, NULL) == 0);
+    CHECK(init.sid == 1);
+    CHECK(init.pgid == 1);
+
+    void *open_private = NULL;
+    CHECK(tty_dev_file_open(NULL, O_RDWR | O_NOCTTY, &open_private) == 0);
+    CHECK(kernel_current.controlling_tty == NULL);
+    CHECK(tty_console_acquire(&init, O_RDWR) == 0);
+    tty_core_t *console = process_ctty_get(&init);
+    CHECK(console != NULL);
+    CHECK(init.controlling_tty == console);
+    CHECK(kernel_current.controlling_tty == NULL);
+    CHECK(console->session == 1);
+    CHECK(console->foreground_pgid == 1);
+
+    install_process(&foreground, &foreground_task, 2, 1, 2);
+    foreground.parent = &init;
+    process_ctty_inherit(&foreground, &init);
+    install_process(&inherited, &inherited_task, 3, 1, 2);
+    inherited.parent = &foreground;
+    process_ctty_inherit(&inherited, &init);
+    CHECK(inherited.controlling_tty == console);
+
+    host_current_task = &init_task;
+    int value         = 2;
+    CHECK(tty_core_ioctl(console, O_RDWR, TIOCSPGRP, &value) == 0);
+    CHECK(console->foreground_pgid == 2);
+    value = 0;
+    CHECK(tty_core_ioctl(console, O_RDWR, TIOCGPGRP, &value) == 0);
+    CHECK(value == 2);
+
+    host_current_task = &foreground_task;
+    value             = 0;
+    CHECK(tty_core_ioctl(console, O_RDWR, TIOCSPGRP, &value) == -EPERM);
+    install_process(&wrong_session, &wrong_session_task, 5, 5, 5);
+    value = 5;
+    CHECK(tty_core_ioctl(console, O_RDWR, TIOCSPGRP, &value) == -EPERM);
+
+    console->termios.c_lflag &= ~ECHO;
+    const uint8_t controls[] = {3, 28, 26};
+    const int     signals[]  = {SIGINT, SIGQUIT, SIGTSTP};
+    for (size_t i = 0; i < sizeof(controls); i++) {
+        reset_signal_record();
+        CHECK(tty_core_receive(console, &controls[i], 1, O_NONBLOCK) == 1);
+        CHECK(scoped_signal_count == 1);
+        CHECK(unscoped_signal_count == 0);
+        CHECK(last_signal_pgid == 2);
+        CHECK(last_signal_sid == 1);
+        CHECK(last_signal == signals[i]);
+    }
+
+    reset_signal_record();
+    init.parent       = &foreground;
+    host_current_task = &init_task;
+    uint8_t byte      = 0;
+    CHECK(tty_core_read(console, &byte, 1, O_NONBLOCK) == -EINTR);
+    CHECK(scoped_signal_count == 1);
+    CHECK(unscoped_signal_count == 0);
+    CHECK(last_signal_pgid == 1);
+    CHECK(last_signal_sid == 1);
+    CHECK(last_signal == SIGTTIN);
+
+    reset_signal_record();
+    console->termios.c_lflag |= TOSTOP;
+    CHECK(tty_core_write(console, &byte, 1, O_NONBLOCK) == -EINTR);
+    CHECK(scoped_signal_count == 1);
+    CHECK(unscoped_signal_count == 0);
+    CHECK(last_signal_pgid == 1);
+    CHECK(last_signal_sid == 1);
+    CHECK(last_signal == SIGTTOU);
+
+    reset_signal_record();
+    value = 2;
+    CHECK(tty_core_ioctl(console, O_RDWR, TIOCSPGRP, &value) == -EINTR);
+    CHECK(scoped_signal_count == 1);
+    CHECK(unscoped_signal_count == 0);
+    CHECK(last_signal_pgid == 1);
+    CHECK(last_signal_sid == 1);
+    CHECK(last_signal == SIGTTOU);
+
+    host_signal_blocked_or_ignored = true;
+    reset_signal_record();
+    CHECK(tty_core_write(console, &byte, 1, O_NONBLOCK) == 1);
+    CHECK(scoped_signal_count == 0);
+    CHECK(unscoped_signal_count == 0);
+
+    reset_signal_record();
+    value = 2;
+    CHECK(tty_core_ioctl(console, O_RDWR, TIOCSPGRP, &value) == 0);
+    CHECK(scoped_signal_count == 0);
+    CHECK(unscoped_signal_count == 0);
+    host_signal_blocked_or_ignored = false;
+
+    tty_core_release(console);
+}
+
+static void test_boot_order_and_loader_modes(void)
+{
+    simulated_next_pid  = 1;
+    simulated_init_pid  = 0;
+    simulated_debug_pid = 0;
+    boot_start_init_before_debug(simulate_start_init, simulate_start_debug);
+    CHECK(simulated_init_pid == 1);
+    CHECK(simulated_debug_pid == 2);
+
+    process_t init;
+    process_t other;
+    task_t    init_task;
+    task_t    other_task;
+    install_process(&init, &init_task, 1, 1, 1);
+    install_process(&other, &other_task, 2, 2, 2);
+
+    loader_call_count        = 0;
+    loader_requested_console = false;
+    CHECK(elf_loader_load_initial_process(&init, NULL, 0, NULL, NULL) == 0);
+    CHECK(loader_call_count == 1);
+    CHECK(loader_requested_console);
+
+    CHECK(elf_loader_load_initial_process(&other, NULL, 0, NULL, NULL) != 0);
+    CHECK(loader_call_count == 1);
+
+    loader_requested_console = true;
+    CHECK(elf_loader_load_user_process(&init, NULL, 0, NULL, NULL, NULL, NULL) == 0);
+    CHECK(loader_call_count == 2);
+    CHECK(!loader_requested_console);
+}
+
+int main(void)
+{
+    test_boot_console_and_job_control();
+    test_boot_order_and_loader_modes();
+    return test_failures ? (test_failure_line & 0xff) : 0;
+}
```
