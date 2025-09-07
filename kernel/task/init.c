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

int init_kmain(int *test)
{
    printk("\n[    INFO    ]Init process is running. test=%d\n", *test);
    enable_scheduler();
    enable_intr();
    while (1) { __asm__("hlt"); }
    return 0; // nerver get there
}
