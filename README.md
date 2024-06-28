 # 欢迎来到 Uinxed 内核项目

## 简介

Uinxed内核是一款与Linux相似的x86内核，目前仅仅是简单的几个功能。

## 编译要求

1. 必须是Linux系统，例如Debian、Ubuntu等。
2. 必须安装好gcc、make、nasm和xorriso工具，如果需要测试，请安装qemu虚拟机。

## 编译指南

1. 将源码PULL到本地。
2. 在已PULL到本地的项目源码根目录内执行make命名，即可开始编译。
3. 编译后会生成两个文件：UxImage和system.iso，这两个文件分别为内核文件和带grub引导的镜像文件。
4. 输入“make clean”清理编译物，输入“make run”即可通过qemu测试启动iso镜像，输入“make runk”可以通过qemu测试内核文件启动。

## 版权声明

本Uinxed内核项目所有商为ViudiraTech。
内核源码为GPL-3.0开源协议
Copyright © 2020 ViudiraTech，保留所有权力。