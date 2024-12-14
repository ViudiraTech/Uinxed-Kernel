; =====================================================
;
;		pg.s
;		物理内存页复制汇编程序
;
;		2024/6/30 By XIAOYI12
;		基于 GPL-3.0 开源协议
;		Copyright © 2020 ViudiraTech，保留最终解释权。
;
; =====================================================

[GLOBAL copy_page_physical]

copy_page_physical:
	push ebx				; 根据__cdecl，我们必须保存EBX的内容
	pushf					; 推送EFLAGS，这样我们就可以弹出它并重新启用中断

	cli						; 禁用中断

	mov ebx, [esp+12]		; 源地址
	mov ecx, [esp+16]		; 目的地地址

	mov edx, cr0			; 获取控制寄存器
	and edx, 0x7fffffff
	mov cr0, edx			; 禁用分页

	mov edx, 1024			; 1024*4比特 = 4096比特

.loop:
	mov eax, [ebx]			; 在源地址处获取字
	mov [ecx], eax			; 将其存储在 dest 地址
	add ebx, 4				; 源地址 += sizeof(word)
	add ecx, 4				; 目标地址 += sizeof(word)
	dec edx
	jnz .loop

	mov edx, cr0			; 再次获取控制寄存器
	or edx, 0x80000000
	mov cr0, edx			; 启用分页

	popf					; 将 EFLAGS 出栈
	pop ebx					; 取回 EBX 的原始值
	ret
