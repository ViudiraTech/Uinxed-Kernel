# 欢迎来到 Uinxed 内核项目

![](https://img.shields.io/badge/License-GPLv3-blue) ![](https://img.shields.io/badge/Language-3-orange) ![](https://img.shields.io/badge/hardware-x86-green) ![](https://img.shields.io/badge/firmware-BIOS-yellow)

## 简介

此项目是一个基于GPL-3.0开源协议的操作系统内核开发项目，名为Uinxed-Kernel。它由MicroFish和Rainy101112等人于2024年发起并持续开发。Uinxed内核项目遵循开源精神，旨在创建一个自由、透明、任何人都可以使用和修改的操作系统内核。

本项目因使用了带有GPL的源码，所以不得不使用GPL协议。但我们保证我们绝对开放，并且不会制造任何种族歧视和政治偏见，我们欢迎任何人提交源码和建议。

## 项目特点

1. **自由使用**：任何人都可以自由地使用Uinxed内核，无论是个人学习、研究还是商业用途。
2. **开源协作**：项目鼓励社区参与，任何人都可以为项目贡献代码或提供反馈。
3. **GPL-3.0协议**：遵循GPL-3.0协议，确保了项目的开源性和自由传播，要求所有基于Uinxed的衍生作品也必须开源。
4. **持续更新**：项目团队定期更新内核，修复bug，添加新功能，提高系统性能和稳定性。

## 编译要求

1. **操作系统**：需要在Unix环境（例如FreeBSD、Linux、macOS，Windows平台可安装WSL或CygWin）中进行编译。
2. **工具安装**：需要安装好gcc、nasm和grub-pc、xorriso工具。编译工具按自己喜好选择make或者xmake。如果需要测试，请安装qemu虚拟机。

## 工具安装方法：

**Debian & Ubuntu & Kali**
```bash
sudo apt update
sudo apt install gcc make xmake nasm grub-pc xorriso qemu-system
```

**ArchLinux**
```bash
pacman -Sy gcc make xmake nasm grub-pc xorriso qemu-system
```

**Alpine**
```bash
sudo apk update
sudo apk add gcc make xmake nasm grub-pc xorriso qemu-system
```

## 编译指南

1. **获取源码**：将源码clone到本地。
2. **编译**：在已clone到本地的项目源码根目录内执行make或者xmake命令即可开始编译。
3. **编译结果**：编译后会生成两个文件：UxImage和Uinxed.iso（通过xmake编译的会生成在./build/），这两个文件分别为内核文件和带引导的镜像文件。
4. **清理与测试**：
   - 输入“make clean” or “xmake clean”清理所有中间文件及UxImage和镜像。
   - （xmake编译后需要用xmake clean清理，make编译用make clean清理，为防止玄学事情发生，务必这样做！）
   - （编译后通过清理会残留一个./build/文件夹，目的是防止测试时误删用户文件）
   - 输入“make run” or “xmake run”即可通过qemu测试启动iso镜像。
   - 输入“make runk”可以通过qemu测试内核文件启动。
   - “make run-db”和“make runk-db”可以调出对应启动模式的调试（控制台显示汇编代码）。

## 贡献者名单

1. **MicroFish**
2. **Rainy101112**
3. **min0911Y**
4. **wenxuanjun**
5. **copi143**
6. **Hiernymus**
7. **wrhmade**
8. **Vinbe Wan**
9. **xiaoyi1212**
10. **ywx2012**

## 项目所使用的开源代码或项目

- Hurlex-Kernel: [http://wiki.0xffffff.org/](http://wiki.0xffffff.org/)
- CoolPotOS: [https://github.com/xiaoyi1212/CoolPotOS](https://github.com/plos-clan/CoolPotOS)
- libos-terminal: [https://github.com/plos-clan/libos-terminal](https://github.com/plos-clan/libos-terminal)
- pl_readline: [https://github.com/plos-clan/pl_readline](https://github.com/plos-clan/pl_readline)
- FatFS FileSystem: [http://elm-chan.org/fsw/ff/](http://elm-chan.org/fsw/ff/00index_e.html)

## 参考的网站

- OSDev: [https://wiki.osdev.org/](https://wiki.osdev.org)

## 外部链接

- UxSDK: [https://github.com/ViudiraTech/UxSDK](https://github.com/ViudiraTech/UxSDK)

## 版权声明

本Uinxed内核项目发起组织为ViudiraTech。
内核源码为GPL-3.0开源协议
Copyright © 2020 ViudiraTech，保留最终解释权。
