<div align="center">
  <img src="https://github.com/user-attachments/assets/cb3f4ec8-4504-4fe9-b402-8d1588a986a8" height="200" width="200"/>
  <h1 align="center">Uinxed-Kernel</h1>
  <h3 align="center">一个从零开始用 C 语言编写的类 UNIX x86-64 内核。</h3>
</div>

<div align="center">
  <img src="https://img.shields.io/badge/License-Apache2.0-blue"/>
  <img src="https://img.shields.io/badge/Language-C-orange"/>
  <img src="https://img.shields.io/badge/Hardware-x64-green"/>
  <img src="https://img.shields.io/badge/Firmware-UEFI/Legacy-yellow"/>
  <a href="https://deepwiki.com/ViudiraTech/Uinxed-Kernel"><img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki"></a>
</div>

<div align="center">
  [English](../README.md) | **中文（当前）** | [日本語](README_ja.md) | [한국어](README_ko.md) | [Русский](README_ru.md) | [Français](README_fr.md)
</div>

---

## 概述

Uinxed 是一个面向 x86-64 的宏内核、类 UNIX 操作系统内核，完全用 C 语言从零编写。它通过 [Limine](https://limine-bootloader.org/) 引导加载程序在 UEFI 和 Legacy 模式下启动，通过对称多处理（SMP）启动所有核心，并实现与 Linux 兼容的系统调用 ABI（Linux 6.12 x86-64 编号，系统调用 0-462）。

本项目旨在构建一个实用的、自包含的内核，遵循现代设计原则：EEVDF 调度器、带交换支持的统一页缓存、支持多种文件系统的完整虚拟文件系统（VFS）、Linux 风格的网络和套接字层，以及不断增长的设备驱动集。未实现的系统调用返回 `-ENOSYS`，使 ABI 接口在发展过程中保持可预测性。

> **当前状态：** 开发镜像可在 x86-64 上启动 Alpine Linux 3.23，并能启动带可用终端的 Xfce（X11）桌面。PS/2 键盘/鼠标路径、evdev 消费者、poll/epoll 唤醒和 EEVDF 调度器正在积极验证中。这仍是一个实验性内核；GPU、VirtIO、音频以及部分 Linux 兼容 ABI 可能仍不完整。可在包括 Dell PowerEdge R410 在内的物理服务器上运行。

## 核心功能

### 调度与进程管理

- EEVDF（最早合格虚拟截止时间优先）调度器，带每 CPU 运行队列和红黑树时间线（`vruntime`、`deadline`、`vlag`、`weight`）
- 支持 SMP 的任务放置、CPU 迁移、负载均衡和基于处理器间中断（IPI）的抢占
- 避免丢失唤醒的两阶段等待队列，以及由调度器定时器队列支持的定时等待
- 用于健壮互斥锁和 futex 语义的优先级继承（PI）
- 内核线程和用户进程，带每进程虚拟内存区域（VMA）、文件描述符表和凭证
- Linux 兼容的 `ptrace` 和带 pids 控制器的 cgroups

### 内存管理

- 物理页帧分配器（二进制伙伴算法）和标准 4 级页表，支持 4 KiB、2 MiB 和 1 GiB 页
- 高半区直接映射（HHDM）和基于伙伴算法的内核堆/slab 分配器
- 统一页缓存，带页锁定、LRU 回收、脏页写回、预读和截断
- 匿名内存交换子系统：多交换区、槽位分配和换入/换出缺页处理

### VFS 与文件系统

- 类 UNIX 虚拟文件系统，带挂载点、类 inode 节点和基于回调的驱动接口
- tmpfs 作为默认根文件系统；procfs、sysfs、devtmpfs、cpio 和 cgroupfs 提供虚拟视图
- FAT12/16/32/exFAT（通过 FatFS，支持 64 位 LBA 和可变扇区大小）、ext2/ext3/ext4、NTFS（带写支持）和 ISO 9660（带 Rock Ridge）

### 网络

- 自研协议栈：以太网、ARP、IPv4/IPv6、ICMP/ICMPv6、NDP、UDP 和 TCP
- 以太网网卡驱动：Intel e1000/e1000e（82540EM、82545EM、82546EB、82541PI、82574L）和 Realtek RTL8139/RTL8169，位于通用网络设备抽象层之后
- Linux `AF_INET` / `AF_INET6` 套接字 ABI（`SOCK_DGRAM` / `SOCK_STREAM`）、DHCP 客户端和 `/proc/net` / `/sys/class/net` 视图

### ABI 与进程间通信

- Linux x86-64 系统调用 ABI（Linux 6.12 编号，0-462）
- `AF_UNIX`、`AF_NETLINK`、`AF_INET`、`AF_INET6` 套接字
- 管道、`epoll`、`eventfd`、`timerfd`、`signalfd`、`memfd`、`pidfd`、POSIX 消息队列和 System V IPC
- 带优先级继承的 futex 以及 futex2 系统调用（`futex_wait` / `futex_wake` / `futex_requeue` / `futex_waitv`）；由页缓存支持的 `mmap` / `munmap` / `mremap`
- 用于文件系统事件通知的 inotify
- POSIX termios 和 Linux TTY ioctl，包括 Unix98 PTY 和多个虚拟终端（VT，默认 8 个）
- 通过 `init_module` / `finit_module` / `delete_module` 实现可加载内核模块

### 安全与追踪

- seccomp 过滤器，带 `no_new_privs`、用户通知和 seccomp 事件支持
- Linux 兼容的 `ptrace` 检查、进程生命周期事件和系统调用停止处理

### 驱动

- **输入设备：** PS/2 键盘和鼠标、Linux 兼容的 `evdev`、USB HID（键盘、鼠标、消费者控制）
- **存储设备：** IDE/ATA、AHCI（SATA）、NVMe 和 USB 大容量存储（批量传输 / SCSI）
- **音频设备：** Sound Blaster 16、Intel HD Audio 和 ALSA 兼容的 PCM/控制 ABI
- **显示设备：** DRM/KMS 核心，带通用 GPU 驱动注册表（内置 VirtIO-GPU）、GOP 帧缓冲控制台点位图字体、软件帧缓冲回退
- **总线：** PCI/PCIe（ECAM + 传统）、USB 主机控制器（UHCI/OHCI/EHCI/xHCI）和 I2C
- **平台设备：** ACPI、HPET、RTC、串口、IEEE 1284 并口和 TPM（TIS/CRB，TPM 1.2/2.0）

## 系统架构

内核通过 Limine 启动，Limine 将控制权交给 `init/main.c` 中的 `kernel_entry()`。早期初始化启动 SIMD 状态、串口输出、物理分配器、页表和堆；然后平台层探测 ACPI、TPM、TSC 和 SMP，之后注册驱动和文件系统。最后初始化进程管理、IPC 和调度器，之后引导加载程序提供的 `init` 用户空间被加载为 PID 1 并开始调度。

```
Limine (UEFI/Legacy)
               |
               v
+------------------------------+     +-------------------------------+
| 早期初始化                   |---->| 平台与驱动                    |
| FPU/SSE -> 串口 -> 分配器     |     | ACPI -> SMP -> PCI -> 存储    |
| 页表 -> 堆 -> 模块            |     | 网络 -> 音频 -> 输入 -> USB   |
+------------------------------+     +-------------------------------+
               |                                    |
               v                                    v
+------------------------------+     +-------------------------------+
| VFS 与文件系统                |     | 内核服务                      |
| tmpfs/procfs/sysfs -> FAT    |     | 调度器 -> 进程 -> IPC          |
| ext/NTFS/ISO9660             |     | 系统调用 -> 信号 -> cgroups   |
+------------------------------+     +-------------------------------+
               |                                    |
               +-----------------+------------------+
                                 |
                                 v
                   sched_start() -> init (PID 1)
```

## 快速开始

### 前置要求

- **make**、**gcc**（推荐 13.3+）、**qemu**、**xorriso**
- **clang-format**、**clang-tidy**（格式化和静态分析）
- **kconfig-frontends** + **libncurses-dev**（用于 `menuconfig`）

Debian/Ubuntu：

```bash
sudo apt update
sudo apt install make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev dos2unix
```

ArchLinux：

```bash
pacman -Sy make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev dos2unix
```

### 编译

```bash
git clone https://github.com/ViudiraTech/Uinxed-Kernel.git
cd Uinxed-Kernel
make
```

这会生成 `UxImage`（内核镜像）和 `Uinxed-x64.iso`（可引导光盘镜像）。

### 在 QEMU 中运行

```bash
make run
```

`make run` 使用 `-machine q35`、OVMF 固件和 `-serial stdio` 启动 ISO，串口输出会显示在终端中。

### 在物理硬件上运行

**UEFI 模式**

1. 将目标磁盘转换为 GPT 分区表并创建 ESP。
2. 将 `./assets/Limine` 的内容复制到 ESP。
3. 将 `UxImage` 复制到 ESP 的 `EFI/Boot/`。
4. 以 64 位 UEFI 模式启动（禁用 Secure Boot）。

**Legacy 模式**

1. 将 `Uinxed-x64.iso` 刻录到磁盘。
2. 在 64 位机器上从它启动。

两种模式也都支持通过 [Ventoy](https://www.ventoy.net/) 启动：将 ISO 复制到磁盘上，从启动菜单中选择即可。

## 项目结构

```
Uinxed-Kernel/
|-- assets/           # 构建和引导资源
|-- boot/             # 引导协议结构和接口
|-- docs/             # 项目文档和技术笔记
|-- drivers/          # 硬件驱动和设备支持
|-- fs/               # 文件系统实现和 VFS 组件
|-- include/          # 内核头文件和公共接口
|-- init/             # 内核入口和初始化例程
|-- ipc/              # 进程间通信机制
|-- kernel/           # 核心内核子系统和运行时服务
|-- libs/             # 内部内核库和实用函数
|-- mem/              # 内存管理子系统
|-- net/              # 网络栈和协议实现
|-- scripts/          # 构建、配置和维护脚本
|-- security/         # 安全和策略相关组件
|-- tools/            # 开发、调试和辅助工具
|-- .clang-format     # 代码格式化配置
|-- .clang-tidy       # 静态分析配置
|-- .config-default   # 默认内核配置
|-- .gitignore        # Git 忽略规则
|-- CONTRIBUTING.md   # 贡献指南
|-- CREDITS           # 贡献者名单
|-- Kconfig           # 内核配置系统
|-- LICENSE           # 项目许可证（Apache License 2.0）
|-- Makefile          # 构建系统
|-- NOTICE            # 第三方许可证和归属声明
|-- README.md         # 项目概述和介绍
`-- SECURITY.md       # 安全策略和漏洞报告
```

## 常见问题

**有哪些开发命令可用？**

运行 `make help` 列出所有支持的目标（format、check、menuconfig、gen.clangd 等）。

**编辑器中出现 "XXX.h file not found" 错误？**

如果你使用 clangd 作为 LSP 服务器，用以下命令生成项目配置：

```bash
make gen.clangd
```

对于其他 LSP 服务器，请根据 Makefile 调整你的配置。

**如何查看内核日志？**

日志输出发送到引导控制台。默认输出到屏幕（`tty0`，通过 `assets/Limine/Limine/limine.conf` 中的 `kernel_cmdline: console=tty0` 设置）。要通过串口捕获日志：

1. 将 `limine.conf` 中的 `kernel_cmdline` 改为 `console=ttyS0`（或 `ttyS1`-`ttyS3`）。
2. 重新编译并运行。`make run` 已将 QEMU 串口输出连接到终端（`-serial stdio`）。

`console=` 参数接受 `tty0`（VGA 屏幕）和 `ttyS0`-`ttyS3`（串口）。注意，屏幕控制台会缓冲输出，如果 VGA 队列溢出或内核在引导过程中挂起，可能会丢失数据；串口控制台是调试挂起的可靠方式。`plogk` 调试消息仅在启用 `CONFIG_KERNEL_LOG` 时才会被编译进去。

**所有 Linux 系统调用都实现了吗？**

没有。系统调用表遵循 Linux 6.12 x86-64 编号（系统调用 0-462），但只实现了子集。未实现的系统调用返回 `-ENOSYS` 而不是崩溃，实现集随项目发展而增长。

**为什么某个驱动不工作（例如 VirtIO-GPU、SB16）？**

某些子系统在 Kconfig 中默认禁用。例如，`VIRTIO` 和 `VIRTIO_GPU` 默认为 `n`，`SOUND_SB16` 默认为 `n`。使用 `make menuconfig` 启用它们（需要 kconfig-frontends 和 libncurses-dev），然后确保对应的设备存在于你的虚拟机或硬件中。GPU 驱动通过 `drivers/gpu/gpu_drivers.c` 中的注册表发现；添加新的 GPU 驱动意味着在那里加一个条目。构建时如果存在 `.config` 则读取它，否则读取 `.config-default`；生成的 `.config` 优先级更高。

**能在真实硬件上运行吗？**

可以，但请将其视为实验性内核。按照上述物理硬件步骤操作，并优先使用一次性机器或测试磁盘——文件系统驱动（尤其是 NTFS 写入器）尚不足以安全处理重要数据。

## 许可证与免责声明

### 许可证

本项目采用 [Apache License 2.0](../LICENSE) 许可证。
本发行版包含第三方软件，各自遵循其许可证。完整归属和许可证文本收集在 [NOTICE](../NOTICE) 中。
部分代码引用并重新实现了 Linux 内核接口以实现互操作性。这些是公共接口和规范的独立实现；不包含 Linux 内核源代码。

### 免责声明

Uinxed 是一个正在积极开发的实验性内核。按"原样"提供，不提供任何形式的明示或暗示保证，包括但不限于对适销性和特定用途适用性的保证。

- 内核及其文件系统驱动（包括 NTFS 写入器）**不**适用于生产数据。仅使用一次性磁盘或虚拟机。
- 硬件支持不完整；在未经测试的真实硬件上运行可能导致挂起、崩溃或数据丢失。
- 本项目与 Linux、Limine 或任何引用的开源项目无隶属关系。所有商标归其各自所有者所有。

使用本软件即表示你承认风险自负。

## 联系方式

- 邮箱：rainy101112@163.com | 2609948707@qq.com | 3585302907@qq.com
- Discord：[加入服务器](https://discord.gg/nTkg7HCpy7)
- QQ 群：[983673299](https://qm.qq.com/q/8goacFf1iU)
