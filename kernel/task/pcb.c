#include "pcb.h"
#include "alloc.h"
#include "common.h"
#include "debug.h"
#include "heap.h"
#include "init.h"
#include "printk.h"
#include "scheduler.h"
#include "smp.h"
#include "string.h"
#include "uinxed.h"

uint32_t now_pid = 0;
pcb_t  **idle_pcb;
pcb_t  **current_tasks;
pcb_t   *init_pcb;

void kthread_exit(int status)
{
    (void)status;
    plogk("pid%d exited, exit status:%d\n", current_task->pid, status);
    for (;;) __asm__("hlt");
}

void kthread_entry(void **args)
{
    int (*_start)(void *) = args[0];
    int status = _start(args[1]);
    kthread_exit(status);
}

pcb_t *kernel_thread(int (*_start)(void *arg), void *args, char *name)
{
    __asm__("cli");
    int s = get_scheduler();
    disable_scheduler();
    pcb_t *new_task = (pcb_t *)calloc(1, KERNEL_ST_SZ);
    if (new_task == NULL) { panic("No enough Memory\r\n"); }
    memset(new_task, 0, sizeof(pcb_t));
    new_task->name  = (char *)malloc(strlen(name) * sizeof(char));
    new_task->level = 0;
    new_task->time  = 100;

    strcpy(new_task->name, name);
    uint64_t *stack_top       = (uint64_t *)(new_task + (STACK_SIZE / sizeof(*new_task)));
    *(--stack_top)            = (uint64_t)_start;
    new_task->context0.rflags = 0x202;
    new_task->context0.rip    = (uint64_t)_start;
    new_task->context0.rsp    = (uint64_t)new_task + STACK_SIZE - sizeof(uint64_t) * 3; // 设置上下文
    new_task->context0.rdi    = (uint64_t)args;
    new_task->kernel_stack    = (new_task->context0.rsp &= ~0xF); // 栈16字节对齐
    new_task->user_stack      = new_task->kernel_stack;
    new_task->pid             = now_pid++;
    new_task->page_dir        = get_kernel_pagedir();
    new_task->flag            = 0 | PCB_FLAGS_KTHREAD;
    new_task->state           = 0; //就绪态
    add_task(new_task);
    if (s == 1) { enable_scheduler(); }

    __asm__("sti");
    return new_task;
}

pcb_t *create_kernel_thread(int (*_start)(void *arg), void *args, char *name)
{
    void **arg = (void **)malloc(sizeof(void *) * 2);
    if (!arg) { return NULL; }
    arg[0] = _start;
    arg[1] = args;
    return kernel_thread((int (*)(void*))kthread_entry, arg, name);
}

pcb_t *init_task()
{
    init_scheduler();
    idle_pcb           = (pcb_t **)calloc(sizeof(pcb_t *), get_cpu_count());
    current_tasks      = (pcb_t **)calloc(sizeof(pcb_t *), get_cpu_count());
    uint32_t cpu_count = get_cpu_count();
    for (uint32_t i = 0; i < cpu_count; i++) {
        idle_pcb[i]        = create_kernel_thread(idle_thread, NULL, "System(idle)");
        idle_pcb[i]->level = 3;
    }
    int *p   = (int *)malloc(sizeof(int));
    *p       = 114514;
    init_pcb = create_kernel_thread((int (*)(void *))init_kmain, p, "init");
    plogk("idle stack: %p\tinit stack:%p\n\t", idle_pcb[0]->context0.rsp, init_pcb->context0.rsp);
    return init_pcb;
}
