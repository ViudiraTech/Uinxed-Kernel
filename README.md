<div align="center"> 
  <img src="https://github.com/user-attachments/assets/cb3f4ec8-4504-4fe9-b402-8d1588a986a8" height="200" width="200"/>
  <h1 align="center">Uinxed</h1>
  <h3 align="center">Welcome to the Uinxed project</h3>
</div>

<div align="center">
  <img src="https://img.shields.io/badge/License-Apache2.0-blue"/>
  <img src="https://img.shields.io/badge/Language-C-orange"/>
  <img src="https://img.shields.io/badge/Hardware-x64-green"/>
  <img src="https://img.shields.io/badge/Firmware-UEFI/Legacy-yellow"/>
  <a href="https://deepwiki.com/ViudiraTech/Uinxed-Kernel"><img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki"></a>
</div>

## Overview 💡

Uinxed is a monolithic UNIX-like x86_64 kernel, version 0.4.0, written from scratch in C. It boots via the Limine bootloader, supports SMP, and implements a Linux-compatible syscall ABI (440 syscalls matching Linux numbering). The kernel emphasizes modern scheduler design (EEVDF), comprehensive memory management including swap, and a full VFS/driver stack.

## Architecture Overview

**Boot Sequence** (`init/main.c:kernel_entry`)

1. Limine bootloader hands off to `kernel_entry()`
2. Early init: FPU/SSE → serial → physical frame allocator → 4-level paging → kernel heap → Limine modules
3. Video (VESA/GOP framebuffer) → GDT → IDT → ISR registration → syscall init → AVX
4. ACPI → TSC → SMP init (APs brought online)
5. PCI enumeration → SB16 audio → IDE → parallel port → PS/2
6. VFS init → tmpfs, FAT, cpio, devtmpfs, procfs, DRM, VirtIO-GPU
7. Scheduler init (EEVDF) → process management → eventfd/timerfd/signalfd → mmap
8. `swapper_run_init()` loads the `init` ELF from a Limine module
9. Interrupts enabled → `sched_start()` (does not return)

### Memory Management

- **Frame allocator** (`mem/frame`, `mem/buddy`): binary buddy free areas with eager splitting/coalescing, exact-length contiguous allocations, aligned huge-page allocations, and per-frame ownership counts
- **Page tables** (`mem/page`): standard x86_64 4-level paging (PML4 → PDPT → PD → PT). Supports 4KB, 2MB, and 1GB huge pages
- **HHDM** (Higher Half Direct Map): all physical memory accessible via `phys_to_virt()`
- **Kernel heap** (`mem/heap`, `mem/alloc`): buddy-backed slab caches with partial/full/empty slab lists, cache coloring, typed caches, aligned large allocations, reclamation, poisoning, statistics, and invariant checks
- **KASLR**: kernel address space layout randomization
- **Swap** (`mem/swap`): anonymous memory swap subsystem with multiple swap areas, slot allocation, reclaim, and swap-in/swap-out fault handling

### Scheduler (EEVDF)

- Earliest Eligible Virtual Deadline First scheduler (`kernel/sched/`)
- Per-CPU runqueues (`eevdf_rq_t`) with red-black tree timeline sorted by virtual deadline
- SMP-aware: `choose_task_cpu_locked()` for initial placement, `task_set_cpu()` for migration
- Wait queues with two-phase prepare/sleep to avoid lost wakeups, plus timed waits
- Preemption via IPI (`sched_ipi_reschedule`)
- Global `scheduler` struct holds sleep queue and timer queue (intrusive linked lists)
- Priority Inheritance (PI) for robust mutex/futex semantics

### Process & Task Model

- `task_t` (`proc/task.h`): the schedulable entity. Contains CPU context (`task_context_t`), thread-local arch state (`thread_struct_t`: fs_base/gs_base), page directory pointer, kernel stack (64KB), EEVDF fields (vruntime, deadline, vlag, weight)
- `process_t` (`proc/process.h`): holds the task, VMA list, file descriptor table, signal state, credentials
- Task states: READY → RUNNING ↔ BLOCKED/SLEEPING → ZOMBIE; plus IDLE (per-CPU idle tasks)
- Kernel threads via `kthread_create()` / `kthread_create_on_cpu()`
- User processes loaded via `elf_loader_load_user_process()` from Limine modules
- `ptrace` support: Linux-compatible process tracing with events, single-step, seccomp (`proc/ptrace.h`)
- cgroups: control groups with pids controller (`cgroup/cgroup.h`)

### Syscall Interface

- Linux x86_64 ABI compatibility: syscalls invoked via `syscall` instruction with RAX=syscall number
- 440 syscalls defined in `include/syscall/syscall_table.h` (enum `x86_syscall_table`)
- Syscall handler in `kernel/syscall/` dispatches through a function pointer table
- Not all 440 syscalls are implemented; unimplemented ones return `-ENOSYS`

### Virtual Filesystem (VFS)

- UNIX-like VFS with mount points (`fs/vfs`)
- Filesystem callbacks: mount, unmount, open, close, read, write, readlink, mkdir, mkfile, link, symlink, stat, ioctl, dup, poll, free, delete, rename, mmap
- `vfs_node_t`: inode-like structure with parent/child linkage (circular linked list), reference counting, permissions (owner/group/mode), timestamps
- Supported filesystems: tmpfs (root), FAT12/16/32 (via FatFS library), procfs, devtmpfs, cpio
- File types distinguished by bitmask flags (`file_dir`, `file_block`, `file_stream`, `file_symlink`, `file_fbdev`, `file_pipe`, `file_socket`, `file_epoll`, etc.)
- TTY core with line discipline, controlling terminal, foreground process group
- Unix98 PTY support (master/slave pairs, `/dev/ptmx`, `/dev/ttyX`, `/dev/pts/X`)

### DRM Subsystem

- Linux-compatible Direct Rendering Manager (`drivers/gpu/drm/`) with KMS (Kernel Mode Setting)
- Atomic modesetting (`drm_atomic_uapi.c`), framebuffer management, connector/CRTC/encoder/plane model
- Device nodes exposed via VFS at `/dev/dri/card0` and `/dev/dri/renderD*`
- VirtIO-GPU driver (`drivers/virt/gpu/`) uses the DRM framework for paravirtualized GPU access

## Core Features 🌟

- **x86_64 architecture support**: optimized for modern 64-bit x86 processors
- **UEFI boot**: uses UEFI as the boot mode to support modern hardware platforms
- **Legacy boot**: Compatible with traditional Legacy boot
- **KASLR**: Kernel address space layout randomization to enhance security.
- **Memory management**:
  - Physical memory frame allocator
  - Virtual memory page management
  - High half memory mapping (HHDM)
  - Unified page cache with page locking, LRU reclaim, dirty-page writeback, readahead, truncation, and statistics
  - Swap subsystem for anonymous memory (swap areas, slot allocation, reclaim, swap-in/swap-out fault handling)
- **Interrupt management**:
  - Complete interrupt descriptor table (IDT) implementation
  - Exception interrupt handlers for both kernel and userspace
  - Advanced Programmable Interrupt Controller (APIC) support
- **System management**:
  - ACPI support
  - High Precision Event Timer (HPET)
  - Multi-core support based on symmetric multi-processing (SMP)
  - ALSA-like Audio subsystem
  - `evdev` for event devices
  - `DRM` (Direct rendering manager) subsystem
  - IPC (Inter-Process Communication)
  - Linux-style USB device, interface, bus, and sysfs integration
  - `ptrace` (Linux-compatible process tracing with events, single-step, seccomp)
  - cgroups (control groups with pids controller)
- **Console measures**:
  - Bitmap fonts (9x16 pixels)
  - High-speed framebuffer console implementation
  - TTY core with line discipline, controlling terminal, foreground process group
  - Unix98 PTY support (master/slave pairs, `/dev/ptmx`, `/dev/ttyX`, `/dev/pts/X`)
  - POSIX termios and Linux TTY ioctl ABI
- **Networking**:
  - Intel e1000/e1000e Gigabit Ethernet driver (82540EM, 82545EM, 82546EB, 82541PI, 82574L)
  - Generic network device abstraction
  - Ethernet/ARP/IPv4/ICMP/UDP/TCP protocol stack
  - IPv6 with NDP (Neighbor Discovery Protocol)
  - Linux AF_INET / AF_INET6 socket ABI (SOCK_DGRAM / SOCK_STREAM)
  - `/proc/net/{dev,arp,route,tcp,udp}`
  - `/sys/class/net/<iface>/` with Linux-style attributes
- **Filesystem**:
  - UNIX-like virtual filesystem (VFS)
  - tmpfs (default root filesystem)
  - FatFS (FAT12, FAT16, FAT32)
  - NTFS with write support
  - ISO 9660
  - procfs, sysfs, devtmpfs, cgroupfs
  - SimpleFS for test
  - VFS-backed page-cache mappings for regular files and memory-mapped I/O
- **Scheduler**:
  - EEVDF (Earliest Eligible Virtual Deadline First) scheduler with per-CPU runqueues
  - Red-black tree timeline sorted by virtual deadline (`vruntime`, `deadline`, `vlag`, `weight`)
  - SMP-aware load balancing, CPU migration, and IPI-based preemption
  - Priority Inheritance (PI) support for robust mutex/ futex semantics
  - Wait queues with two-phase prepare/sleep to eliminate lost wakeups
  - Timed waits backed by the scheduler timer queue
- **ABI**:
  - Linux syscalls (440 syscalls matching x86-64 numbering)
  - Linux AF_UNIX / AF_NETLINK / AF_INET / AF_INET6 sockets
  - epoll, eventfd, signalfd, timerfd
  - POSIX message queues, System V IPC, futex with Priority Inheritance (PI)
  - `mmap` / `munmap` / `mremap` with VFS-backed page-cache mappings
  - `ptrace` (TRACEME, ATTACH, PEEKTEXT/POKETEXT, GETREGS, SYSCALL, seccomp, events)
  - POSIX termios and Linux TTY ioctl ABI (including Unix98 PTY ioctls)
- **Drivers**:
  - Network
    1. Intel e1000/e1000e (82540EM, 82545EM, 82546EB, 82541PI, 82574L)
  - Audio
    1. Sound Blaster 16
    2. Intel HD Audio
  - Input
    1. PS/2 keyboard
    2. PS/2 mouse
    3. evdev subsystem
    4. USB HID report-descriptor parser with USB keyboard, mouse, and consumer-control input
  - Storage
    1. ATA/ATAPI (IDE)
    2. AHCI (SATA)
    3. NVMe
    4. USB Mass Storage Bulk-Only Transport with SCSI READ/WRITE and removable `/dev/sdX` disks
  - GPU
    1. DRM (Direct Rendering Manager) / KMS
    2. VirtIO-GPU (paravirtualized)
  - Bus
    1. PCI/PCIe (ECAM + legacy configuration)
    2. PCI xHCI host controller with control, bulk, interrupt, root-port enumeration, and hotplug handling
  - Port
    1. Standard serial port (RS232)
    2. Standard parallel port (IEEE 1284)
  - Security
    1. Trusted Platform Module (TPM 1.2/2.0, TIS + CRB)
  - Video
    1. VESA/GOP framebuffer
  - Virtualization
    1. VirtIO (PCI transport)
- **Userspace**:
  - `init`
    1. Load `init` from Limine module.

## Development Environment Preparation 🛠️

### Required Tools

1. **make**: Used to build projects.
2. **gcc**: GCC Version 13.3.0+ is recommended.
3. **qemu**: Used for simulation testing.
4. **xorriso**: Used to build ISO image files.
5. **clang-format**: Used to format the code.
6. **clang-tidy**: Used for static analysis of code.
7. **kconfig-frontends**: Provides a graphical configuration menu.
8. **libncurses-dev**: Text-based user interface library.
9. **dos2unix**: Converts text files from CRLF to LF line endings.

### Installation Steps

**Debian & Ubuntu**
```bash
sudo apt update
sudo apt install make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev dos2unix
```

**ArchLinux**
```bash
pacman -Sy make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev dos2unix
```

**Alpine**
```bash
sudo apk update
sudo apk add make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev dos2unix
```

## Compilation Guide 📖

### Clone the project

```bash
git clone https://github.com/ViudiraTech/Uinxed-Kernel.git
cd Uinxed-Kernel
```

### Start Compiling

```bash
make
```

USB support is enabled by default in `.config-default`. The individual options are exposed in the `USB support` section of `Kconfig`:

```text
CONFIG_USB
CONFIG_USB_XHCI
CONFIG_USB_HID
CONFIG_USB_STORAGE
```

## Running Tests 🏃‍♂️

### Protocol and subsystem tests

The host-side test suite covers HID report decoding, xHCI ring cycle semantics, USB Mass Storage BOT/SCSI wire encoding, partition parsing, and existing VFS/input subsystems:

```bash
make -C tests
```

### Virtual machine running

```bash
make run
```

To run the USB path in QEMU with an xHCI controller, USB keyboard, USB tablet, and `ahci_disk.img` attached as a USB mass-storage disk:

```bash
make run-usb
```

Expected device nodes include USB HID devices under `/dev/input/eventX` and the removable disk under `/dev/sdX`, with partition nodes when a valid MBR or GPT table is present.

### Physical machine operation

#### Boot in UEFI mode

Direct boot:

1. Convert the USB drive or hard disk to GPT partition table and create ESP partition.
2. Copy all folders under the project directory ./assets/Limine to the ESP partition.
3. Copy the compiled kernel (UxImage) to the ./EFI/Boot/ directory in the ESP partition.
4. Boot from a physical machine (must be in 64-bit UEFI mode with CSM disabled)

Boot with ventoy:

1. Copy the ISO image into your USB drive.
2. Enter firmware settings and turn off your security boot to make sure ventoy is able to boot.
3. Boot from your USB drive and select the ISO image.
4. Kernel boots successfully.

#### Boot in legacy mode

Direct boot:

1. Burn the ISO image to your drive.
2. Boot from a physical machine (at least a 64-bit machine)

Boot with ventoy:

1. Copy the ISO image to your drive.
2. Boot from your drive and enter in ventoy.
3. Select the image then press enter, boot in normal mode or memdisk mode.

## Project Structure 📁

```
Uinxed-Kernel/
├── .git/            # Version management.
├── .github/         # Github configuration file.
├── assets/          # Static resource files.
├── boot/            # Boot related.
├── docs/            # Related documents.
├── drivers/         # Device driver.
├── fs/              # File system.
├── include/         # Header file.
├── init/            # Code entry.
├── ipc/             # Inter-process communication.
├── net/             # Networking stack (Ethernet, ARP, IPv4, ICMP, UDP, TCP, ABI).
├── kernel/          # Kernel part.
├── libs/            # Library file.
├── mem/             # Memory management.
├── tools/           # Kernel tools.
├── .clang-format    # Formatting configuration files.
├── .clang-tidy      # Static analysis configuration file.
├── .clangd_template # Clangd configuration template.
├── .config-default  # Default configuration options.
├── .gitignore       # Ignore rules.
├── Kconfig          # Project configuration file.
├── LICENSE          # Open source license.
├── Makefile         # Build script.
├── README.md        # Project introduction.
└── SECURITY.md      # Security policy file.
```

```
Uinxed-Kernel/
├── UxImage         # Kernel image.
└── Uinxed-x64.iso  # Bootable CD image.
```

## FAQ 🔍

### Errors about "'XXX.h' file not found" in editors.

A: If you are using clangd as a code analyzer, you can generate a .clangd file via Makefile. Just like this:

```bash
make gen.clangd
```

However, if you are using a different LSP server, you can refer to the Makefile to change your configuration file.

### How to format the code?

A: Make sure you have clang-format installed, then execute make format. Just like this:

```bash
make format
```

### How to statically check code?

A: Make sure clang-tidy is installed, and then execute make check as follows:

```bash
make check
```

## Contribution Guide 🤝

We welcome contributions to this project! To ensure a smooth workflow, please follow the steps below to contribute code to the develop branch:

### 1.Fork the Repository

- Fork the repository from the project's main GitHub page.

### 2.Clone the Repository

- Clone your forked repository to your local machine:

```bash
git clone https://github.com/your-username/your-repository.git
cd your-repository
```

### 3.Develop Your Code

- Start coding! Make the necessary changes or add new features in your develop branch.
- Be sure to test your code and fix any bugs before proceeding.

### 4.Stage Your Changes

- After making your changes, stage them using:

```bash
git add .
```

### 5.Commit Your Changes

- Commit your changes with a clear and descriptive message:

```bash
git commit -m "Describe the changes or features you've implemented."
```

### 6.Push Your Changes to the Remote Branch

- Push your local branch to your remote fork:

```bash
git push
```

### 7.Create a Pull Request

- Visit your GitHub repository and create a Pull Request (PR).
- Fill in the PR description clearly, outlining the changes you've made.
- Submit the PR for review.

### 8.Code Review and Merging

- Wait for the project maintainers to review your code. They may provide feedback or request changes.
- Once the code is reviewed and approved, it will be merged into the develop branch.

### 9. **Update Your Local Branch**

- After your PR is merged, pull the latest changes from remote branch to keep your local repository up to date:

```bash
git pull --rebase
```

### Important Notes:

- All development work should be done on the develop branch.
- The master branch is reserved for stable, production-ready versions. Do not submit changes directly to master.

## Submit an Issue ❓

Communication is the key to problem solving. When you encounter a bug, you can report it to us by submitting an issue.
We welcome everyone to submit an issue. But before you do it, you should know our rule of submitting.

1. **Describe the problem in as much detail as possible.**
It will help us understand clearly what happened. For this, you may submit with your log or code snippet.
2. **Just be polite.**
Bring your problem politely. Asking questions to us in an extremely impolite manner will not solve any substantive problems, but will instead cause both the questioner and the answerer to engage in a fight. Asking questions politely and rationally will help solve the problem smoothly.
3. **No need to worry about being formal.**
You can be more casual, no need to be too formal. It is because of your questions that we can make our project better, so in a sense we are partners. On the other hand, because the language is too formal, it may be difficult to read. In order to maximize efficiency, we do not require formal language.
4. **Native language allowed.**
You can submit an issue in your own language. However, please be aware of factors such as spelling that may affect the translation results, and sometimes the translator may not work well because of this.

## Resident Contributor 👨‍💻

| Number | Nick name | Github | Job |
|---|---|---|---|
| 1 | Rainy101112 | [Rainy101112](https://github.com/Rainy101112) | Userspace & Services, Code optimization & Testing, Bug fixing, Management |
| 2 | MicroFish | [FengHeting](https://github.com/FengHeting) | Driver & Low-level, Planning, Code optimization & Testing, Management |
| 3 | JiTianYu391 | [JiTianYu391](https://github.com/JiTianYu391) | Features, Code optimization & Testing, Bug fixing |
| 4 | suhuajun | [suhuajun-github](https://github.com/suhuajun-github) | Code optimization & Testing, Bug fixing |
| 5 | XSlime | [W9pi3cZ1](https://github.com/W9pi3cZ1) | Features, Code optimization & Testing, Bug fixing |
| 6 | TMX | [TMXQWQ](https://github.com/TMXQWQ) | Features, Code optimization & Testing, Bug fixing |

A resident contributor is someone who directly contributes to and manages the project in a resident capacity. Removing a resident developer from the resident developer list does not mean that the project manager will not recognize their contributions, but rather that they are no longer involved in the project in a resident capacity. However, their contribution record will remain.

## Open Source Projects Referenced or Used 🎈

- Hurlex-Kernel:[http://wiki.0xffffff.org/](http://wiki.0xffffff.org/)
- CoolPotOS:[https://github.com/plos-clan/CoolPotOS](https://github.com/plos-clan/CoolPotOS)

## License 📜

This project adopts the Apache 2.0 open source license. Please refer to the LICENSE file for details.

## Contact Details 📩

- Email：rainy101112@163.com | 2609948707@qq.com | 3585302907@qq.com
- Join our discord server: [Click here](https://discord.gg/nTkg7HCpy7)
- Tencent QQ chat group: [983673299](https://qm.qq.com/q/8goacFf1iU)
