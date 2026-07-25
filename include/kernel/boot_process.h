#ifndef INCLUDE_KERNEL_BOOT_PROCESS_H_
#define INCLUDE_KERNEL_BOOT_PROCESS_H_

typedef void (*boot_process_start_t)(void);

void boot_start_init_before_debug(boot_process_start_t start_init, boot_process_start_t start_debug);

#endif
