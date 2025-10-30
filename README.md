<div align="center"> 
  <img src="https://github.com/user-attachments/assets/cb3f4ec8-4504-4fe9-b402-8d1588a986a8" height="200" width="200"/>
  <h1 align="center">Uinxed-Kernel</h1>
  <h3 align="center">Welcome to the Uinxed-Kernel project</h3>
</div>

<div align="center">
  <img src="https://img.shields.io/badge/License-Apache2.0-blue"/>
  <img src="https://img.shields.io/badge/Language-C-orange"/>
  <img src="https://img.shields.io/badge/Hardware-x64-green"/>
  <img src="https://img.shields.io/badge/Firmware-UEFI/Legacy-yellow"/>
</div>

## Overview 💡

Uinxed-Kernel is a Unix-like operating system kernel developed from scratch, focusing on modern computer architecture and advanced system design concepts. The project aims to build an efficient, stable, and scalable operating system kernel while maintaining code clarity and maintainability.

## Core Features 🌟

- **x86_64 architecture support**: optimized for modern 64-bit x86 processors
- **UEFI boot**: uses UEFI as the boot mode to support modern hardware platforms
- **Legacy boot**: Compatible with traditional Legacy boot
- **KASLR**: Kernel address space layout randomization to enhance security.
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
  - Multi-core support based on symmetric multi-processing
- **Terminal meatures**:
  - TTF fonts or Bitmap fonts
  - High-speed terminal implementation

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

### Physical machine operation

#### Boot in UEFI mode

1. Convert the USB drive or hard disk to GPT partition table and create ESP partition.
2. Copy all folders under the project directory ./assets/Limine to the ESP partition.
3. Copy the compiled kernel (UxImage) to the ./EFI/Boot/ directory in the ESP partition.
4. Boot from a physical machine (must be in 64-bit UEFI mode with CSM disabled)

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
├── docs/            # Related Documents.
├── drivers/         # Device driver.
├── include/         # Header file.
├── init/            # Code entry.
├── kernel/          # Kernel part.
├── libs/            # Library file.
├── .clangd_template # Clangd configuration template.
├── .clang-format    # Formatting Configuration Files.
├── .config-default  # Default configuration options.
├── .gitignore       # Ignore rules.
├── Kconfig          # Project Configuration File.
├── LICENSE          # Open source license.
├── Makefile         # Build script.
└── README.md        # Project introduction.
```

```
Uinxed-Kernel/
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

### 3.Switch to the develop Branch

- Make sure you're on the develop branch to work on new features and improvements:

```bash
git checkout develop
```

### 4.Develop Your Code

- Start coding! Make the necessary changes or add new features in your develop branch.
- Be sure to test your code and fix any bugs before proceeding.

### 5.Stage Your Changes

- After making your changes, stage them using:

```bash
git add .
```

### 6.Commit Your Changes

- Commit your changes with a clear and descriptive message:

```bash
git commit -m "Describe the changes or features you've implemented."
```

### 7.Push Your Changes to the Remote develop Branch

- Push your local develop branch to your remote fork:

```bash
git push origin develop
```

### 8.Create a Pull Request

- Visit your GitHub repository and create a Pull Request (PR).
- Make sure the base branch is set to develop (not master).
- Fill in the PR description clearly, outlining the changes you've made.
- Submit the PR for review.

### 9.Code Review and Merging

- Wait for the project maintainers to review your code. They may provide feedback or request changes.
- Once the code is reviewed and approved, it will be merged into the develop branch.

### 10. **Update Your develop Branch**

- After your PR is merged, pull the latest changes from develop to keep your local repository up to date:

```bash
git pull origin develop
```

### Important Notes:

- All development work should be done on the develop branch.
- The master branch is reserved for stable, production-ready versions. Do not submit changes directly to master.

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

## Resident contributor 👨‍💻

| Number | Nick name | Github | Job |
|---|---|---|---|
| 1 | MicroFish | [FengHeting](https://github.com/FengHeting) | Development. Planning. Management. |
| 2 | suhuajun | [suhuajun-github](https://github.com/suhuajun-github) | Code optimization. Testing. Bug fixing. |
| 3 | XSlime | [W9pi3cZ1](https://github.com/W9pi3cZ1) | Features. Code optimization. Testing. Bug fixing. |
| 4 | TMX | [TMXQWQ](https://github.com/TMXQWQ) | Features. Code optimization. Testing. Bug fixing. |

A resident contributor is someone who directly contributes to and manages the project in a resident capacity. Removing a resident developer from the resident developer list does not mean that the project manager will not recognize their contributions, but rather that they are no longer involved in the project in a resident capacity. However, their contribution record will remain.

## Open source projects referenced or used 🎈

- Hurlex-Kernel:[http://wiki.0xffffff.org/](http://wiki.0xffffff.org/)
- CoolPotOS:[https://github.com/plos-clan/CoolPotOS](https://github.com/plos-clan/CoolPotOS)

## License 📜

This project adopts the Apache 2.0 open source license. Please refer to the LICENSE file for details.

## Contact details 📩

- Email：2609948707@qq.com | 3585302907@qq.com
- Join our discord server: [Click here](https://discord.gg/nTkg7HCpy7)
- Tencent QQ chat group: [983673299](https://qm.qq.com/q/8goacFf1iU)
