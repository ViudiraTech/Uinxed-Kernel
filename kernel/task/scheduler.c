#include "scheduler.h"
#include "acpi.h"
#include "alloc.h"
#include "apic.h"
#include "debug.h"
#include "heap.h"
#include "idt.h"
#include "page.h"
#include "printk.h"
#include "smp.h"
#include "spin_lock.h"
#include "stddef.h"

int *is_scheduler; // 0:disable
// 1:enable

list_t    *pcb_list = NULL;
spinlock_t pcb_list_lock;

void enable_scheduler()
{
    is_scheduler[get_current_cpu_id()] = 1;
}

void disable_scheduler()
{
    is_scheduler[get_current_cpu_id()] = 0;
}

int get_scheduler()
{
    return is_scheduler[get_current_cpu_id()];
}

int add_task(pcb_t *new_task)
{
    spin_lock(&pcb_list_lock);
    if (pcb_list == NULL) {
        pcb_list       = (list_t *)malloc(sizeof(list_t));
        pcb_list->data = (void *)new_task;
        pcb_list->pre  = pcb_list;
        pcb_list->next = pcb_list;
    } else {
        list_t *p;
        for (p = pcb_list; p->next != pcb_list; p = p->next) {}
        p->next       = (list_t *)malloc(sizeof(list_t));
        p->next->data = (void *)new_task;
        p->next->next = pcb_list;
        p->next->pre  = p;
    }
    spin_unlock(&pcb_list_lock);
    return 0;
}

void remove_task(pcb_t *task)
{
    ((void)task);
}

//简易轮转调度
int scheduler(interrupt_frame_t *frame, regs_t *regs)
{
    if (is_scheduler == 0) { return 1; }
    spin_lock(&pcb_list_lock);
    list_t *next = pcb_list;
    if (current_task == NULL) {
        current_task = idle_pcb[0];
        current_task->context0.rip = frame->rip;
    }
    current_task->state = READY;
    for (;; next = next->next) {
        // printk("[Debug]next:%s\n", ((pcb_t *)(next->data))->name);
        // printk("\tnext state:%s\n", (((pcb_t *)(next->data))->state)==READY ? "READY" : "UNREADY");
        if (((pcb_t *)(next->next->data))->state == READY && ((pcb_t *)(next->next->data)) != current_task)
        {
            break;
        }
    }
    next                = next->next;
    pcb_t *now          = current_task;
    current_task        = ((pcb_t *)(next->data));
    current_task->state = RUNNING;
    spin_unlock(&pcb_list_lock);
    switch_to(now, current_task, frame, regs);
    return 0;
}

void switch_to(pcb_t *source, pcb_t *target, interrupt_frame_t *frame, regs_t *regs)
{
    switch_page_directory(target->page_dir);
    // TODO: Switch FPU context
    TaskContext *old = &(source->context0), *new = &(target->context0);
    old->r15 = regs->r15;
    old->r14 = regs->r14;
    old->r13 = regs->r13;
    old->r12 = regs->r12;
    old->r11 = regs->r11;
    old->r10 = regs->r10;
    old->r9  = regs->r9;
    old->r8  = regs->r8;
    old->rax = regs->rax;
    old->rbx = regs->rbx;
    old->rcx = regs->rcx;
    old->rdx = regs->rdx;
    old->rbp = regs->rbp;
    old->rsi = regs->rsi;
    old->rdi = regs->rdi;
    old->rsp = frame->rsp;
    // 创建新任务寄存器上下文
    regs_t new_regs = (regs_t) {
        // 段寄存器
        .ds = 0x10,
        .es = 0x10,
        .fs = 0x10,
        .gs = 0x10,

        // 通用寄存器
        .rax = new->rax,
        .rbx = new->rbx,
        .rcx = new->rcx,
        .rdx = new->rdx,
        .rbp = new->rbp,
        .rsi = new->rsi,
        .rdi = new->rdi,
        .r8  = new->r8,
        .r9  = new->r9,
        .r10 = new->r10,
        .r11 = new->r11,
        .r12 = new->r12,
        .r13 = new->r13,
        .r14 = new->r14,
        .r15 = new->r15,

        // 中断信息
        .vector   = 0, // 中断向量号
        .err_code = 0, // 错误代码

        // CPU 自动保存部分
        .rip    = new->rip, // 指令指针
        .cs     = 0x8,      // 内核代码段选择子
        .rflags = new->rflags,
        .rsp    = new->rsp, // 栈指针
        .ss     = 0x10      // 内核数据段选择子
    };
    if ((target->flag & PCB_FLAGS_KTHREAD) == 0) {
        new_regs.cs = 0x20;
        new_regs.ss = 0x18;
    }
    frame->rip    = new_regs.rip;
    frame->cs     = new_regs.cs;
    frame->rflags = new_regs.rflags;
    if (frame->cs == 0x8) {
        frame->rsp = new_regs.rsp;
        frame->ss  = new_regs.ss;
    }
    // 伪造寄存器上下文
    regs->r15 = new->r15;
    regs->r14 = new->r14;
    regs->r13 = new->r13;
    regs->r12 = new->r12;
    regs->r11 = new->r11;
    regs->r10 = new->r10;
    regs->r9  = new->r9;
    regs->r8  = new->r8;
    regs->rax = new->rax;
    regs->rbx = new->rbx;
    regs->rcx = new->rcx;
    regs->rdx = new->rdx;
    regs->rbp = new->rbp;
    regs->rsi = new->rsi;
    regs->rdi = new->rdi;
}

void init_scheduler()
{
    if ((is_scheduler = calloc(get_cpu_count(), sizeof(*is_scheduler))) == NULL) panic("Cannot alloc memory for is_scheduler!\n");
    plogk("is_scheduler inited successfully, cpu_count = %d\n", get_cpu_count());
}
