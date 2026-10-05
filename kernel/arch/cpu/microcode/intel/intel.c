/*
 *
 *      intel.c
 *      Intel processor microcode update
 *
 *      2026/10/5 Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/cpuid.h>
#include <arch/cpu/microcode/intel/intel.h>
#include <arch/misc/common.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/vsprintf.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

static const char *const intel_microcode_modules[] = {
    "GenuineIntel",
    "intel-ucode",
    "microcode",
};

/* Update selected for the boot processor, kept for the application processors. */
static struct intel_microcode_header *intel_microcode_patch;

/* Payload size of a record; a zero datasize means the pre-Pentium 4 default. */
static uint32_t intel_datasize(const struct intel_microcode_header *mc)
{
    return mc->datasize ? mc->datasize : INTEL_MC_DEFAULT_DATASIZE;
}

/* Total size of a record, header and extended table included. */
static uint32_t intel_totalsize(const struct intel_microcode_header *mc)
{
    return mc->datasize ? mc->totalsize : INTEL_MC_DEFAULT_TOTALSIZE;
}

/* Size of the extended signature table described by @et. */
static size_t intel_exttable_size(const struct intel_microcode_extended_table *et)
{
    return (size_t)et->count * INTEL_MC_EXT_SIG_SIZE + INTEL_MC_EXT_HEADER_SIZE;
}

/* Sum of the 32-bit words of a region; a valid record sums to zero. */
static uint32_t intel_checksum(const void *data, size_t size)
{
    const uint32_t *words = data;
    uint32_t        sum   = 0;

    for (size_t i = 0; i < size / sizeof(uint32_t); i++) sum += words[i];
    return sum;
}

/* Read the revision the calling CPU currently runs. */
static uint32_t intel_microcode_revision(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* Intel SDM: clear the signature MSR, run CPUID.1, then read it back. */
    wrmsr(INTEL_MSR_MICROCODE_REV, 0);
    cpuid_safe(1, 0, &eax, &ebx, &ecx, &edx);
    return (uint32_t)(rdmsr(INTEL_MSR_MICROCODE_REV) >> 32);
}

/* Platform flag bit of the calling CPU, taken from MSR 0x17. */
static uint32_t intel_platform_flag(void)
{
    uint32_t family = get_cpu_family();
    uint32_t model  = get_cpu_model();

    /* Klamath and older have neither an update nor an IA32_PLATFORM_ID. */
    if (family < 6 || (family == 6 && model <= 3)) return 0;
    return 1U << (uint32_t)((rdmsr(INTEL_MSR_PLATFORM_ID) >> 50) & 7U);
}

/* Everything the matching rules need to know about the calling CPU. */
static void intel_collect_cpu_info(cpu_signature_t *sig)
{
    uint32_t eax, ebx, ecx, edx;

    cpuid_safe(1, 0, &eax, &ebx, &ecx, &edx);
    sig->sig = eax;
    sig->rev = intel_microcode_revision();
    sig->pf  = intel_platform_flag();
}

/* Validate one record: sizes, format versions and both checksums. */
static int intel_microcode_sanity_check(const struct intel_microcode_header *mc)
{
    const struct intel_microcode_extended_table *ext = NULL;
    size_t                                       total, data, ext_size;
    uint32_t                                     ext_count = 0;

    total = intel_totalsize(mc);
    data  = intel_datasize(mc);

    if (data + INTEL_MC_HEADER_SIZE > total) return -EINVAL;
    if (mc->ldrver != INTEL_MC_LOADER_VERSION || mc->hdrver != INTEL_MC_HEADER_TYPE) return -EINVAL;

    ext_size = total - (INTEL_MC_HEADER_SIZE + data);
    if (ext_size) {
        if (ext_size < INTEL_MC_EXT_HEADER_SIZE || (ext_size - INTEL_MC_EXT_HEADER_SIZE) % INTEL_MC_EXT_SIG_SIZE) return -EINVAL;
        ext = (const void *)((const uint8_t *)mc + INTEL_MC_HEADER_SIZE + data);

        /* Bound the declared count before the table size it implies is trusted. */
        if (ext->count > (ext_size - INTEL_MC_EXT_HEADER_SIZE) / INTEL_MC_EXT_SIG_SIZE) return -EINVAL;
        if (ext_size != intel_exttable_size(ext)) return -EINVAL;
        ext_count = ext->count;

        /* Every dword of a valid extended signature table sums to zero. */
        if (intel_checksum(ext, ext_size)) return -EINVAL;
    }

    /* The header and the payload sum to zero as well. */
    if (intel_checksum(mc, INTEL_MC_HEADER_SIZE + data)) return -EINVAL;
    for (uint32_t i = 0; i < ext_count; i++) {
        const struct intel_microcode_extended_sig *es    = &ext->sigs[i];
        uint32_t                                   delta = (mc->sig + mc->pf + mc->cksum) - (es->sig + es->pf + es->cksum);
        if (delta) return -EINVAL;
    }

    return 0;
}

/* Does a record's signature and platform mask cover this CPU? */
static int intel_signatures_match(const cpu_signature_t *sig, uint32_t mc_sig, uint32_t mc_pf)
{
    if (sig->sig != mc_sig) return 0;

    /* An empty mask matches every platform; only the PII ever has one. */
    if (!mc_pf) return 1;
    return (sig->pf & mc_pf) != 0;
}

/* Does this record, or one of its extended signatures, cover this CPU? */
static int intel_find_matching_signature(const struct intel_microcode_header *mc, const cpu_signature_t *sig)
{
    const struct intel_microcode_extended_table *ext;
    const struct intel_microcode_extended_sig   *es;

    if (intel_signatures_match(sig, mc->sig, mc->pf)) return 1;

    /* Without room for a table this record only covers its own signature. */
    if (intel_totalsize(mc) <= intel_datasize(mc) + INTEL_MC_HEADER_SIZE) return 0;
    ext = (const void *)((const uint8_t *)mc + intel_datasize(mc) + INTEL_MC_HEADER_SIZE);
    es  = ext->sigs;

    for (uint32_t i = 0; i < ext->count; i++, es++)
        if (intel_signatures_match(sig, es->sig, es->pf)) return 1;

    return 0;
}

/* Newest record for this CPU that beats its revision, or NULL; a blob that does not scan cleanly to its end is not trusted. */
static const struct intel_microcode_header *intel_scan_blob(const microcode_blob_t *blob, const cpu_signature_t *sig, int *error)
{
    const uint8_t                       *cursor  = blob->data;
    const struct intel_microcode_header *best    = NULL;
    size_t                               size    = blob->size;
    uint32_t                             cur_rev = sig->rev;

    *error = 0;

    while (size >= INTEL_MC_HEADER_SIZE) {
        const struct intel_microcode_header *mc      = (const void *)cursor;
        uint32_t                             mc_size = intel_totalsize(mc);

        if (!mc_size || mc_size > size || intel_microcode_sanity_check(mc) < 0) {
            *error = -EINVAL;
            break;
        }
        if (intel_find_matching_signature(mc, sig) && cur_rev < mc->rev) {
            best    = mc;
            cur_rev = mc->rev;
        }

        cursor += mc_size;
        size -= mc_size;
    }

    /* Trailing bytes mean the blob is not the flat record list it claims. */
    if (size) {
        *error = -EINVAL;
        return NULL;
    }

    return best;
}

/* Apply one already validated record to the calling CPU. */
static microcode_state_t intel_apply_microcode(const struct intel_microcode_header *mc)
{
    if (!mc) return MICROCODE_NFOUND;

    /* Skip the expensive MSR write when the sibling thread already did it. */
    if (intel_microcode_revision() >= mc->rev) return MICROCODE_OK;

    /* IA32_BIOS_UPDT_TRIG takes the linear address of the payload. */
    wrmsr(INTEL_MSR_MICROCODE_WRITE, (uint64_t)(uintptr_t)((const uint8_t *)mc + INTEL_MC_HEADER_SIZE));
    return intel_microcode_revision() == mc->rev ? MICROCODE_UPDATED : MICROCODE_ERROR;
}

/* Does this buffer start with a record this loader understands? */
static int intel_microcode_probe(const uint8_t *data, size_t size)
{
    const struct intel_microcode_header *mc;
    uint32_t                             mc_size;

    if (!data || size < INTEL_MC_HEADER_SIZE) return 0;
    mc      = (const void *)data;
    mc_size = intel_totalsize(mc);

    if (mc_size < INTEL_MC_HEADER_SIZE || mc_size > size) return 0;
    return intel_microcode_sanity_check(mc) == 0;
}

/* Keep a private copy: the APs reuse this record, not the blob's own bytes. */
static struct intel_microcode_header *intel_save_patch(const struct intel_microcode_header *mc)
{
    uint32_t                       size  = intel_totalsize(mc);
    struct intel_microcode_header *saved = malloc(size);

    if (!saved) return NULL;
    memcpy(saved, mc, size);
    return saved;
}

/* Scan @blob, keep the best record and apply it to the boot processor. */
static microcode_state_t intel_microcode_load(const microcode_blob_t *blob)
{
    const struct intel_microcode_header *patch;
    struct intel_microcode_header       *saved;
    cpu_signature_t                      sig;
    microcode_state_t                    state;
    uint32_t                             before;
    int                                  error = 0;

    intel_collect_cpu_info(&sig);
    before = sig.rev;
    patch  = intel_scan_blob(blob, &sig, &error);

    if (error) {
        plogk("microcode: Intel blob is malformed, nothing applied.\n");
        return MICROCODE_ERROR;
    }
    if (!patch) {
        plogk("microcode: no Intel update for CPU0 (signature 0x%08x, platform 0x%x), revision 0x%08x\n", sig.sig, sig.pf, before);
        return MICROCODE_NFOUND;
    }

    saved = intel_save_patch(patch);
    if (!saved) {
        plogk("microcode: out of memory for the Intel update (%u bytes)\n", intel_totalsize(patch));
        return MICROCODE_ERROR;
    }

    intel_microcode_patch = saved;

    state = intel_apply_microcode(saved);
    if (state == MICROCODE_UPDATED) {
        plogk("microcode: Intel CPU0 revision 0x%08x -> 0x%08x (%04x-%02x-%02x)\n", before, saved->rev, saved->date & 0xffffU, (saved->date >> 16) & 0xffU, (saved->date >> 24) & 0xffU);
    } else if (state == MICROCODE_OK) {
        plogk("microcode: Intel CPU0 revision 0x%08x is already current.\n", before);
    } else {
        plogk("microcode: Intel CPU0 update to revision 0x%08x did not take effect.\n", saved->rev);
    }

    return state;
}

/* Apply the kept record, or rescan it for this AP's stepping. */
static microcode_state_t intel_microcode_load_ap(void)
{
    const struct intel_microcode_header *patch = intel_microcode_patch;
    cpu_signature_t                      sig;
    microcode_state_t                    state;

    /* Nothing was selected at boot, so there is nothing to apply. */
    if (!patch) return MICROCODE_NFOUND;
    intel_collect_cpu_info(&sig);

    /* The boot processor's record is the one the APs apply; a record for another stepping is never pushed into a CPU it does not cover. */
    if (!intel_find_matching_signature(patch, &sig)) return MICROCODE_NFOUND;
    state = intel_apply_microcode(patch);

    /* APs share this line, so report a failure once. */
    if (state == MICROCODE_ERROR) plogk_once("microcode: an Intel application processor could not be updated.\n");
    return state;
}

/* Dispatcher hook: is the running processor a supported Intel one? */
static int intel_microcode_match(void)
{
    if (strcmp(get_vendor_name(), "GenuineIntel") != 0) return 0;

    /* Family 6 is the first Intel family with updates in this format. */
    return get_cpu_family() >= 6;
}

/* Name of the update for this exact processor, "<family>-<model>-<stepping>". */
static const char *intel_microcode_builtin_name(void)
{
    static char name[16];
    uint32_t    eax, ebx, ecx, edx;

    cpuid_safe(1, 0, &eax, &ebx, &ecx, &edx);
    snprintf(name, sizeof(name), "%02x-%02x-%02x", get_cpu_family(), get_cpu_model(), eax & 0xfU);
    return name;
}

const microcode_ops_t intel_microcode_ops = {
    .name              = "Intel",
    .builtin_name      = intel_microcode_builtin_name,
    .module_names      = intel_microcode_modules,
    .module_name_count = sizeof(intel_microcode_modules) / sizeof(intel_microcode_modules[0]),
    .match             = intel_microcode_match,
    .probe             = intel_microcode_probe,
    .load              = intel_microcode_load,
    .load_ap           = intel_microcode_load_ap,
    .revision          = intel_microcode_revision,
};
