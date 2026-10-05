/*
 *
 *      amd.c
 *      AMD processor microcode update
 *
 *      2026/10/5 MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/cpuid.h>
#include <arch/cpu/microcode/amd/amd.h>
#include <arch/misc/common.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/vsprintf.h>
#include <libs/std/string.h>
#include <libs/util/sha256.h>
#include <mem/alloc.h>
#include <mem/page.h>

static const char *const amd_microcode_modules[] = {
    "AuthenticAMD",
    "amd-ucode",
    "microcode",
};

/* Revisions that are final: a processor running one must not be updated. */
static const uint32_t amd_final_levels[] = {
    0x01000098,
    0x0100009f,
    0x010000af,
    0,
};

/* Cutoff revision per generation: updates may not mix across it. */
static const struct amd_cutoff {
        uint32_t family_id; // patch id with the revision field masked off
        uint32_t revision;  // first revision of that generation that is safe
} amd_cutoffs[] = {
    {0x80012, 0x8001277},
    {0x80082, 0x800820f},
    {0x83010, 0x830107c},
    {0x86001, 0x860010e},
    {0x86081, 0x8608108},
    {0x87010, 0x8701034},
    {0x8a000, 0x8a0000a},
    {0xa0010, 0xa00107a},
    {0xa0011, 0xa0011da},
    {0xa0012, 0xa001243},
    {0xa0082, 0xa00820e},
    {0xa1011, 0xa101153},
    {0xa1012, 0xa10124e},
    {0xa1081, 0xa108109},
    {0xa2010, 0xa20102f},
    {0xa2012, 0xa201212},
    {0xa4041, 0xa404109},
    {0xa5000, 0xa500013},
    {0xa6012, 0xa60120a},
    {0xa7041, 0xa704109},
    {0xa7052, 0xa705208},
    {0xa7080, 0xa708009},
    {0xa70c0, 0xa70c009},
    {0xaa001, 0xaa00116},
    {0xaa002, 0xaa00218},
    {0xb0021, 0xb002146},
    {0xb0081, 0xb008111},
    {0xb1010, 0xb101046},
    {0xb2040, 0xb204031},
    {0xb4040, 0xb404031},
    {0xb4041, 0xb404101},
    {0xb6000, 0xb600031},
    {0xb6080, 0xb608031},
    {0xb7000, 0xb700031},
};

/* One cached update, kept so the application processors can apply it too. */
typedef struct amd_microcode_patch {
        uint8_t *data;      // patch bytes exactly as the processor wants them
        uint32_t size;      // length of @data
        uint32_t patch_id;  // revision this patch installs
        uint16_t equiv_cpu; // equivalence id of the target processor
} amd_microcode_patch_t;

/* CPUID.1:EAX of the boot processor; the container is scanned against it. */
static uint32_t amd_bsp_cpuid_1_eax;

/* Its family and model, decoded once: every CPU matches against the boot processor's. */
static uint32_t amd_bsp_family;
static uint32_t amd_bsp_model;

/* Updates collected from the blob, one entry per distinct processor. */
static amd_microcode_patch_t *amd_patch_cache;
static size_t                 amd_patch_count;
static size_t                 amd_patch_capacity;

/* Saved equivalence table, used to match application processors. */
static struct amd_equiv_cpu_entry *amd_equiv_table;
static size_t                      amd_equiv_count;

/* CPUID.1:EAX of the calling CPU. */
static uint32_t amd_cpuid_1_eax(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid_safe(1, 0, &eax, &ebx, &ecx, &edx);
    return eax;
}

/* Rebuild the CPUID.1:EAX a Zen patch id stands for, for the matching rule. */
static uint32_t amd_rev_to_cpuid(uint32_t rev)
{
    return ((rev >> 8) & 0xfU) | (((rev >> 12) & 0xfU) << 4) | (0xfU << 8) | (((rev >> 20) & 0xfU) << 16) | (((rev >> 24) & 0xffU) << 20);
}

/* The inverse: a pseudo patch id for a processor with no revision of its own. */
static uint32_t amd_cpuid_to_rev(uint32_t eax)
{
    return ((eax & 0xfU) << 8) | (((eax >> 4) & 0xfU) << 12) | (((eax >> 16) & 0xfU) << 20) | (((eax >> 20) & 0xffU) << 24);
}

/* Revision currently installed on the calling CPU. */
static uint32_t amd_patch_level(void)
{
    return (uint32_t)rdmsr(AMD_MSR_PATCH_LEVEL);
}

/* Is the installed revision one that must never be overwritten? */
static int amd_patch_level_final(void)
{
    uint32_t level = amd_patch_level();

    for (size_t i = 0; amd_final_levels[i]; i++)
        if (level == amd_final_levels[i]) return 1;

    return 0;
}

/* Cutoff revision of the generation @rev belongs to, or 0 when unknown. */
static uint32_t amd_cutoff_revision(uint32_t rev)
{
    for (size_t i = 0; i < sizeof(amd_cutoffs) / sizeof(amd_cutoffs[0]); i++)
        if ((rev >> 8) == amd_cutoffs[i].family_id) return amd_cutoffs[i].revision;
    return 0;
}

/* Does the digest of an update have to be verified before it is applied? */
static int amd_need_sha_check(uint32_t cur_rev)
{
    uint32_t cutoff;
    if (!cur_rev) cur_rev = amd_cpuid_to_rev(amd_bsp_cpuid_1_eax);

    cutoff = amd_cutoff_revision(cur_rev);
    if (cutoff) return cur_rev <= cutoff;

    /* An unknown generation has no weak-signature window to fall back on. */
    return 1;
}

/* Do the updates of this processor carry a digest we can check? */
static int amd_cpu_has_entrysign(void)
{
    uint32_t family = amd_bsp_family;
    uint32_t model  = amd_bsp_model;

    if (family == 0x17 || family == 0x19) return 1;
    if (family == 0x1a) return model <= 0x2f || (model >= 0x40 && model <= 0x4f) || (model >= 0x60 && model <= 0x7f);

    return 0;
}

/* Digest known for @patch_id, or NULL when there is none. */
static const amd_patch_digest_t *amd_find_digest(uint32_t patch_id)
{
    for (size_t i = 0; i < amd_patch_digest_count; i++)
        if (amd_patch_digests[i].patch_id == patch_id) return &amd_patch_digests[i];
    return NULL;
}

/* Check the SHA-256 digest of a patch; Zen and revisions past the cutoff need it. */
static int amd_verify_sha256_digest(uint32_t patch_id, uint32_t cur_rev, const uint8_t *data, size_t size)
{
    const amd_patch_digest_t *digest;
    uint8_t                   hash[SHA256_DIGEST_SIZE];

    if (!amd_cpu_has_entrysign()) return 1;
    if (!amd_need_sha_check(cur_rev)) return 1;

    digest = amd_find_digest(patch_id);
    if (!digest) return 0;

    sha256(data, size, hash);
    return memcmp(hash, digest->sha256, SHA256_DIGEST_SIZE) == 0;
}

/* Is there a valid container header at the start of the buffer? */
static int amd_verify_container(const uint8_t *buf, size_t size)
{
    if (size <= AMD_MC_CONTAINER_HDR_SIZE) return 0;
    return *(const uint32_t *)buf == AMD_MC_MAGIC;
}

/* Is there a valid, non-truncated equivalence table at the start of @buf? */
static int amd_verify_equivalence_table(const uint8_t *buf, size_t size)
{
    const uint32_t *hdr = (const uint32_t *)buf;
    uint32_t        length;

    if (!amd_verify_container(buf, size)) return 0;

    /* Zen and newer do not need an equivalence table. */
    if (amd_bsp_family >= 0x17) return 1;
    if (hdr[1] != AMD_MC_EQUIV_CPU_TABLE_TYPE) return 0;

    size -= AMD_MC_CONTAINER_HDR_SIZE;
    length = hdr[2];

    if (length < sizeof(struct amd_equiv_cpu_entry) || size < length) return 0;
    return 1;
}

/* Validate a section header and report the payload size it claims. */
static int amd_verify_patch_section(const uint8_t *buf, size_t size, uint32_t *section_size)
{
    const uint32_t *hdr = (const uint32_t *)buf;
    uint32_t        type, length;

    if (size < AMD_MC_SECTION_HDR_SIZE) return 0;
    type   = hdr[0];
    length = hdr[1];

    if (type != AMD_MC_MICROCODE_TYPE) return 0;
    if (length < sizeof(struct amd_microcode_header)) return 0;

    *section_size = length;
    return 1;
}

/* Does a payload fit the family specific limits and the buffer it sits in? */
static int amd_verify_patch_size(uint32_t size, size_t available)
{
    uint32_t family = amd_bsp_family;
    uint32_t max_size;

    if (family < 0x15) {
        switch (family) {
            case 0x10 :
            case 0x11 :
            case 0x12 :
                max_size = AMD_MC_F1XH_MPB_MAX_SIZE;
                break;
            case 0x14 :
                max_size = AMD_MC_F14H_MPB_MAX_SIZE;
                break;
            default :
                return 0;
        }
        if (size > max_size) return 0;
    }
    return size <= available;
}

/* Verify one patch section: 0 = for this family, 1 = another family, -1 = broken. */
static int amd_verify_patch(const uint8_t *buf, size_t size, uint32_t *patch_size)
{
    const struct amd_microcode_header *hdr;
    uint32_t                           family = amd_bsp_family;
    uint32_t                           cur_rev, cutoff, patch_rev, section_size;
    uint32_t                           patch_family;
    uint16_t                           proc_id;

    if (!amd_verify_patch_section(buf, size, &section_size)) return -1;

    /* The section header itself is not counted in the size it declares. */
    size -= AMD_MC_SECTION_HDR_SIZE;
    if (size < section_size) return -1;
    if (!amd_verify_patch_size(section_size, size)) return -1;

    *patch_size = section_size;
    hdr         = (const struct amd_microcode_header *)(buf + AMD_MC_SECTION_HDR_SIZE);

    /* Chipset specific code is not something this loader supports. */
    if (hdr->nb_dev_id || hdr->sb_dev_id) return -1;
    proc_id      = hdr->processor_rev_id;
    patch_family = 0xfU + (proc_id >> 12);

    if (patch_family != family) return 1;
    cur_rev = amd_patch_level();
    cutoff  = amd_cutoff_revision(cur_rev);

    if (!cutoff) return 0;
    patch_rev = hdr->patch_id;

    /* Either both revisions are protected by the new scheme or neither is. */
    if (cur_rev <= cutoff && patch_rev <= cutoff) return 0;
    if (cur_rev > cutoff && patch_rev > cutoff) return 0;

    return 1;
}

/* Equivalence id of @sig in @table, or zero when it is not listed. */
static uint16_t amd_lookup_equiv_id(const struct amd_equiv_cpu_entry *table, size_t count, uint32_t sig)
{
    if (amd_bsp_family >= 0x17) return 0;
    for (size_t i = 0; i < count; i++)
        if (table[i].installed_cpu == sig) return table[i].equiv_cpu;

    return 0;
}

/* Does @patch run on the processor identified by @sig? */
static int amd_patch_matches(const amd_microcode_patch_t *patch, uint32_t sig, uint16_t equiv_id)
{
    /* Zen and newer hardcode family, model and stepping in the patch id. */
    if (amd_bsp_family >= 0x17) return amd_rev_to_cpuid(patch->patch_id) == sig;
    return equiv_id && equiv_id == patch->equiv_cpu;
}

/* Do two cached updates target the same processor? */
static int amd_patch_equivalent(const amd_microcode_patch_t *a, const amd_microcode_patch_t *b, int ignore_stepping)
{
    uint32_t a_cpuid, b_cpuid;

    if (amd_bsp_family < 0x17) return a->equiv_cpu == b->equiv_cpu;
    a_cpuid = amd_rev_to_cpuid(a->patch_id);
    b_cpuid = amd_rev_to_cpuid(b->patch_id);

    if (ignore_stepping) {
        a_cpuid &= ~0xfU;
        b_cpuid &= ~0xfU;
    }

    return a_cpuid == b_cpuid;
}

/* Is @b a newer revision than @a?  Negative means "a different processor". */
static int amd_patch_newer(const amd_microcode_patch_t *a, const amd_microcode_patch_t *b)
{
    if (amd_bsp_family < 0x17) return b->patch_id > a->patch_id;

    /* A different stepping is a different processor, never a replacement. */
    if (((a->patch_id >> 8) & 0xfU) != ((b->patch_id >> 8) & 0xfU)) return -1;
    return (b->patch_id & 0xffU) > (a->patch_id & 0xffU);
}

/* Add one verified update, replacing an older one for the same processor; takes ownership of @data. */
static int amd_cache_patch(uint8_t *data, uint32_t size, uint32_t patch_id, uint16_t equiv_cpu)
{
    amd_microcode_patch_t candidate = {.data = data, .size = size, .patch_id = patch_id, .equiv_cpu = equiv_cpu};

    for (size_t i = 0; i < amd_patch_count; i++) {
        int newer;

        if (!amd_patch_equivalent(&amd_patch_cache[i], &candidate, 1)) continue;

        newer = amd_patch_newer(&amd_patch_cache[i], &candidate);

        /* A different stepping is a different processor: both stay cached. */
        if (newer < 0) continue;
        if (newer == 0) {
            free(data);
            return 0;
        }

        free(amd_patch_cache[i].data);
        amd_patch_cache[i] = candidate;
        return 0;
    }
    if (amd_patch_count == amd_patch_capacity) {
        size_t                 capacity = amd_patch_capacity ? amd_patch_capacity * 2 : 16;
        amd_microcode_patch_t *grown    = realloc(amd_patch_cache, capacity * sizeof(*grown));

        if (!grown) {
            free(data);
            return -ENOMEM;
        }

        amd_patch_cache    = grown;
        amd_patch_capacity = capacity;
    }

    amd_patch_cache[amd_patch_count++] = candidate;
    return 0;
}

/* Copy one verified section into the cache. */
static int amd_cache_section(const struct amd_microcode_header *hdr, uint32_t size)
{
    uint8_t *copy = malloc(size);
    if (!copy) return -ENOMEM;

    memcpy(copy, hdr, size);
    return amd_cache_patch(copy, size, hdr->patch_id, hdr->processor_rev_id);
}

/* Keep a private copy of the container's equivalence table. */
static int amd_install_equiv_table(const struct amd_equiv_cpu_entry *table, uint32_t size)
{
    if (amd_bsp_family >= 0x17) return 0;
    free(amd_equiv_table);
    amd_equiv_table = malloc(size);

    if (!amd_equiv_table) {
        amd_equiv_count = 0;
        return -ENOMEM;
    }

    memcpy(amd_equiv_table, table, size);
    amd_equiv_count = size / sizeof(*table);
    return 0;
}

/* Cache one container's updates; @consumed says how far the caller may advance. */
static int amd_scan_container(const uint8_t *data, size_t size, size_t *consumed, int *matched)
{
    const struct amd_equiv_cpu_entry *table;
    const uint8_t                    *cursor;
    size_t                            left;
    uint32_t                          table_size;
    uint16_t                          equiv_id;

    *consumed = 0;
    *matched  = 0;

    if (!amd_verify_equivalence_table(data, size)) return 0;
    table_size = ((const uint32_t *)data)[2];

    /* A container claiming a longer table than it holds is corrupt. */
    if (table_size > size - AMD_MC_CONTAINER_HDR_SIZE) return 0;
    table    = (const void *)(data + AMD_MC_CONTAINER_HDR_SIZE);
    equiv_id = amd_lookup_equiv_id(table, table_size / sizeof(*table), amd_bsp_cpuid_1_eax);

    cursor = data + AMD_MC_CONTAINER_HDR_SIZE + table_size;
    left   = size - AMD_MC_CONTAINER_HDR_SIZE - table_size;

    while (left) {
        const struct amd_microcode_header *hdr;
        uint32_t                           size_here = 0;
        int                                ret       = amd_verify_patch(cursor, left, &size_here);

        if (ret < 0) break;
        if (ret == 0) {
            int err;

            hdr                             = (const void *)(cursor + AMD_MC_SECTION_HDR_SIZE);
            amd_microcode_patch_t candidate = {.patch_id = hdr->patch_id, .equiv_cpu = hdr->processor_rev_id};

            err = amd_cache_section(hdr, size_here);
            if (err < 0) return err;
            if (amd_patch_matches(&candidate, amd_bsp_cpuid_1_eax, equiv_id)) *matched = 1;
        }

        cursor += size_here + AMD_MC_SECTION_HDR_SIZE;
        left -= size_here + AMD_MC_SECTION_HDR_SIZE;
    }

    *consumed = size - left;

    if (*matched) {
        int err = amd_install_equiv_table(table, table_size);
        if (err < 0) return err;
    }

    return 0;
}

/* Newest cached update matching @sig, or NULL; @sig is compared directly. */
static const amd_microcode_patch_t *amd_find_patch(uint32_t sig)
{
    uint16_t equiv_id = amd_lookup_equiv_id(amd_equiv_table, amd_equiv_count, sig);

    /* Family 10h through 16h are only reachable through the table. */
    if (amd_bsp_family < 0x17 && !equiv_id) return NULL;
    for (size_t i = 0; i < amd_patch_count; i++)
        if (amd_patch_matches(&amd_patch_cache[i], sig, equiv_id)) return &amd_patch_cache[i];

    return NULL;
}

/* Load one cached update into the calling CPU. */
static int amd_apply_patch(const amd_microcode_patch_t *patch, uint32_t cur_rev)
{
    uint64_t address = (uint64_t)(uintptr_t)patch->data;

    /* -EINVAL means the digest did not vouch for this update. */
    if (!amd_verify_sha256_digest(patch->patch_id, cur_rev, patch->data, patch->size)) return -EINVAL;
    wrmsr(AMD_MSR_PATCH_LOADER, address);

    /* Family 17h caches the update, so drop the lines covering it. */
    if (amd_bsp_family == 0x17) {
        flush_tlb(address);
        if (address / PAGE_4K_SIZE != (address + patch->size - 1) / PAGE_4K_SIZE) flush_tlb(address + patch->size - 1);
    }
    if (amd_patch_level() != patch->patch_id) return -EIO;

    return 0;
}

/* Walk the blob's containers until one covers the boot processor. */
static int amd_load_cache(const microcode_blob_t *blob)
{
    const uint8_t *cursor = blob->data;
    size_t         left   = blob->size;

    while (left) {
        size_t consumed = 0;
        int    matched  = 0;
        int    err      = amd_scan_container(cursor, left, &consumed, &matched);

        if (err < 0) return err;
        if (matched) return 0;

        /* A container that cannot be measured would loop forever. */
        if (!consumed) break;

        cursor += consumed;
        left -= consumed;
    }

    return 0;
}

/* Cache @blob's updates and apply the boot processor's one. */
static microcode_state_t amd_microcode_load(const microcode_blob_t *blob)
{
    const amd_microcode_patch_t *patch;
    uint32_t                     before = amd_patch_level();
    int                          err;

    amd_bsp_cpuid_1_eax = amd_cpuid_1_eax();
    amd_bsp_family      = get_cpu_family();
    amd_bsp_model       = get_cpu_model();
    if (amd_patch_level_final()) {
        plogk("microcode: AMD revision 0x%08x is final and is not updated.\n", before);
        return MICROCODE_OK;
    }

    err = amd_load_cache(blob);
    if (err < 0) {
        plogk("microcode: out of memory while caching the AMD updates.\n");
        return MICROCODE_ERROR;
    }

    patch = amd_find_patch(amd_bsp_cpuid_1_eax);
    if (!patch) {
        plogk("microcode: no AMD update for CPU0 (CPUID 0x%08x), revision 0x%08x\n", amd_bsp_cpuid_1_eax, before);
        return MICROCODE_NFOUND;
    }

    /* An equal revision is applied too: it may carry SMT-specific changes. */
    if (before > patch->patch_id) {
        plogk("microcode: AMD CPU0 revision 0x%08x is already current.\n", before);
        return MICROCODE_OK;
    }

    err = amd_apply_patch(patch, before);
    if (err == 0) {
        plogk("microcode: AMD CPU0 revision 0x%08x -> 0x%08x\n", before, patch->patch_id);
        return MICROCODE_UPDATED;
    }

    if (err == -EINVAL)
        plogk("microcode: AMD patch 0x%08x was rejected by the SHA-256 check.\n", patch->patch_id);
    else
        plogk("microcode: AMD patch 0x%08x did not take effect.\n", patch->patch_id);

    return MICROCODE_ERROR;
}

/* Apply this AP's update out of the cache. */
static microcode_state_t amd_microcode_load_ap(void)
{
    const amd_microcode_patch_t *patch;
    uint32_t                     sig = amd_cpuid_1_eax();
    uint32_t                     rev = amd_patch_level();
    int                          err;

    patch = amd_find_patch(sig);
    if (!patch) return MICROCODE_NFOUND;

    /* An AP may run a different stepping, which the cache already covers. */
    if (rev > patch->patch_id) return MICROCODE_OK;

    err = amd_apply_patch(patch, rev);
    if (err < 0) {
        /* APs share this line, so report a failure once. */
        if (err == -EINVAL)
            plogk_once("microcode: an AMD application processor failed the SHA-256 check.\n");
        else
            plogk_once("microcode: an AMD application processor could not be updated.\n");
        return MICROCODE_ERROR;
    }

    return MICROCODE_UPDATED;
}

/* Dispatcher hook: is the running processor a supported AMD one? */
static int amd_microcode_match(void)
{
    if (strcmp(get_vendor_name(), "AuthenticAMD") != 0) return 0;

    /* Family 10h is the first one that uses the container format below. */
    return get_cpu_family() >= 0x10;
}

/* Name of this family's update, "microcode_amd_fam17h"; earlier families share the legacy file. */
static const char *amd_microcode_builtin_name(void)
{
    static char name[32];
    uint32_t    family = get_cpu_family();

    if (family >= 0x15)
        snprintf(name, sizeof(name), "microcode_amd_fam%02xh", family);
    else
        snprintf(name, sizeof(name), "microcode_amd");

    return name;
}

const microcode_ops_t amd_microcode_ops = {
    .name              = "AMD",
    .builtin_name      = amd_microcode_builtin_name,
    .module_names      = amd_microcode_modules,
    .module_name_count = sizeof(amd_microcode_modules) / sizeof(amd_microcode_modules[0]),
    .match             = amd_microcode_match,
    .probe             = amd_verify_container,
    .load              = amd_microcode_load,
    .load_ap           = amd_microcode_load_ap,
    .revision          = amd_patch_level,
};
