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
    current_task->state = DEATH;
    for (;;) __asm__("hlt");
}

int kthread_entry(void **args)
{
    int (*_start)(void *) = args[0];
    if (_start == NULL)
    {
        panic("_start=NULL");
    }
    int status            = _start(args[1]);
    kthread_exit(status);
    return 0;
}

pcb_t *kernel_thread(int (*_start)(void *arg), void *args, char *name)
{
    int s = get_scheduler();
    disable_scheduler();
    pcb_t *new_task = (pcb_t *)calloc(1, sizeof(pcb_t) + STACK_SIZE);
    if (new_task == NULL) { panic("No enough Memory to alloc for new tasks\r\n"); }
    memset(new_task, 0, sizeof(pcb_t));
    new_task->name  = (char *)malloc(strlen(name) * sizeof(char));
    if (new_task->name == NULL)
    {
        panic("No enough Memory to alloc for new tasks\r\n");
    }
    
    new_task->level = 0;
    new_task->time  = 100;

    strcpy(new_task->name, name);
    uint64_t *stack_top       = (uint64_t *)(new_task + sizeof(pcb_t) + STACK_SIZE);
    *(--stack_top)            = (uint64_t)_start;
    new_task->context0.rflags = 0x202;
    new_task->context0.rip    = (uint64_t)_start;
    new_task->context0.rsp    = (uint64_t)stack_top - sizeof(uint64_t) * 3; // 设置上下文
    new_task->context0.rdi    = (uint64_t)args;
    new_task->context0.rsp    = (new_task->context0.rsp & (~0xF)) - sizeof(uint64_t); // 栈16字节对齐
    new_task->kernel_stack    = new_task->context0.rsp;
    new_task->user_stack      = new_task->kernel_stack;
    new_task->pid             = now_pid++;
    new_task->page_dir        = get_kernel_pagedir();
    new_task->flag            = 0 | PCB_FLAGS_KTHREAD;
    new_task->state           = READY; //就绪态
    add_task(new_task);
    if (s == 1) { enable_scheduler(); }
    return new_task;
}

pcb_t *create_kernel_thread(int (*_start)(void *arg), void *args, char *name)
{
    void **arg = (void **)malloc(sizeof(void *) * 2);
    if (!arg) { return NULL; }
    arg[0] = _start;
    arg[1] = args;
    return kernel_thread((int (*)(void *))kthread_entry, arg, name);
}

pcb_t *init_task()
{
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
    plogk("idle stack: %p\tinit stack:%p\n", (void *)(uintptr_t)idle_pcb[0]->context0.rsp, (void *)(uintptr_t)init_pcb->context0.rsp);
    return init_pcb;
}
