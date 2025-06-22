<div align="center"> 
  <img height="200px" src="https://github.com/user-attachments/assets/1fd6a3d5-5add-4244-83a2-8e0c651602d0"/>
  <h1 align="center">Uinxed-Kernel</h1>
  <h3 align="center">Welcome to the Uinxed-Kernel project</h3>
</div>

<div align="center">
  <img src="https://img.shields.io/badge/License-GPLv3-blue"/>
  <img src="https://img.shields.io/badge/Language-3-orange"/>
  <img src="https://img.shields.io/badge/hardware-x64-green"/>
  <img src="https://img.shields.io/badge/firmware-UEFI/Legacy-yellow"/>
</div>

## Overview 💡

Uinxed is a Unix-like operating system kernel developed from scratch, focusing on modern computer architecture and advanced system design concepts. The project aims to build an efficient, stable, and scalable operating system kernel while maintaining code clarity and maintainability.

## Core Features 🌟

- **x86_64 architecture support**: optimized for modern 64-bit x86 processors
- **UEFI boot**: uses UEFI as the boot mode to support modern hardware platforms
- **Legacy boot**: Compatible with traditional Legacy boot
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

1. **make**: Used to build projects.
2. **gcc**: GCC Version 13.3.0+ is recommended.
3. **qemu**: Used for simulation testing.
4. **xorriso**: Used to build ISO image files.
5. **clang-format**: Used to format the code.
6. **clang-tidy**: Used for static analysis of code.
7. **kconfig-frontends**: Provides a graphical configuration menu.
8. **libncurses-dev**: Text-based user interface library.

### Installation Steps

**Debian & Ubuntu & Kali**
```bash
sudo apt update
sudo apt install make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev
```

**ArchLinux**
```bash
pacman -Sy make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev
```

**Alpine**
```bash
sudo apk update
sudo apk add make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev
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
├── .git/            # Version management.
├── .github/         # Github configuration file.
├── assets/          # Boot and scripts.
├── devices/         # Device driver.
├── docs/            # Related Documents.
├── include/         # Header file.
├── init/            # Code entry.
├── kernel/          # Kernel part.
├── libs/            # Library file.
├── .clangd_template # Clangd configuration template.
├── .clang-format    # Formatting Configuration Files.
├── .config-default  # Default configuration options.
├── .gitignore       # Ignore rules.
├── Kconfig          # Project Configuration File.
├── LICENSE          # Open source agreement.
├── Makefile         # Build script.
└── README.md        # Project introduction.
```

```
Uinxed-x86_64/
├── UxImage         # Kernel file.
└── Uinxed-x64.iso  # Bootable image.
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

## Contribution Guidelines 🤝

Contributions are welcome! Please follow the steps below:

1. Fork the project repository.
2. Contribute your code or modifications.
3. Ensure there are no obvious issues.
4. Format the code.
5. Submit a Pull Request.
6. Wait for code review and merging.

## Submit an issue ❓

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

__*TIPS!*__
1. Any code-related questions __*can only be asked by submitting issues*__. Other methods, such as asking questions directly through software such as *Discord*, will __*not be accepted*__.
2. If you are unkind, such as being rude as described above, it will be considered a violation of our submission rules and the issue will be __*closed immediately*__.

__*What kind of issues will not be adopted?*__

The following samples are unaccept. 

Rude.
```
YOU GUYS DID A REALLY S**T.
GO F**K YOURSELVES.
```

Unclear.
```
Doesn't work on VMware.
*No anything else*
```

Common sense questions.
```
How to install the compiler?
```

Strange.
```
:D
```

Not relevant.
```
Fish can't live without water.
```

## Core Developers 👨‍💻

1. MicroFish:Main Development/Planning/Management
2. Rainy101112:Deputy Development/Planning/Management
3. suhuajun:Testing/bug fixing
4. XIAOYI12:Co-developer

## Open source code or projects used by the project 🎈

- Hurlex-Kernel:[http://wiki.0xffffff.org/](http://wiki.0xffffff.org/)
- CoolPotOS:[https://github.com/plos-clan/CoolPotOS](https://github.com/plos-clan/CoolPotOS)

## license 📜

This project adopts the GPL-3.0 open source agreement. Please refer to the LICENSE file for details.

## Contact Details 📩

email：2609948707@qq.com | 3585302907@qq.com
