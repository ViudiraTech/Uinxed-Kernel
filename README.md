<div align="center"> 
  <img height="200px" src="https://github.com/user-attachments/assets/ef586a12-6c55-4e8b-bdac-ab31411aa2de"/>     
  <h1 align="center">Uinxed-Kernel</h1>
  <h3 align="center">Welcome to the Uinxed-Kernel project</h3>
</div>

<div align="center">
  <img src="https://img.shields.io/badge/License-GPLv3-blue"/>
  <img src="https://img.shields.io/badge/Language-2-orange"/>
  <img src="https://img.shields.io/badge/hardware-x64-green"/>
  <img src="https://img.shields.io/badge/firmware-UEFI-yellow"/>
</div>

## Overview 💡

Uinxed is a Unix-like operating system kernel developed from scratch, focusing on modern computer architecture and advanced system design concepts. The project aims to build an efficient, stable, and scalable operating system kernel while maintaining code clarity and maintainability.

## Core Features 🌟

- **x86_64 architecture support**: optimized for modern 64-bit x86 processors
- **UEFI boot**: uses UEFI as the boot mode to support modern hardware platforms
- **Memory management**:
  - Physical memory frame allocator
  - Virtual memory page management
  - High half memory mapping (HHDM)
- **Interrupt management**:
  - Complete interrupt descriptor table (IDT) implementation
  - Advanced Programmable Interrupt Controller (APIC) support
- **System management**:
  - ACPI support
  - High Precision Event Timer (HPET)

## Development environment preparation 🛠️

### Required Tools

1. **Make**: used to build projects
2. **GCC**: GCC Version 13.3.0+ is recommended
3. **QEMU**: used for simulation testing
4. **Xorriso**: used to build ISO image files

### Installation Steps

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

## Compilation Guide 📖

### Clone the project

```bash
git clone https://github.com/FengHeting/Uinxed-x86_64.git
cd Uinxed-x86_64
```

### Start Compiling

```bash
make
```

## Running Tests 🏃‍♂️

### Virtual machine running

```bash
make run
```

### Actual hardware operation

1. Convert the USB drive or hard disk to a GPT partition table and create an ESP partition.
2. Copy the efi folder in the project directory ./assets/Limine to the ESP partition.
3. Copy the compiled kernel (UxImage) to ./efi/boot/ in your ESP partition.
4. Boot from a physical machine (must be in 64-bit UEFI mode and CSM is disabled)

## Project Structure 📁

```
Uinxed-x86_64/
├── .git/            # Version management
├── assets/          # Boot and scripts
├── devices/         # Device driver
├── include/         # Header file
├── init/            # Code entry
├── kernel/          # Kernel part
├── libs/            # Library file
├── .gitignore       # Ignore rules
├── LICENSE          # Open source agreement
├── Makefile         # Build script
├── .clangd_template # Clangd configuration template
└── README.md        # Project introduction
```

```
Uinxed-x86_64/
├── UxImage         # Kernel file
└── Uinxed-x64.iso  # Bootable image
```

## FAQ 🔍

### Errors about "'XXX.h' file not found" in editors

A: If you are using clangd as a code analyzer, you can generate a .clangd file via Makefile. Just like this:

```bash
make gen.clangd
```

However, if you are using a different LSP server, you can refer to the Makefile to change your configuration file.

## Contribution Guidelines 🤝

Contributions are welcome! Please follow the steps below:

1. Fork the project repository
2. Create a new branch for development
3. Sign the code file
4. Submit a Pull Request
5. Wait for code review and merge

## Core Developers 👨‍💻

1. MicroFish:Main Development/Planning/Management
2. Rainy101112:Deputy Development/Planning/Management
3. suhuajun:Testing/bug fixing
4. XIAOYI12:Co-developer

## Blacklist 🚷

1. [copi143](https://github.com/copi143) Inciting conflict

## Open source code or projects used by the project 🎈

- Hurlex-Kernel:[http://wiki.0xffffff.org/](http://wiki.0xffffff.org/)
- CoolPotOS:[https://github.com/xiaoyi1212/CoolPotOS](https://github.com/plos-clan/CoolPotOS)

## license 📜

This project adopts the GPL-3.0 open source agreement. Please refer to the LICENSE file for details.

## Contact Details 📩

email：2609948707@qq.com | 3585302907@qq.com
