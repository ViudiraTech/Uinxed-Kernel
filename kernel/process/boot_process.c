#include <kernel/boot_process.h>

void boot_start_init_before_debug(boot_process_start_t start_init, boot_process_start_t start_debug)
{
    if (start_init) start_init();
    if (start_debug) start_debug();
}
