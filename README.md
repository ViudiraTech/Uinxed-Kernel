# 欢迎来到 Uinxed-Kernel 项目

![](https://img.shields.io/badge/License-GPLv3-blue) ![](https://img.shields.io/badge/Language-2-orange) ![](https://img.shields.io/badge/hardware-x64-green) ![](https://img.shields.io/badge/firmware-UEFI-yellow)

## 概述 💡

Uinxed是一个从零开始开发的类Unix操作系统内核，专注于现代计算机架构和先进的系统设计理念。该项目旨在构建一个高效、稳定、可扩展的操作系统内核，同时保持代码的清晰性和可维护性。

## 核心特性 🌟

- **x86_64架构支持**：针对现代64位x86处理器进行优化
- **UEFI启动**：采用UEFI作为启动方式，支持现代硬件平台
- **内存管理**：
  - 物理内存帧分配器
  - 虚拟内存页管理
  - 高半区内存映射(HHDM)
- **中断管理**：
  - 完整的中断描述符表(IDT)实现
  - 高级可编程中断控制器(APIC)支持
- **系统管理**：
  - ACPI支持
  - 高精度事件计时器(HPET)

## 开发环境准备 🛠️

### 必需工具

1. **Make**：用于构建项目
2. **GCC**：推荐使用GCC Version 13.3.0+
3. **QEMU**：用于模拟测试
4. **Xorriso**：用于构建ISO镜像文件

### 安装步骤

**Debian & Ubuntu & Kali**
```bash
sudo apt update
sudo apt install make gcc qemu-system xorriso
```

**ArchLinux**
```bash
pacman -Sy make gcc qemu-system xorriso
```

**Alpine**
```bash
sudo apk update
sudo apk add make gcc qemu-system xorriso
```

## 编译指南 📖

### 克隆项目

```bash
git clone https://github.com/FengHeting/Uinxed-x86_64.git
cd Uinxed-x86_64
```

### 开始编译

```bash
make
```

## 运行测试 🏃‍♂️

### 虚拟机运行

```bash
make run
```

### 实际硬件运行

1. 将U盘或硬盘转为GPT分区表，并创建一个ESP分区。
2. 将项目目录./assets/Limine中的efi文件夹拷贝进ESP分区。
3. 将编译出来的内核（UxImage）拷贝进您ESP分区下的./efi/boot/中。
4. 通过实体机启动（必须为64位UEFI模式，且CSM为关闭）

## 项目结构 📁

```
Uinxed-x86_64/
├── .git/          # 版本管理
├── assets/        # 引导和脚本
├── devices/       # 设备驱动
├── include/       # 头部文件
├── init/          # 代码入口
├── kernel/        # 内核部分
├── libs/          # 库文件
├── .gitignore     # 忽略规则
├── LICENSE        # 开源协议
├── Makefile       # 构建脚本
└── README.md      # 项目介绍
```

```
Uinxed-x86_64/
├── UxImage        # 内核文件
└── Uinxed-x64.iso # 可启动镜像
```

## 贡献指南 🤝

欢迎贡献代码！请遵循以下步骤：

1. fork项目仓库
2. 创建新分支进行开发
3. 代码文件署名
4. 提交Pull Request
5. 等待代码审查和合并

## 核心开发者 👨‍💻

1. MicroFish：主开发/策划/管理
2. Rainy101112：副开发/策划/管理
3. suhuajun：测试/修BUG
4. XIAOYI12：协开发者

## 项目所使用的开源代码或项目 🎈

- Hurlex-Kernel：[http://wiki.0xffffff.org/](http://wiki.0xffffff.org/)
- CoolPotOS：[https://github.com/xiaoyi1212/CoolPotOS](https://github.com/plos-clan/CoolPotOS)

## 许可证 📜

本项目采用GPL-3.0开源协议，详情请参阅LICENSE文件。

## 联系方式 📩

QQ：2609948707 | 3585302907
