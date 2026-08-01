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
    mov $2, %rax            # SYS_open
    lea console_path(%rip), %rdi
    mov $2, %rsi            # O_RDWR
    xor %rdx, %rdx          # mode = 0
    syscall
    test %rax, %rax
    jns 1f

    mov $2, %rax            # SYS_open
    lea tty_path(%rip), %rdi
    mov $2, %rsi            # O_RDWR
    xor %rdx, %rdx          # mode = 0
    syscall
    test %rax, %rax
    js idle

1:
    mov %rax, %rdi          # fd returned by open
    mov $1, %rax            # SYS_write
    lea msg(%rip), %rsi     # buf
    mov $msg_len, %rdx      # count
    syscall

idle:
    mov $34, %rax           # SYS_pause; PID 1 must remain alive
    syscall
    jmp idle
.section .rodata
console_path:
    .asciz "/dev/console"
tty_path:
    .asciz "/dev/tty"
msg:
    .ascii "Hello world!\n"
msg_len = . - msg
