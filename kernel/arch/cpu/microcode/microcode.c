/*
 *
 *      microcode.c
 *      Processor microcode update core
 *
 *      2026/10/5 Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/cpuid.h>
#include <arch/cpu/microcode/amd/amd.h>
#include <arch/cpu/microcode/intel/intel.h>
#include <arch/cpu/microcode/microcode.h>
#include <arch/cpu/smp.h>
#include <boot/limine_module.h>
#include <kernel/cmdline/cmdline.h>
#include <kernel/printk.h>
#include <libs/std/string.h>

/* Every backend the core can drive; append a new vendor's table here. */
static const microcode_ops_t *const microcode_backends[] = {
    &intel_microcode_ops,
    &amd_microcode_ops,
};

/* Backend selected for this machine; NULL until init_microcode() has run. */
static const microcode_ops_t *microcode_backend;

/* Set when the platform makes early loading pointless or unsafe. */
static int microcode_loader_disabled;

/* Revision the boot processor reported, and one slot per other processor, bounded like the kernel's other per-CPU state. */
static uint32_t microcode_boot_revision;
static uint32_t microcode_ap_revisions[CONFIG_NMI_LOG_MAX_CPUS];

/* Remember what the boot processor reports, so readers stay off the MSRs. */
static void microcode_cache_revision(void)
{
    if (microcode_backend && microcode_backend->revision) microcode_boot_revision = microcode_backend->revision();
}

/* Pick the first backend that claims the running CPU. */
static const microcode_ops_t *microcode_find_backend(void)
{
    for (size_t i = 0; i < sizeof(microcode_backends) / sizeof(microcode_backends[0]); i++)
        if (microcode_backends[i]->match()) return microcode_backends[i];
    return NULL;
}

/* The module called @name, if its content really is a blob for this vendor. */
static const microcode_blob_t *microcode_blob_by_name(const char *name, const microcode_ops_t *ops, microcode_blob_t *blob)
{
    lmodule_t *module = get_lmodule(name);
    if (!module || !module->data || !module->size || !ops->probe(module->data, module->size)) return NULL;

    blob->name = module->name;
    blob->data = module->data;
    blob->size = module->size;

    return blob;
}

/* The blob to use: this processor's own name first, then the vendor wide names. */
static const microcode_blob_t *microcode_find_blob(const microcode_ops_t *ops, microcode_blob_t *blob)
{
    const char *specific = ops->builtin_name ? ops->builtin_name() : NULL;

    if (specific && microcode_blob_by_name(specific, ops, blob)) return blob;
    for (size_t i = 0; i < ops->module_name_count; i++)
        if (microcode_blob_by_name(ops->module_names[i], ops, blob)) return blob;

    return NULL;
}

/* The "microcode=" option; the switch is also accepted standing on its own. */
static int microcode_cmdline_disabled(void)
{
    const char *cmdline = get_cmdline();
    char        option[64];

    if (cmdline_get_option("microcode", option, sizeof(option))) {
        for (char *token = option; token && *token;) {
            char *comma = strchr(token, ',');

            if (comma) *comma = '\0';
            if (!strcmp(token, "dis_ucode_ldr")) return 1;

            token = comma ? comma + 1 : NULL;
        }
    }

    /* The switch may also stand alone, with no "microcode=" in front of it. */
    for (const char *token = cmdline; token && *token;) {
        const char *end = token;

        while (*end && *end != ' ' && *end != '\t') end++;
        if (streq_trimmed(token, (size_t)(end - token), "dis_ucode_ldr")) return 1;

        while (*end == ' ' || *end == '\t') end++;
        token = *end ? end : NULL;
    }

    return 0;
}

/* Update the boot processor from the bootloader supplied microcode module. */
void init_microcode(void)
{
    microcode_blob_t blob;
    uint32_t         ecx;

    /* Detected before the gates below, so microcode_revision() still works. */
    microcode_backend = microcode_find_backend();
    if (!microcode_backend) {
        plogk("microcode: no support for this CPU vendor.\n");
        return;
    }

    microcode_cache_revision();

    /* A guest leaves the host's microcode alone; CPUID.1:ECX[31] says one is running us. */
    cpuid_safe(0x00000001, 0, NULL, NULL, &ecx, NULL);
    if (ecx & (1U << 31)) {
        microcode_loader_disabled = 1;
        plogk("microcode: early loading disabled by a hypervisor.\n");
        return;
    }

    if (microcode_cmdline_disabled()) {
        microcode_loader_disabled = 1;
        plogk("microcode: early loading disabled on the command line.\n");
        return;
    }

    /* No module is the normal case: nothing to load, nothing to report. */
    if (!microcode_find_blob(microcode_backend, &blob)) return;

    plogk("microcode: %s blob \"%s\", %zu bytes.\n", microcode_backend->name, blob.name, blob.size);
    microcode_backend->load(&blob);

    /* The update just changed it. */
    microcode_cache_revision();
}

/* Update the calling application processor with the selected update. */
void init_microcode_ap(void)
{
    uint32_t cpu;

    if (microcode_loader_disabled || !microcode_backend) return;
    microcode_backend->load_ap();

    /* Record this CPU's own revision; its id is known by the time this runs. */
    cpu = get_current_cpu_id();
    if (microcode_backend->revision && cpu < CONFIG_NMI_LOG_MAX_CPUS) microcode_ap_revisions[cpu] = microcode_backend->revision();
}

/* Revision @cpu runs, for /proc/cpuinfo. */
uint32_t microcode_revision(uint32_t cpu)
{
    if (cpu < CONFIG_NMI_LOG_MAX_CPUS && microcode_ap_revisions[cpu]) return microcode_ap_revisions[cpu];
    return microcode_boot_revision;
}
