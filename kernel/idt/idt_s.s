; =====================================================
;
;		idt_s.s
;		设置中断描述符汇编程序
;
;		2024/6/27 By Rainy101112
;		基于 GPL-3.0 开源协议
;		Copyright © 2020 ViudiraTech，开放所有权利。
;
; =====================================================

[GLOBAL idt_flush]
idt_flush:
	mov eax, [esp+4]  ; 参数存入 eax 寄存器
	lidt [eax]        ; 加载到 IDTR
	ret
.end:

; 定义两个构造中断处理函数的宏（有的中断有错误代码，有的没有）
; 用于没有错误代码的中断
%macro ISR_NOERRCODE 1
[GLOBAL isr%1]
isr%1:
	cli					; 首先关闭中断
	push 0				; push 无效的中断错误代码（起到占位作用，便于所有isr函数统一清栈）
	push %1				; push 中断号
	jmp isr_common_stub
%endmacro

; 用于有错误代码的中断
%macro ISR_ERRCODE 1
[GLOBAL isr%1]
isr%1:
	cli					; 关闭中断
	push %1				; push 中断号
	jmp isr_common_stub
%endmacro

; 定义中断处理函数
ISR_NOERRCODE  0	; 0 #DE 除 0 异常
ISR_NOERRCODE  1	; 1 #DB 调试异常
ISR_NOERRCODE  2	; 2 NMI
ISR_NOERRCODE  3	; 3 BP 断点异常
ISR_NOERRCODE  4	; 4 #OF 溢出
ISR_NOERRCODE  5	; 5 #BR 对数组的引用超出边界
ISR_NOERRCODE  6	; 6 #UD 无效或未定义的操作码
ISR_NOERRCODE  7	; 7 #NM 设备不可用（无数学协处理器）
ISR_ERRCODE    8	; 8 #DF 双重故障（有错误代码）
ISR_NOERRCODE  9	; 9 协处理器跨段操作
ISR_ERRCODE   10	; 10 #TS 无效TSS（有错误代码）
ISR_ERRCODE   11	; 11 #NP 段不存在（有错误代码）
ISR_ERRCODE   12	; 12 #SS 栈错误（有错误代码）
ISR_ERRCODE   13	; 13 #GP 常规保护（有错误代码）
ISR_ERRCODE   14	; 14 #PF 页故障（有错误代码）
ISR_NOERRCODE 15	; 15 CPU 保留
ISR_NOERRCODE 16	; 16 #MF 浮点处理单元错误
ISR_ERRCODE   17	; 17 #AC 对齐检查
ISR_NOERRCODE 18	; 18 #MC 机器检查
ISR_NOERRCODE 19	; 19 #XM SIMD（单指令多数据）浮点异常

; 20~31 Intel 保留
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

; 32～255 用户自定义
ISR_NOERRCODE 128

[GLOBAL isr_common_stub]
[EXTERN isr_handler]

; 中断服务程序
isr_common_stub:
	pusha				; Pushes edi, esi, ebp, esp, ebx, edx, ecx, eax
	mov ax, ds
	push eax			; 保存数据段描述符

	mov ax, 0x10		; 加载内核数据段描述符表
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax

	push esp			; 此时的 esp 寄存器的值等价于 pt_regs 结构体的指针
	call isr_handler	; 在 C 语言代码里
	add esp, 4			; 清除压入的参数

	pop ebx				; 恢复原来的数据段描述符
	mov ds, bx
	mov es, bx
	mov fs, bx
	mov gs, bx
	mov ss, bx

	popa				; Pops edi, esi, ebp, esp, ebx, edx, ecx, eax
	add esp, 8			; 清理栈里的 error code 和 ISR
	iret
.end:

; 构造中断请求的宏
%macro IRQ 2
[GLOBAL irq%1]
irq%1:
	cli
	push 0
	push %2
	jmp irq_common_stub
%endmacro

IRQ   0,    32 	; 电脑系统计时器
IRQ   1,    33 	; 键盘
IRQ   2,    34 	; 与 IRQ9 相接，MPU-401 MD 使用
IRQ   3,    35 	; 串口设备
IRQ   4,    36 	; 串口设备
IRQ   5,    37 	; 建议声卡使用
IRQ   6,    38 	; 软驱传输控制使用
IRQ   7,    39 	; 打印机传输控制使用
IRQ   8,    40 	; 即时时钟
IRQ   9,    41 	; 与 IRQ2 相接，可设定给其他硬件
IRQ  10,    42 	; 建议网卡使用
IRQ  11,    43 	; 建议 AGP 显卡使用
IRQ  12,    44 	; 接 PS/2 鼠标，也可设定给其他硬件
IRQ  13,    45 	; 协处理器使用
IRQ  14,    46 	; IDE0 传输控制使用
IRQ  15,    47 	; IDE1 传输控制使用

[GLOBAL irq_common_stub]
[EXTERN irq_handler]
irq_common_stub:
	pusha				; pushes edi, esi, ebp, esp, ebx, edx, ecx, eax

	mov ax, ds
	push eax			; 保存数据段描述符

	mov ax, 0x10		; 加载内核数据段描述符
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax

	push esp
	call irq_handler
	add esp, 4

	pop ebx				; 恢复原来的数据段描述符
	mov ds, bx
	mov es, bx
	mov fs, bx
	mov gs, bx
	mov ss, bx

	popa				; Pops edi,esi,ebp...
	add esp, 8			; 清理压栈的 错误代码 和 ISR 编号
	iret				; 出栈 CS, EIP, EFLAGS, SS, ESP
.end:
