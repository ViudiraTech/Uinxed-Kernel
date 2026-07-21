#ifndef INCLUDE_ELF_LOADER_H_
#define INCLUDE_ELF_LOADER_H_

#include <stddef.h>
#include <stdint.h>

struct process;

int elf_loader_load_user_process(struct process *proc, const uint8_t *elf_data, size_t elf_size);

#endif /* INCLUDE_ELF_LOADER_H_ */
