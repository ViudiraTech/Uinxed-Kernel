#include "idt.h"
#include "pcb.h"

int add_task(pcb_t *new_task);

void remove_task(pcb_t *task);

void enable_scheduler();

void disable_scheduler();

int get_scheduler();

int scheduler(interrupt_frame_t *frame, regs_t *regs);

void timer_handle(interrupt_frame_t *frame);

// 切换上下文：保存 old，恢复 new
void switch_to(pcb_t *source, pcb_t *target, interrupt_frame_t *frame, regs_t *regs);

void init_scheduler();

// 保存寄存器上下文
#define save_regs()                               \
    do {                                          \
        __asm__ __volatile__("sub $0x10,%rsp\n\t" \
                             "pushq %r15\n\t"     \
                             "pushq %r14\n\t"     \
                             "pushq %r13\n\t"     \
                             "pushq %r12\n\t"     \
                             "pushq %r11\n\t"     \
                             "pushq %r10\n\t"     \
                             "pushq %r9\n\t"      \
                             "pushq %r8\n\t"      \
                             "pushq %rdi\n\t"     \
                             "pushq %rsi\n\t"     \
                             "pushq %rbp\n\t"     \
                             "pushq %rdx\n\t"     \
                             "pushq %rcx\n\t"     \
                             "pushq %rbx\n\t"     \
                             "pushq %rax\n\t"     \
                             "mov %gs,%ax\n\t"    \
                             "pushq %rax\n\t"     \
                             "mov %fs,%ax\n\t"    \
                             "pushq %rax\n\t"     \
                             "mov %es,%ax\n\t"    \
                             "pushq %rax\n\t"     \
                             "mov %ds,%ax\n\t"    \
                             "pushq %rax\n\t");   \
    } while (0)

#define save_regs_asm     \
    "sub $0x10,%%rsp\n\t" \
    "pushq %%r15\n\t"     \
    "pushq %%r14\n\t"     \
    "pushq %%r13\n\t"     \
    "pushq %%r12\n\t"     \
    "pushq %%r11\n\t"     \
    "pushq %%r10\n\t"     \
    "pushq %%r9\n\t"      \
    "pushq %%r8\n\t"      \
    "pushq %%rdi\n\t"     \
    "pushq %%rsi\n\t"     \
    "pushq %%rbp\n\t"     \
    "pushq %%rdx\n\t"     \
    "pushq %%rcx\n\t"     \
    "pushq %%rbx\n\t"     \
    "pushq %%rax\n\t"     \
    "mov %%gs,%%ax\n\t"   \
    "pushq %%rax\n\t"     \
    "mov %%fs,%%ax\n\t"   \
    "pushq %%rax\n\t"     \
    "mov %%es,%%ax\n\t"   \
    "pushq %%rax\n\t"     \
    "mov %%ds,%%ax\n\t"   \
    "pushq %%rax\n\t"

#define save_regs_asm_   \
    "sub $0x10,%rsp\n\t" \
    "pushq %r15\n\t"     \
    "pushq %r14\n\t"     \
    "pushq %r13\n\t"     \
    "pushq %r12\n\t"     \
    "pushq %r11\n\t"     \
    "pushq %r10\n\t"     \
    "pushq %r9\n\t"      \
    "pushq %r8\n\t"      \
    "pushq %rdi\n\t"     \
    "pushq %rsi\n\t"     \
    "pushq %rbp\n\t"     \
    "pushq %rdx\n\t"     \
    "pushq %rcx\n\t"     \
    "pushq %rbx\n\t"     \
    "pushq %rax\n\t"     \
    "xor %rax,%rax\n\t"  \
    "mov %gs,%ax\n\t"    \
    "pushq %rax\n\t"     \
    "mov %fs,%ax\n\t"    \
    "pushq %rax\n\t"     \
    "mov %es,%ax\n\t"    \
    "pushq %rax\n\t"     \
    "mov %ds,%ax\n\t"    \
    "pushq %rax\n\t"

// 恢复寄存器
#define restore_regs()                                               \
    __asm__ __volatile__(/* 恢复段寄存器 */                          \
                         "popq %rax\n\t"                             \
                         "mov %ax, %ds\n\t" /* 恢复 DS */            \
                         "popq %rax\n\t"                             \
                         "mov %ax, %es\n\t" /* 恢复 ES */            \
                         "popq %rax\n\t"                             \
                         "mov %ax, %fs\n\t" /* 恢复 FS */            \
                         "popq %rax\n\t"                             \
                         "mov %ax, %gs\n\t" /* 恢复 GS */            \
                                                                     \
                         "popq %rax\n\t" /* 恢复 RAX */              \
                         "popq %rbx\n\t" /* 恢复 RBX */              \
                         "popq %rcx\n\t" /* 恢复 RCX */              \
                         "popq %rdx\n\t" /* 恢复 RDX */              \
                         "popq %rbp\n\t" /* 恢复 RBP */              \
                         "popq %rsi\n\t" /* 恢复 RSI */              \
                         "popq %rdi\n\t" /* 恢复 RDI */              \
                         "popq %r8\n\t"  /* 恢复 R8 */               \
                         "popq %r9\n\t"  /* 恢复 R9 */               \
                         "popq %r10\n\t" /* 恢复 R10 */              \
                         "popq %r11\n\t" /* 恢复 R11 */              \
                         "popq %r12\n\t" /* 恢复 R12 */              \
                         "popq %r13\n\t" /* 恢复 R13 */              \
                         "popq %r14\n\t" /* 恢复 R14 */              \
                         "popq %r15\n\t" /* 恢复 R15 */              \
                                                                     \
                         "add $0x10,%rsp\n\t" /* 跳过error code,v */ \
    )

#define restore_regs_asm               \
    /* 恢复段寄存器 */                 \
    "popq %%rax\n\t"                   \
    "mov %%ax, %%ds\n\t" /* 恢复 DS */ \
    "popq %%rax\n\t"                   \
    "mov %%ax, %%es\n\t" /* 恢复 ES */ \
    "popq %%rax\n\t"                   \
    "mov %%ax, %%fs\n\t" /* 恢复 FS */ \
    "popq %%rax\n\t"                   \
    "mov %%ax, %%gs\n\t" /* 恢复 GS */ \
                                       \
    "popq %%rax\n\t" /* 恢复 RAX */    \
    "popq %%rbx\n\t" /* 恢复 RBX */    \
    "popq %%rcx\n\t" /* 恢复 RCX */    \
    "popq %%rdx\n\t" /* 恢复 RDX */    \
    "popq %%rbp\n\t" /* 恢复 RBP */    \
    "popq %%rsi\n\t" /* 恢复 RSI */    \
    "popq %%rdi\n\t" /* 恢复 RDI */    \
    "popq %%r8\n\t"  /* 恢复 R8 */     \
    "popq %%r9\n\t"  /* 恢复 R9 */     \
    "popq %%r10\n\t" /* 恢复 R10 */    \
    "popq %%r11\n\t" /* 恢复 R11 */    \
    "popq %%r12\n\t" /* 恢复 R12 */    \
    "popq %%r13\n\t" /* 恢复 R13 */    \
    "popq %%r14\n\t" /* 恢复 R14 */    \
    "popq %%r15\n\t" /* 恢复 R15 */    \
                                       \
    "add $0x10,%%rsp\n\t" /* 跳过error code,v */

#define restore_regs_asm_            \
    /* 恢复段寄存器 */               \
    "popq %rax\n\t"                  \
    "mov %ax, %ds\n\t" /* 恢复 DS */ \
    "popq %rax\n\t"                  \
    "mov %ax, %es\n\t" /* 恢复 ES */ \
    "popq %rax\n\t"                  \
    "mov %ax, %fs\n\t" /* 恢复 FS */ \
    "popq %rax\n\t"                  \
    "mov %ax, %gs\n\t" /* 恢复 GS */ \
                                     \
    "popq %rax\n\t" /* 恢复 RAX */   \
    "popq %rbx\n\t" /* 恢复 RBX */   \
    "popq %rcx\n\t" /* 恢复 RCX */   \
    "popq %rdx\n\t" /* 恢复 RDX */   \
    "popq %rbp\n\t" /* 恢复 RBP */   \
    "popq %rsi\n\t" /* 恢复 RSI */   \
    "popq %rdi\n\t" /* 恢复 RDI */   \
    "popq %r8\n\t"  /* 恢复 R8 */    \
    "popq %r9\n\t"  /* 恢复 R9 */    \
    "popq %r10\n\t" /* 恢复 R10 */   \
    "popq %r11\n\t" /* 恢复 R11 */   \
    "popq %r12\n\t" /* 恢复 R12 */   \
    "popq %r13\n\t" /* 恢复 R13 */   \
    "popq %r14\n\t" /* 恢复 R14 */   \
    "popq %r15\n\t" /* 恢复 R15 */   \
                                     \
    "add $0x10,%rsp\n\t" /* 跳过error code,v */
