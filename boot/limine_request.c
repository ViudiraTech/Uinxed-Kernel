/*
 *
 *      limine_request.c
 *      Kernel limine request
 *
 *      2024/6/23 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <limine.h>
#include <uinxed.h>

__attribute__((used, section(".limine_requests"))) volatile struct limine_rsdp_request rsdp_request = {
    .id       = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_kernel_file_request kernel_file_request = {
    .id       = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0,
    .response = 0,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_smp_request smp_request = {
    .id       = LIMINE_SMP_REQUEST,
    .revision = 0,
    .response = 0,
    .flags    = 1,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_framebuffer_request framebuffer_request = {
    .id       = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_smbios_request smbios_request = {
    .id = LIMINE_SMBIOS_REQUEST,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_memmap_request memmap_request = {
    .id       = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_hhdm_request hhdm_request = {
    .id       = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_kernel_address_request kernel_address_request
    = {.id = LIMINE_KERNEL_ADDRESS_REQUEST, .revision = 0};

__attribute__((used, section(".limine_requests"))) volatile struct limine_entry_point_request entry_point_request = {
    .id       = LIMINE_ENTRY_POINT_REQUEST,
    .revision = 3,
    .entry    = &kernel_entry,
};

__attribute__((used, section(".limine_requests"))) volatile struct limine_module_request module_request
    = {.id = LIMINE_MODULE_REQUEST, .revision = 0};
