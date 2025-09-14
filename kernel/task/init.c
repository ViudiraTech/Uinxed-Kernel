#include "common.h"
#include "pcb.h"
#include "printk.h"
#include "scheduler.h"

int idle_thread()
{
    for (;;) __asm__("hlt");
}

int init_user_main()
{
    plogk("Hello World!\r\n");
    for (;;) {}
}

void ps()
{
    printk("pid: %d\tname:%s\tstatus:%d\r\n", ((pcb_t *)pcb_list->data)->pid, ((pcb_t *)pcb_list->data)->name, ((pcb_t *)pcb_list->data)->state);
    for (list_t *p = pcb_list->next; p != pcb_list; p = p->next) {
        printk("pid: %d\tname:%s\tstatus:%d\r\n", ((pcb_t *)p->data)->pid, ((pcb_t *)p->data)->name, ((pcb_t *)p->data)->state);
    }
    return;
}

int init_kmain(int *test)
{
    printk("\n[    INFO    ]Init process is running. test=%d\n", *test);
    enable_scheduler();
    enable_intr();
    for (int i = 0; i < 230; i++)
    {
        create_kernel_thread(idle_thread,NULL,"test");
    }
    ps();
    while (1) { __asm__("hlt"); }
    return 0; // nerver get there
}
