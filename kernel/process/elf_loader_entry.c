/*
 *
 *      elf_loader_entry.c
 *      Elf loader entry
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <libs/std/stdbool.h>
#include <process/elf_loader.h>
#include <process/process.h>

/* Load a regular userspace process image from memory */
int elf_loader_load_user_process(process_t *proc, const uint8_t *elf_data, size_t elf_size, char *const argv[], char *const envp[], uintptr_t *entry_out, uintptr_t *rsp_out)
{
    return elf_loader_load_process_internal(proc, elf_data, elf_size, argv, envp, entry_out, rsp_out);
}

/* Load the init image as PID 1 */
int elf_loader_load_initial_process(process_t *proc, const uint8_t *elf_data, size_t elf_size, char *const argv[], char *const envp[])
{
    if (!proc || !proc->task || proc->task->pid != 1) return -EINVAL;
    return elf_loader_load_process_internal(proc, elf_data, elf_size, argv, envp, NULL, NULL);
}
