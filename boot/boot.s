; =====================================================
;
;		boot.s
;		Uinxed内核启动程序
;
;		2024/6/23 By Rainy101112
;		基于 GPL-3.0 开源协议
;		Copyright © 2020 ViudiraTech，保留所有权利。
;
; =====================================================

; Multiboot 魔数，由规范决定的
MBOOT_HEADER_MAGIC EQU 0x1BADB002

; 0号位表示所有的引导模块将按页(4KB)边界对齐
MBOOT_PAGE_ALIGN EQU 1 << 0

; 1号位通过 Multiboot 信息结构的 mem_* 域包括可用内存的信息
; （告诉GRUB把内存空间的信息包含在 Multiboot 信息结构中）
MBOOT_MEM_INFO EQU 1 << 1    

; 定义我们使用的 Multiboot 的标记
MBOOT_HEADER_FLAGS EQU MBOOT_PAGE_ALIGN | MBOOT_MEM_INFO

; 域 checksum 是一个32位的无符号值，当与其他的 magic 域（也就是 magic 和 flags）
; 相加时，要求其结果必须是32位的无符号值0（即 magic + flags + checksum = 0）
MBOOT_CHECKSUM EQU -(MBOOT_HEADER_MAGIC+MBOOT_HEADER_FLAGS)

; 符合 Multiboot 规范的 OS 映象需要这样一个 magic Multiboot 头
; Multiboot 头的分布必须如下表所示：
; ----------------------------------------------------------
; 偏移量		类型		域名			备注
;
;	0		u32		magic		必需
;	4		u32		flags		必需 
;	8		u32		checksum	必需 
;
; 我们只使用到这些就够了，更多的详细说明请参阅 GNU 相关文档
; -----------------------------------------------------------

[BITS 32]							; 所有代码以 32-bit 的方式编译
SECTION .text						; 代码段从这里开始

; 在代码段的起始位置设置符合 Multiboot 规范的标记

DD MBOOT_HEADER_MAGIC				; GRUB 会通过这个魔数判断该映像是否支持
DD MBOOT_HEADER_FLAGS				; GRUB 的一些加载时选项，其详细注释在定义处
DD MBOOT_CHECKSUM					; 检测数值，其含义在定义处

[GLOBAL start]						; 向外部声明内核代码入口，此处提供该声明给链接器
[GLOBAL glb_mboot_ptr]				; 向外部声明 struct multiboot * 变量
[EXTERN kernel_init]				; 声明内核 C 代码的初始化函数

start:
	CLI								; 此时还没有设置好保护模式的中断处理，所以必须关闭中断
	MOV ESP, STACK_TOP				; 设置内核栈地址
	MOV EBP, 0						; 帧指针修改为0
	AND ESP, 0FFFFFFF0H				; 栈地址按照16字节对齐
	MOV [glb_mboot_ptr], EBX		; 将 ebx 中存储的指针存入全局变量
	PUSH EBX						; 将 ebx 中存储的指针压栈
	CALL kernel_init				; 调用内核初始化函数

stop:
	HLT								; 停机指令，可以降低 CPU 功耗
	JMP stop						; 到这里结束，关机什么的后面再说

SECTION .bss						; 未初始化的数据段从这里开始

stack:
	RESB 32768						; 这里作为内核栈

glb_mboot_ptr:						; 全局的 multiboot 结构体指针
	RESB 4

STACK_TOP EQU $-stack-1				; 内核栈顶，$ 符指代是当前地址
