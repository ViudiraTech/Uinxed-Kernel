#ifndef PCB_H
#define PCB_H
#include "ctype.h"
#include "idt.h"
#include "page.h"
#include "stdlib.h"

#define PCB_FLAGS_KTHREAD (1UL << 0)
#define PCB_FLAGS_SWITCH_TO_USER (1UL << 1)
#define C_F_CLONE_ADDRESS ((uint8_t)1 << 0) //克隆/共享虚拟地址空间
#define C_F_CLONE_SOURCES ((uint8_t)1 << 1) //克隆/共享资源
typedef struct task_regs {
  uint64_t ds, es, fs, gs;
  uint64_t rax, rbx, rcx, rdx, rbp, rsi, rdi;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t vector;   // 保留
  uint64_t err_code; // 保留
  // CPU自动压入
  uint64_t rip;
  uint64_t cs;
  uint64_t rflags;
  uint64_t rsp;
  uint64_t ss;
} regs_t;

typedef struct task_regs_ {
  uint64_t ds, es, fs, gs;
  uint64_t rax, rbx, rcx, rdx, rbp, rsi, rdi;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t vector;   // 保留
  uint64_t err_code; // 保留
  // CPU自动压入
  union {
    struct {
      uint64_t rip;
      uint64_t cs;
      uint64_t rflags;
      uint64_t rsp;
      uint64_t ss;
    } frame_regs;
    interrupt_frame_t frame;
  } auto_regs;
} regs_t_;

typedef struct {
  uint64_t r15;
  uint64_t r14;
  uint64_t r13;
  uint64_t r12;
  uint64_t r11;
  uint64_t r10;
  uint64_t r9;
  uint64_t r8;
  uint64_t rbx;
  uint64_t rbp;
  uint64_t rip;
  uint64_t rsp;
  uint64_t rflags;
  uint64_t rax;
  uint64_t rcx;
  uint64_t rdx;
  uint64_t rdi;
  uint64_t rsi;
} TaskContext;

typedef struct task_pcb {
  uint32_t pid;
  char *name;
  uint8_t level; // 0:system（例如任务队列[一种特殊的内核线程]）
                 // 1:服务器/驱动
                 // 2:用户
                 // 3:仅用于idle
  uint8_t nice;  // 优先级(数字越大优先级越高)，每次用尽时间片就+1直到0
  uint32_t time; // 时间片（虚拟运行时间）
  uint8_t state; // 0:可调度(就绪态)
                 // 1:阻塞
                 // 2:调试暂停
                 // 3:运行中
                 // 4:僵死(可销毁)
  uint8_t init_nice;    //初始优先级(数字越大优先级越高)
  TaskContext context0; // 上下文
  struct page_directory *page_dir; // 进程页表
  struct task_pcb *father;         //父进程
  list_t *childs;
  void *sources;         //持有资源列表(暂不实现)
  uint64_t kernel_stack; // 内核栈
  uint64_t user_stack;   // 用户栈
  uint64_t flag;
} pcb_t;

pcb_t *init_task();

pcb_t *get_current_task();

int idle_thread();

extern pcb_t **idle_pcb;

extern pcb_t *current_task;

extern pcb_t *init_pcb;

extern list_t *current_task_ls;

extern list_t *pcb_list;

#endif