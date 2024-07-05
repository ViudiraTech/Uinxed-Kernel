; =====================================================
;
;		timer_s.s
;		内核定时器程序汇编程序
;
;		2024/7/6 By MicroFish
;		基于 GPL-3.0 开源协议
;		Copyright © 2020 ViudiraTech，保留所有权利。
;
; =====================================================

[GLOBAL io_load_eflags]

io_load_eflags:
	pushfd ; PUSH EFLAGS
	pop eax
	ret

[GLOBAL io_store_eflags]

io_store_eflags:
	mov eax, [esp+4]
	push eax
	popfd ; POP EFLAGS
	ret
