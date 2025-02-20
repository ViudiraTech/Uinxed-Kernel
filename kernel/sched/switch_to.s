; -------------------------------------------------
;
;
;		switch_to.s
; 		进程切换核心
;
;		2024/9/1 By Rainy101112
;		基于 GPL-3.0 开源协议
;		Copyright © 2020 ViudiraTech，开放所有权利。
;
;
; -------------------------------------------------

[GLOBAL switch_to]

; 具体的进程切换操作重点在于寄存器的保存与恢复
switch_to:
	mov eax, [esp+4]

	mov [eax+0],  esp
	mov [eax+4],  ebp
	mov [eax+8],  ebx
	mov [eax+12], esi
	mov [eax+16], edi
	mov [eax+20], ecx
	mov [eax+24], edx

	pushf
	pop ecx
	mov [eax+28], ecx

	mov eax, [esp+8]

	mov esp, [eax+0]
	mov ebp, [eax+4]
	mov ebx, [eax+8]
	mov esi, [eax+12]
	mov edi, [eax+16]
	mov ecx, [eax+20]
	mov edx, [eax+24]

	mov eax, [eax+28]
	push eax
	popf

	ret
