/*
 *
 *		syscall.c
 *		系统调用
 *
 *		2024/12/15 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "syscall.h"
#include "printk.h"
#include "common.h"
#include "sched.h"
#include "types.h"
#include "task.h"
#include "vfs.h"
#include "acpi.h"
#include "timer.h"
#include "beep.h"

typedef enum oflags {
	O_RDONLY,
	O_WRONLY,
	O_RDWR,
	O_CREAT = 4
} oflags_t;

/* POSIX规范-打开文件 */
static uint32_t syscall_posix_open(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	enable_intr();
	char *name = (char *)ebx;
	if (ecx & O_CREAT) {
		int status = vfs_mkfile(name);
		if (status == -1)goto error;
	}
	vfs_node_t r = vfs_open(name);
	if (r == 0) goto error;
	for (int i = 3; i < 255; i++) {
		if (get_current_proc()->file_table[i] == 0) {
			cfile_t file = kmalloc(sizeof(cfile_t));
			file->handle = r;
			file->pos = 0;
			file->flags = ecx;
			get_current_proc()->file_table[i] = file;
			return i;
		}
	}
	error:
	return -1;
}

/* POSIX规范-关闭文件 */
static uint32_t syscall_posix_close(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	for (int i = 3; i < 255; i++) {
		if (i == ebx) {
			cfile_t file = get_current_proc()->file_table[i];
			if (file == 0)return -1;
			vfs_close(file->handle);
			kfree(file);
			return (uint32_t)(get_current_proc()->file_table[i] = 0);
		}
	}
	return -1;
}

/* POSIX规范-读取文件 */
static uint32_t syscall_posix_read(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	enable_intr();
	if (ebx < 0 || ebx == 1 || ebx == 2)return -1;
	cfile_t file = get_current_proc()->file_table[ebx];
	if (file == 0)return -1;
	if (file->flags == O_WRONLY)return -1;
	char* buffer = kmalloc(file->handle->size);
	if (vfs_read(file->handle, buffer, 0, file->handle->size) == -1)return -1;
	int ret = 0;
	disable_intr();
	char *filebuf = (char *)buffer;
	char *retbuf = (char *)ecx;

	if (edx > file->handle->size) {
		memcpy((uint8_t *)retbuf, (const uint8_t *)filebuf, file->handle->size);
		ret = file->pos += file->handle->size;
	} else {
		memcpy((uint8_t *)retbuf, (const uint8_t *)filebuf, edx);
		ret = file->pos += edx;
	}
	kfree(buffer);
	enable_intr();
	return ret;
}

/* POSIX规范-获取文件大小 */
static uint32_t syscall_posix_size(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	if (ebx < 0 || ebx == 1 || ebx == 2) return -1;
	cfile_t file = get_current_proc()->file_table[ebx];
	if (file == 0) return -1;
	return file->handle->size;
}

/* 发送格式化输出到标准输出 */
static uint32_t syscall_printf(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	printk((const char *)ebx);
	return 0;
}

/* 发送字符到标准输出 */
static uint32_t syscall_putchar(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	putchar(ebx);
	return 0;
}

/* 分配内存并返回地址 */
static uint32_t syscall_malloc(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	return (uint32_t)kmalloc(ebx);
}

/* 释放分配的内存并合并 */
static uint32_t syscall_free(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	kfree((void *)ebx);
	return 0;
}

/* 挂载设备到文件夹 */
static uint32_t syscall_mount(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	return vfs_mount((const char *)ebx, vfs_open((const char *)ecx));
}

/* 释放挂载的设备 */
static uint32_t syscall_umount(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	return vfs_umount((const char *)ebx);
}

/* 关闭电源 */
static uint32_t syscall_poweroff(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	power_off();
	return 0;
}

/* 重启计算机 */
static uint32_t syscall_reboot(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	power_reset();
	return 0;
}

/* 使进程延迟 */
static uint32_t syscall_sleep(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	sleep(ebx);
	return 0;
}

/* 蜂鸣器控制 */
static uint32_t syscall_beep(uint32_t ebx, uint32_t ecx, uint32_t edx, uint32_t esi, uint32_t edi)
{
	system_beep(ebx);
	return 0;
}

/* 系统调用表 */
syscall_t syscall_handlers[MAX_SYSCALLS] = {
	[1] = syscall_posix_open,
	[2] = syscall_posix_close,
	[3] = syscall_posix_read,
	[4] = syscall_posix_size,
	[5] = syscall_printf,
	[6] = syscall_putchar,
	[7] = syscall_malloc,
	[8] = syscall_free,
	[9] = syscall_mount,
	[10] = syscall_umount,
	[11] = syscall_poweroff,
	[12] = syscall_reboot,
	[13] = syscall_sleep,
	[14] = syscall_beep
};

/* 系统调用处理 */
unsigned int syscall(void)
{
	volatile unsigned int eax, ebx, ecx, edx, esi, edi;
	__asm__("mov %%eax, %0\n\t" : "=r"(eax));
	__asm__("mov %%ebx, %0\n\t" : "=r"(ebx));
	__asm__("mov %%ecx, %0\n\t" : "=r"(ecx));
	__asm__("mov %%edx, %0\n\t" : "=r"(edx));
	__asm__("mov %%esi, %0\n\t" : "=r"(esi));
	__asm__("mov %%edi, %0\n\t" : "=r"(edi));
	if (0 <= eax && eax < MAX_SYSCALLS && syscall_handlers[eax] != 0) {
		eax = ((syscall_t)syscall_handlers[eax])(ebx, ecx, edx, esi, edi);
	} else {
		eax = -1;
	}
	return eax;
}

/* 初始化系统调用 */
void syscall_init(void)
{
	print_busy("Setting up system calls...\r");
	idt_use_reg(0x80, (uint32_t)asm_syscall_handler);
	print_succ("The system call setup is complete.\n");
}
