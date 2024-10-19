; =====================================================
;
;		gdt_s.s
;		设置全局描述符汇编程序
;
;		2024/6/27 By Rainy101112
;		基于 GPL-3.0 开源协议
;		Copyright © 2020 ViudiraTech，保留最终解释权。
;
; =====================================================

[GLOBAL gdt_flush]

gdt_flush:
	mov eax, [esp+4]  ; 参数存入 eax 寄存器
	lgdt [eax]        ; 加载到 GDTR [修改原先GRUB设置]

	mov ax, 0x10      ; 加载我们的数据段描述符
	mov ds, ax        ; 更新所有可以更新的段寄存器
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	jmp 0x08:.flush   ; 远跳转，目的是清空流水线并串行化处理器，0x08是我们的代码段描述符
.flush:
	ret
