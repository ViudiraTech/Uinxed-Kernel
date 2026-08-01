/*
 *
 *      init.S
 *      Userspace init entry point
 *
 *      2026/7/21 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

.global _start
.section .text
_start:
    mov $59, %rax           # SYS_execve
    lea init_path(%rip), %rdi
    lea init_argv(%rip), %rsi
    lea init_envp(%rip), %rdx
    syscall

    mov $1, %rax            # SYS_write
    mov $2, %rdi            # stderr, installed by the kernel for PID 1
    lea exec_error(%rip), %rsi
    mov $exec_error_len, %rdx
    syscall

idle:
    mov $34, %rax           # SYS_pause; PID 1 must remain alive
    syscall
    jmp idle
.section .rodata
init_path:
    .asciz "/sbin/init"
path_env:
    .asciz "PATH=/sbin:/bin:/usr/sbin:/usr/bin"
home_env:
    .asciz "HOME=/root"
term_env:
    .asciz "TERM=linux"
shell_env:
    .asciz "SHELL=/bin/sh"
exec_error:
    .ascii "init: execve(/sbin/init) failed\n"
exec_error_len = . - exec_error

.section .data
.align 8
init_argv:
    .quad init_path
    .quad 0
init_envp:
    .quad path_env
    .quad home_env
    .quad term_env
    .quad shell_env
    .quad 0
