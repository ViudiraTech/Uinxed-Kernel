# 欢迎来到 Uinxed 内核项目

## 简介

此项目是一个基于GPL-3.0开源协议的操作系统内核开发项目，名为Uinxed-Kernel。它由MicroFish和Rainy101112等人于2024年发起并持续开发。Uinxed内核项目遵循开源精神，旨在创建一个自由、透明、任何人都可以使用和修改的操作系统内核。

## 项目特点

1. 自由使用：任何人都可以自由地使用Uinxed内核，无论是个人学习、研究还是商业用途。
2. 开源协作：项目鼓励社区参与，任何人都可以为项目贡献代码或提供反馈。
3. GPL-3.0协议：遵循GPL-3.0协议，确保了项目的开源性和自由传播，要求所有基于Uinxed的衍生作品也必须开源。
4. 持续更新：项目团队定期更新内核，修复bug，添加新功能，提高系统性能和稳定性。

## 编译要求

1. 必须是Linux系统，例如Debian、Ubuntu等。
2. 必须安装好gcc、make、nasm和xorriso工具，如果需要测试，请安装qemu虚拟机。

## 编译指南

1. 将源码PULL到本地。
2. 在已PULL到本地的项目源码根目录内执行make命名，即可开始编译。
3. 编译后会生成两个文件：UxImage和system.iso，这两个文件分别为内核文件和带grub引导的镜像文件。
4. 输入“make clean”清理编译物，输入“make run”即可通过qemu测试启动iso镜像，输入“make runk”可以通过qemu测试内核文件启动，”make run-db“和”make runk-db“可以调出对应启动模式的调试（控制台显示汇编代码）

# 贡献者排名

1. Rainy101112	（37份）
2. MicroFish	（26份）
3. XIAOYI12		（4份）
4. wrhmade		（2份）
5. copi143		（1份）

括弧内仅仅代表初次上传并且未删除的代码等文件数量，不包括修改、整理等。

# 项目所使用的开源代码

Hurlex-Kernel: http://wiki.0xffffff.org/  
CoolPotOS: https://github.com/xiaoyi1212/CoolPotOS  

## 版权声明

本Uinxed内核项目所有商为ViudiraTech。
内核源码为GPL-3.0开源协议
Copyright © 2020 ViudiraTech，保留所有权力。
