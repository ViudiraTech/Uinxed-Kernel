; =====================================================
;
;		syscall_s.s
;		系统调用汇编中断处理
;
;		2024/12/15 By MicroFish
;		基于 GPL-3.0 开源协议
;		Copyright © 2020 ViudiraTech，保留最终解释权。
;
; =====================================================

[GLOBAL asm_syscall_handler]
[EXTERN disable_scheduler]
[EXTERN enable_scheduler]
[EXTERN syscall]

; 系统调用时要经处理的汇编程序
asm_syscall_handler:
	cli
	call disable_scheduler
	push ds
	push es
	push fs
	push gs
	pusha
	call syscall
	mov dword[esp+28], eax
	popa
	pop gs
	pop fs
	pop es
	pop ds
	call enable_scheduler
	sti
	iretd
