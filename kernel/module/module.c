/*
 *
 *      module.c
 *      Loadable kernel module registry, linker and lifecycle
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#define UINXED_MODULE_CORE
#include <boot/limine.h>
#include <fs/sysfs/module_sysfs.h>
#include <kernel/errno.h>
#include <kernel/module/elf.h>
#include <kernel/module/module.h>
#include <kernel/module/module_elf.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <libs/kobject/kobject.h>
#include <libs/list/circular_list.h>
#include <libs/std/math.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/page.h>
#include <mem/page_walker.h>
#include <process/task.h>
#include <sync/spin_lock.h>

#ifndef CONFIG_MODULES
#    define CONFIG_MODULES 1
#endif
#ifndef CONFIG_MODULE_FORCE_LOAD
#    define CONFIG_MODULE_FORCE_LOAD 0
#endif
#ifndef CONFIG_MODULE_FORCE_UNLOAD
#    define CONFIG_MODULE_FORCE_UNLOAD 0
#endif
#ifndef CONFIG_MODULE_SIG_FORCE
#    define CONFIG_MODULE_SIG_FORCE 0
#endif
#ifndef CONFIG_MODULE_MAX_SIZE
#    define CONFIG_MODULE_MAX_SIZE 64
#endif

#define MODULE_MAX_SIZE         ((size_t)CONFIG_MODULE_MAX_SIZE * 1024U * 1024U)
#define MODULE_VADDR_OFFSET     0x20000000ULL
#define MODULE_VADDR_LIMIT      0x78000000ULL
#define MODULE_MAX_DEPENDENCIES 256

typedef struct module_dependency {
        struct module            *owner;
        struct module_dependency *next;
} module_dependency_t;

typedef struct module_string_allocation {
        char                            *value;
        struct module_string_allocation *next;
} module_string_allocation_t;

typedef struct module_section_layout {
        uintptr_t address;
        size_t    size;
        uint64_t  flags;
        bool      alloc;
        bool      init;
        bool      ro_after_init;
        char     *name;
} module_section_layout_t;

typedef struct module_internal {
        struct module              *module;
        struct module_internal     *next;
        uintptr_t                   base;
        size_t                      mapped_size;
        uint64_t                   *frames;
        size_t                      page_count;
        module_section_layout_t    *sections;
        size_t                      section_count;
        module_dependency_t        *dependencies;
        module_string_allocation_t *parameter_strings;
        const struct kernel_param  *parameters;
        size_t                      parameter_count;
        const struct kernel_symbol *exports;
        size_t                      export_count;
        int (*init)(void);
        void (*exit)(void);
        char           *license;
        char           *version;
        char           *srcversion;
        char           *imports;
        module_sysfs_t *sysfs;
        wait_queue_t    unload_wait;
} module_internal_t;

extern const struct kernel_symbol __start___ksymtab[];
extern const struct kernel_symbol __stop___ksymtab[];

/*
 * Globals
 * module_list is the singly-linked list of loaded modules. All
 * mutations happen under module_lock; module_operation serializes
 * load/unload against parameter access.
 */

static module_internal_t          *module_list;
static spinlock_t                  module_lock;
static volatile uint32_t           module_operation;
static module_signature_verifier_t signature_verifier;

/* Overflow-checked helpers used throughout module metadata parsing */

static int size_add(size_t left, size_t right, size_t *result)
{
    if (right > SIZE_MAX - left) return -EOVERFLOW;
    *result = left + right;
    return EOK;
}

static int align_size(size_t value, size_t alignment, size_t *result)
{
    if (!alignment) alignment = 1;
    if (alignment & (alignment - 1)) return -ENOEXEC;
    if (value > SIZE_MAX - (alignment - 1)) return -EOVERFLOW;
    *result = (value + alignment - 1) & ~(alignment - 1);
    return EOK;
}

static int image_range_valid(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

static int string_bounded(const char *string, size_t available)
{
    if (!string) return 0;
    for (size_t index = 0; index < available; index++)
        if (!string[index]) return 1;
    return 0;
}

static char *duplicate_range(const char *value, size_t length)
{
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length);
    copy[length] = 0;
    return copy;
}

static const char *section_name(const module_elf_view_t *view, size_t index)
{
    if (!view || index >= view->section_count || view->section_name_index == SHN_UNDEF) return NULL;
    const Elf64_Shdr *strings = &view->sections[view->section_name_index];
    size_t            offset  = view->sections[index].sh_name;
    if (offset >= strings->sh_size) return NULL;
    const char *name = (const char *)view->image + strings->sh_offset + offset;
    return string_bounded(name, strings->sh_size - offset) ? name : NULL;
}

static const Elf64_Shdr *find_section(const module_elf_view_t *view, const char *wanted, size_t *index_out)
{
    for (size_t index = 0; index < view->section_count; index++) {
        const char *name = section_name(view, index);
        if (name && streq(name, wanted)) {
            if (index_out) *index_out = index;
            return &view->sections[index];
        }
    }
    return NULL;
}

static const char *modinfo_find(const module_elf_view_t *view, const char *key, size_t occurrence, size_t *length_out)
{
    const Elf64_Shdr *section = find_section(view, ".modinfo", NULL);
    if (!section || !section->sh_size) return NULL;
    const char *data     = (const char *)view->image + section->sh_offset;
    size_t      key_size = strlen(key);
    size_t      offset   = 0;
    size_t      found    = 0;

    while (offset < section->sh_size) {
        size_t available = section->sh_size - offset;
        if (!string_bounded(data + offset, available)) return NULL;
        size_t length = strlen(data + offset);
        if (length > key_size && data[offset + key_size] == '=' && !strncmp(data + offset, key, key_size)) {
            if (found++ == occurrence) {
                if (length_out) *length_out = length - key_size - 1;
                return data + offset + key_size + 1;
            }
        }
        offset += length + 1;
    }
    return NULL;
}

static char *copy_modinfo(const module_elf_view_t *view, const char *key)
{
    size_t      length = 0;
    const char *value  = modinfo_find(view, key, 0, &length);
    return value ? duplicate_range(value, length) : NULL;
}

static int module_name_valid(const char *name)
{
    if (!name || !name[0]) return 0;
    size_t length = 0;
    for (; name[length]; length++) {
        char c = name[length];
        if (length >= MODULE_NAME_LEN - 1 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return length != 0;
}

const char *module_state_name(enum module_state state)
{
    switch (state) {
        case MODULE_STATE_COMING :
            return "coming";
        case MODULE_STATE_LIVE :
            return "live";
        case MODULE_STATE_GOING :
            return "going";
        default :
            return "unformed";
    }
}

/* Return the module's version string, or "" when absent. */
const char *module_version(const struct module *module)
{
    if (!module) return "";
    module_internal_t *internal = __atomic_load_n((module_internal_t **)&module->loader_private, __ATOMIC_ACQUIRE);
    return internal && internal->version ? internal->version : "";
}

/* Return the module's srcversion string, or "" when absent. */
const char *module_srcversion(const struct module *module)
{
    if (!module) return "";
    module_internal_t *internal = __atomic_load_n((module_internal_t **)&module->loader_private, __ATOMIC_ACQUIRE);
    return internal && internal->srcversion ? internal->srcversion : "";
}

/* Return the number of allocatable sections in a module. */
size_t module_section_count(const struct module *module)
{
    module_internal_t *internal = __atomic_load_n((module_internal_t **)&module->loader_private, __ATOMIC_ACQUIRE);
    return internal ? internal->section_count : 0;
}

/* Return the name of the index-th section, or NULL when out of range. */
const char *module_section_name(const struct module *module, size_t index)
{
    module_internal_t *internal = __atomic_load_n((module_internal_t **)&module->loader_private, __ATOMIC_ACQUIRE);
    if (!internal || index >= internal->section_count || !internal->sections[index].alloc) return NULL;
    return internal->sections[index].name;
}

/* Return the load address of the index-th section, or 0 when out of range. */
uintptr_t module_section_address(const struct module *module, size_t index)
{
    module_internal_t *internal = __atomic_load_n((module_internal_t **)&module->loader_private, __ATOMIC_ACQUIRE);
    if (!internal || index >= internal->section_count) return 0;
    return internal->sections[index].address;
}

/* Return the number of module parameters. */
size_t module_param_count(const struct module *module)
{
    module_internal_t *internal = __atomic_load_n((module_internal_t **)&module->loader_private, __ATOMIC_ACQUIRE);
    return internal ? internal->parameter_count : 0;
}

/* Return the name of the index-th parameter, or NULL when out of range. */
const char *module_param_name(const struct module *module, size_t index)
{
    module_internal_t *internal = __atomic_load_n((module_internal_t **)&module->loader_private, __ATOMIC_ACQUIRE);
    if (!internal || index >= internal->parameter_count) return NULL;
    return internal->parameters[index].name;
}

/* Format the current value of the index-th parameter into buf. */
int module_param_value(const struct module *module, size_t index, char *buf, size_t size)
{
    module_internal_t *internal = __atomic_load_n((module_internal_t **)&module->loader_private, __ATOMIC_ACQUIRE);
    if (!internal || index >= internal->parameter_count || !buf || !size) return -EINVAL;
    const struct kernel_param *param = &internal->parameters[index];
    switch (param->type) {
        case MODULE_PARAM_BYTE :
            return snprintf(buf, size, "%hhd", *(const signed char *)param->arg);
        case MODULE_PARAM_SHORT :
            return snprintf(buf, size, "%hd", *(const short *)param->arg);
        case MODULE_PARAM_USHORT :
            return snprintf(buf, size, "%hu", *(const unsigned short *)param->arg);
        case MODULE_PARAM_INT :
            return snprintf(buf, size, "%d", *(const int *)param->arg);
        case MODULE_PARAM_UINT :
            return snprintf(buf, size, "%u", *(const unsigned int *)param->arg);
        case MODULE_PARAM_LONG :
            return snprintf(buf, size, "%ld", *(const long *)param->arg);
        case MODULE_PARAM_ULONG :
            return snprintf(buf, size, "%lu", *(const unsigned long *)param->arg);
        case MODULE_PARAM_BOOL :
            return snprintf(buf, size, "%c", *(const bool *)param->arg ? 'Y' : 'N');
        case MODULE_PARAM_CHARP :
            return snprintf(buf, size, "%s", *(const char *const *)param->arg ? *(const char *const *)param->arg : "");
        default :
            return -EINVAL;
    }
}

/* Return the number of modules that hold (depend on) this module. */
size_t module_holder_count(const struct module *module)
{
    if (!module) return 0;
    size_t   count = 0;
    uint64_t irq   = spin_lock_irqsave(&module_lock);
    for (module_internal_t *consumer = module_list; consumer; consumer = consumer->next)
        for (module_dependency_t *dep = consumer->dependencies; dep; dep = dep->next)
            if (dep->owner == module) {
                count++;
                break;
            }
    spin_unlock_irqrestore(&module_lock, irq);
    return count;
}

/* Return the index-th module that holds (depends on) this module. */
struct module *module_holder(const struct module *module, size_t index)
{
    if (!module) return NULL;
    size_t   seen = 0;
    uint64_t irq  = spin_lock_irqsave(&module_lock);
    for (module_internal_t *consumer = module_list; consumer; consumer = consumer->next) {
        for (module_dependency_t *dep = consumer->dependencies; dep; dep = dep->next) {
            if (dep->owner == module) {
                if (seen == index) {
                    struct module *holder = consumer->module;
                    spin_unlock_irqrestore(&module_lock, irq);
                    return holder;
                }
                seen++;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&module_lock, irq);
    return NULL;
}

/* Return the sysfs kobject of a module, or NULL. */
struct kobject *module_sysfs_object(const struct module *module)
{
    module_internal_t *internal = __atomic_load_n((module_internal_t **)&module->loader_private, __ATOMIC_ACQUIRE);
    return internal ? module_sysfs_kobj(internal->sysfs) : NULL;
}

static int license_gpl_compatible(const char *license)
{
    static const char *compatible[] = {
        "GPL", "GPL v2", "GPL and additional rights", "Dual BSD/GPL", "Dual MIT/GPL", "Dual MPL/GPL", NULL,
    };
    if (!license) return 0;
    for (size_t index = 0; compatible[index]; index++)
        if (streq(license, compatible[index])) return 1;
    return 0;
}

static module_internal_t *module_find_locked(const char *name)
{
    for (module_internal_t *item = module_list; item; item = item->next)
        if (streq(item->module->name, name)) return item;
    return NULL;
}

static int operation_begin(void)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&module_operation, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? EOK : -EBUSY;
}

static void operation_end(void)
{
    __atomic_store_n(&module_operation, 0, __ATOMIC_RELEASE);
}

/*
 * Reference counting
 * Refcounts are atomic; module_put wakes the unload wait queue when
 * the count drops to zero so an unloader can proceed safely.
 */

uint32_t module_refcount(const struct module *module)
{
    return module ? __atomic_load_n(&module->refcount, __ATOMIC_ACQUIRE) : 0;
}

/* Try to bump the refcount; fails if the module is being unloaded */
int try_module_get(struct module *module)
{
    if (!module || __atomic_load_n(&module->state, __ATOMIC_ACQUIRE) != MODULE_STATE_LIVE) return 0;
    uint32_t count = module_refcount(module);
    while (count != UINT32_MAX) {
        if (__atomic_compare_exchange_n(&module->refcount, &count, count + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (__atomic_load_n(&module->state, __ATOMIC_ACQUIRE) == MODULE_STATE_LIVE) return 1;
            module_put(module);
            return 0;
        }
    }
    return 0;
}

void __module_get(struct module *module)
{
    if (!module) return;
    uint32_t old = __atomic_fetch_add(&module->refcount, 1, __ATOMIC_ACQ_REL);
    if (old == UINT32_MAX) __atomic_store_n(&module->refcount, UINT32_MAX, __ATOMIC_RELEASE);
}

/* Release a reference; the last put wakes any pending unload */
void module_put(struct module *module)
{
    if (!module) return;
    uint32_t count = module_refcount(module);
    while (count) {
        if (__atomic_compare_exchange_n(&module->refcount, &count, count - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (count == 1) {
                module_internal_t *internal = __atomic_load_n((module_internal_t **)&module->loader_private, __ATOMIC_ACQUIRE);
                if (internal) wait_queue_wake_all(&internal->unload_wait);
            }
            return;
        }
    }
}

struct module *module_find_get(const char *name)
{
    if (!name) return NULL;
    struct module     *result = NULL;
    uint64_t           irq    = spin_lock_irqsave(&module_lock);
    module_internal_t *item   = module_find_locked(name);
    if (item && try_module_get(item->module)) result = item->module;
    spin_unlock_irqrestore(&module_lock, irq);
    return result;
}

static int address_in_module(const module_internal_t *internal, uintptr_t address, size_t size)
{
    if (!internal || address < internal->base || size > internal->mapped_size) return 0;
    return address - internal->base <= internal->mapped_size - size;
}

static int module_imports_namespace(const module_internal_t *internal, const char *namespace_name)
{
    if (!namespace_name || !namespace_name[0]) return 1;
    if (!internal->imports) return 0;
    const char *cursor = internal->imports;
    size_t      wanted = strlen(namespace_name);
    while (*cursor) {
        const char *end = strchr(cursor, ',');
        size_t      len = end ? (size_t)(end - cursor) : strlen(cursor);
        if (len == wanted && !strncmp(cursor, namespace_name, len)) return 1;
        if (!end) break;
        cursor = end + 1;
    }
    return 0;
}

static int dependency_add(module_internal_t *consumer, struct module *owner)
{
    if (!owner || owner == consumer->module) return EOK;
    size_t count = 0;
    for (module_dependency_t *dep = consumer->dependencies; dep; dep = dep->next) {
        if (dep->owner == owner) return EOK;
        if (++count >= MODULE_MAX_DEPENDENCIES) return -E2BIG;
    }
    if (!try_module_get(owner)) return -EBUSY;
    module_dependency_t *dependency = calloc(1, sizeof(*dependency));
    if (!dependency) {
        module_put(owner);
        return -ENOMEM;
    }
    dependency->owner      = owner;
    dependency->next       = consumer->dependencies;
    consumer->dependencies = dependency;
    return EOK;
}

static int symbol_usable(module_internal_t *consumer, const struct kernel_symbol *symbol)
{
    if (!symbol || !symbol->name || !symbol->value) return 0;
    if ((symbol->flags & KERNEL_SYMBOL_GPL_ONLY) && !license_gpl_compatible(consumer->license)) return 0;
    if (!module_imports_namespace(consumer, symbol->namespace_name)) return 0;
    return 1;
}

static int resolve_export(module_internal_t *consumer, const char *name, uint64_t *value, struct module **owner_out)
{
    for (const struct kernel_symbol *symbol = __start___ksymtab; symbol < __stop___ksymtab; symbol++) {
        if (symbol->name && streq(symbol->name, name)) {
            if (!symbol_usable(consumer, symbol)) return -EPERM;
            *value = symbol->value;
            if (owner_out) *owner_out = NULL;
            return EOK;
        }
    }

    uint64_t irq = spin_lock_irqsave(&module_lock);
    for (module_internal_t *item = module_list; item; item = item->next) {
        if (item == consumer || item->module->state != MODULE_STATE_LIVE) continue;
        for (size_t index = 0; index < item->export_count; index++) {
            const struct kernel_symbol *symbol = &item->exports[index];
            if (symbol->name && streq(symbol->name, name)) {
                if (!symbol_usable(consumer, symbol)) {
                    spin_unlock_irqrestore(&module_lock, irq);
                    return -EPERM;
                }
                int ret = dependency_add(consumer, item->module);
                if (ret == EOK) {
                    *value = symbol->value;
                    if (owner_out) *owner_out = item->module;
                }
                spin_unlock_irqrestore(&module_lock, irq);
                return ret;
            }
        }
    }
    spin_unlock_irqrestore(&module_lock, irq);
    return -ENOENT;
}

void *module_symbol_get(const char *name, struct module **owner)
{
    if (!name) return NULL;
    if (owner) *owner = NULL;
    for (const struct kernel_symbol *symbol = __start___ksymtab; symbol < __stop___ksymtab; symbol++)
        if (symbol->name && streq(symbol->name, name)) return (void *)symbol->value;

    uint64_t irq = spin_lock_irqsave(&module_lock);
    for (module_internal_t *item = module_list; item; item = item->next) {
        if (item->module->state != MODULE_STATE_LIVE) continue;
        for (size_t index = 0; index < item->export_count; index++) {
            if (streq(item->exports[index].name, name) && try_module_get(item->module)) {
                void *value = (void *)item->exports[index].value;
                if (owner) *owner = item->module;
                spin_unlock_irqrestore(&module_lock, irq);
                return value;
            }
        }
    }
    spin_unlock_irqrestore(&module_lock, irq);
    return NULL;
}

void module_symbol_put(struct module *owner)
{
    module_put(owner);
}

static int append_import(module_internal_t *internal, const char *value, size_t length)
{
    size_t old_length = internal->imports ? strlen(internal->imports) : 0;
    size_t new_length = old_length;
    if (size_add(new_length, length, &new_length) != EOK || (old_length && size_add(new_length, 1, &new_length) != EOK)) return -EOVERFLOW;
    char *imports = realloc(internal->imports, new_length + 1);
    if (!imports) return -ENOMEM;
    if (old_length) imports[old_length++] = ',';
    memcpy(imports + old_length, value, length);
    imports[old_length + length] = 0;
    internal->imports            = imports;
    return EOK;
}

static int prepare_metadata(module_internal_t *internal, const module_elf_view_t *view, unsigned int flags, const char *name_hint)
{
    size_t      length     = 0;
    const char *name       = modinfo_find(view, "name", 0, &length);
    char       *owned_name = NULL;

    if (!name && name_hint) {
        const char *base = strrchr(name_hint, '/');
        base             = base ? base + 1 : name_hint;
        length           = strlen(base);
        if (length > 3 && streq(base + length - 3, ".ko")) length -= 3;
        owned_name = duplicate_range(base, length);
        name       = owned_name;
    }
    if (!name || length >= MODULE_NAME_LEN) {
        free(owned_name);
        return -ENOEXEC;
    }
    memcpy(internal->module->name, name, length);
    internal->module->name[length] = 0;
    free(owned_name);
    if (!module_name_valid(internal->module->name)) return -EINVAL;

    internal->license    = copy_modinfo(view, "license");
    internal->version    = copy_modinfo(view, "version");
    internal->srcversion = copy_modinfo(view, "srcversion");
    if (!license_gpl_compatible(internal->license)) internal->module->taints |= MODULE_TAINT_PROPRIETARY;

    const char *intree = modinfo_find(view, "intree", 0, NULL);
    if (!intree || !streq(intree, "Y")) internal->module->taints |= MODULE_TAINT_OUT_OF_TREE;

    for (size_t occurrence = 0;; occurrence++) {
        const char *import = modinfo_find(view, "import_ns", occurrence, &length);
        if (!import) break;
        int ret = append_import(internal, import, length);
        if (ret != EOK) return ret;
    }

    const char *vermagic = modinfo_find(view, "vermagic", 0, NULL);
    if (!vermagic || !streq(vermagic, KERNEL_VERSION)) {
        if (!(flags & MODULE_INIT_IGNORE_VERMAGIC)) return -ENOEXEC;
        if (!CONFIG_MODULE_FORCE_LOAD) return -EPERM;
        internal->module->taints |= MODULE_TAINT_FORCED;
    }

    const Elf64_Shdr *signature = find_section(view, ".module_sig", NULL);
    if (signature && signature->sh_size) {
        if (!signature_verifier) {
            if (CONFIG_MODULE_SIG_FORCE) return -ENOKEY;
            internal->module->taints |= MODULE_TAINT_UNSIGNED;
        } else {
            int ret = signature_verifier(view->image, view->size, view->image + signature->sh_offset, signature->sh_size);
            if (ret != EOK) return ret < 0 ? ret : -EKEYREJECTED;
        }
    } else {
        if (CONFIG_MODULE_SIG_FORCE) return -ENOKEY;
        internal->module->taints |= MODULE_TAINT_UNSIGNED;
    }
    return EOK;
}

static int load_declared_dependencies(module_internal_t *internal, const module_elf_view_t *view)
{
    size_t      length  = 0;
    const char *depends = modinfo_find(view, "depends", 0, &length);
    if (!depends || !length) return EOK;
    char *list = duplicate_range(depends, length);
    if (!list) return -ENOMEM;

    char *cursor = list;
    int   result = EOK;
    while (*cursor) {
        char *end = strchr(cursor, ',');
        if (end) *end = 0;
        if (*cursor) {
            uint64_t           irq      = spin_lock_irqsave(&module_lock);
            module_internal_t *provider = module_find_locked(cursor);
            if (!provider)
                result = -ENOENT;
            else
                result = dependency_add(internal, provider->module);
            spin_unlock_irqrestore(&module_lock, irq);
            if (result != EOK) break;
        }
        if (!end) break;
        cursor = end + 1;
    }
    free(list);
    return result;
}

static void release_dependencies(module_internal_t *internal)
{
    module_dependency_t *dependency = internal->dependencies;
    while (dependency) {
        module_dependency_t *next = dependency->next;
        module_put(dependency->owner);
        free(dependency);
        dependency = next;
    }
    internal->dependencies = NULL;
}

/*
 * Section layout and mapping
 * Sections marked SHF_ALLOC are laid out page-aligned in a single
 * contiguous virtual region; .init* sections are kept separate so
 * their pages can be freed after module init runs.
 */

static int layout_sections(module_internal_t *internal, const module_elf_view_t *view)
{
    internal->sections = calloc(view->section_count, sizeof(*internal->sections));
    if (!internal->sections) return -ENOMEM;
    internal->section_count = view->section_count;

    size_t total = 0;
    for (size_t index = 0; index < view->section_count; index++) {
        const Elf64_Shdr *section = &view->sections[index];
        if (!(section->sh_flags & SHF_ALLOC)) continue;
        if (section->sh_flags & SHF_TLS) return -ENOEXEC;
        size_t alignment = section->sh_addralign;
        if (alignment < PAGE_4K_SIZE) alignment = PAGE_4K_SIZE;
        int ret = align_size(total, alignment, &total);
        if (ret != EOK) return ret;
        size_t mapped = 0;
        ret           = align_size(section->sh_size, PAGE_4K_SIZE, &mapped);
        if (ret != EOK || size_add(total, mapped, &total) != EOK || total > MODULE_MAX_SIZE) return ret != EOK ? ret : -E2BIG;

        module_section_layout_t *layout = &internal->sections[index];
        layout->size                    = section->sh_size;
        layout->flags                   = section->sh_flags;
        layout->alloc                   = true;
        const char *name                = section_name(view, index);
        if (!name) return -ENOEXEC;
        layout->name = strdup(name);
        if (!layout->name) return -ENOMEM;
        layout->init          = !strncmp(name, ".init", 5);
        layout->ro_after_init = streq(name, ".data..ro_after_init");
        if (layout->init)
            internal->module->init_size += mapped;
        else
            internal->module->core_size += mapped;
    }
    if (!total) return -ENOEXEC;
    internal->mapped_size = total;
    return EOK;
}

/* Release every page backing the module's mapped region */
static void unmap_module(module_internal_t *internal)
{
    if (!internal || !internal->frames) return;
    for (size_t index = 0; index < internal->page_count; index++) {
        if (!internal->frames[index]) continue;
        uint64_t frame = page_unmap(get_kernel_pagedir(), internal->base + index * PAGE_4K_SIZE);
        if (frame) (void)frame_release_range(frame, 1);
        internal->frames[index] = 0;
    }
    free(internal->frames);
    internal->frames     = NULL;
    internal->page_count = 0;
}

/* Allocate frames and map the laid-out sections into the kernel page tables */
static int map_sections(module_internal_t *internal, const module_elf_view_t *view)
{
    uintptr_t kernel_base = KERNEL_BASE_ADDRESS;
    if (kernel_address_request.response && kernel_address_request.response->virtual_base) kernel_base = kernel_address_request.response->virtual_base;
    uintptr_t search = kernel_base + MODULE_VADDR_OFFSET;
    uintptr_t base   = walk_page_tables_find_free(get_kernel_pagedir(), search, internal->mapped_size, PAGE_4K_SIZE);
    if (!base || base < search || base - kernel_base > MODULE_VADDR_LIMIT || internal->mapped_size > MODULE_VADDR_LIMIT - (base - kernel_base)) return -ENOMEM;

    internal->base       = base;
    internal->page_count = internal->mapped_size / PAGE_4K_SIZE;
    internal->frames     = calloc(internal->page_count, sizeof(uint64_t));
    if (!internal->frames) return -ENOMEM;

    size_t offset = 0;
    for (size_t index = 0; index < view->section_count; index++) {
        const Elf64_Shdr        *section = &view->sections[index];
        module_section_layout_t *layout  = &internal->sections[index];
        if (!layout->alloc) continue;
        size_t alignment = section->sh_addralign;
        if (alignment < PAGE_4K_SIZE) alignment = PAGE_4K_SIZE;
        if (align_size(offset, alignment, &offset) != EOK) return -EOVERFLOW;
        layout->address = base + offset;
        size_t mapped   = ALIGN_UP(section->sh_size, PAGE_4K_SIZE);

        for (size_t page = 0; page < mapped / PAGE_4K_SIZE; page++) {
            size_t   frame_index = offset / PAGE_4K_SIZE + page;
            uint64_t frame       = alloc_frames(1);
            if (!frame) return -ENOMEM;
            if (page_map_new_to(get_kernel_pagedir(), base + frame_index * PAGE_4K_SIZE, frame, PTE_PRESENT | PTE_WRITEABLE | PTE_NO_EXECUTE)) {
                (void)frame_release_range(frame, 1);
                return -ENOMEM;
            }
            internal->frames[frame_index] = frame;
        }
        if (section->sh_size) {
            memset((void *)layout->address, 0, section->sh_size);
            if (section->sh_type != SHT_NOBITS) memcpy((void *)layout->address, view->image + section->sh_offset, section->sh_size);
        }
        offset += mapped;
    }
    return EOK;
}

/*
 * Relocation
 * Apply x86-64 relocations after mapping, resolving symbols against
 * the kernel's export table and the module's own definitions.
 */

static size_t relocation_width(uint32_t type)
{
    switch (type) {
        case R_X86_64_NONE :
            return 0;
        case R_X86_64_8 :
        case R_X86_64_PC8 :
            return 1;
        case R_X86_64_16 :
        case R_X86_64_PC16 :
            return 2;
        case R_X86_64_PC32 :
        case R_X86_64_PLT32 :
        case R_X86_64_32 :
        case R_X86_64_32S :
        case R_X86_64_SIZE32 :
            return 4;
        case R_X86_64_64 :
        case R_X86_64_PC64 :
        case R_X86_64_SIZE64 :
            return 8;
        default :
            return SIZE_MAX;
    }
}

static int symbol_name_at(const module_elf_view_t *view, const Elf64_Shdr *symbols, const Elf64_Sym *symbol, const char **name_out)
{
    if (symbols->sh_link >= view->section_count) return -ENOEXEC;
    const Elf64_Shdr *strings = &view->sections[symbols->sh_link];
    if (strings->sh_type != SHT_STRTAB || symbol->st_name >= strings->sh_size) return -ENOEXEC;
    const char *name = (const char *)view->image + strings->sh_offset + symbol->st_name;
    if (!string_bounded(name, strings->sh_size - symbol->st_name)) return -ENOEXEC;
    *name_out = name;
    return EOK;
}

static int resolve_elf_symbol(module_internal_t *internal, const module_elf_view_t *view, const Elf64_Shdr *symbols, const Elf64_Sym *symbol, uint64_t *value_out)
{
    const char *name = NULL;
    int         ret  = symbol_name_at(view, symbols, symbol, &name);
    if (ret != EOK) return ret;
    if (name[0] && streq(name, "__this_module")) {
        *value_out = (uintptr_t)internal->module;
        return EOK;
    }
    if (symbol->st_shndx == SHN_ABS) {
        *value_out = symbol->st_value;
        return EOK;
    }
    if (symbol->st_shndx == SHN_UNDEF) {
        if (!name[0]) return -ENOEXEC;
        ret = resolve_export(internal, name, value_out, NULL);
        if (ret == -ENOENT && ELF64_ST_BIND(symbol->st_info) == STB_WEAK) {
            *value_out = 0;
            return EOK;
        }
        return ret;
    }
    if (symbol->st_shndx == SHN_COMMON || symbol->st_shndx == SHN_XINDEX || symbol->st_shndx >= view->section_count) return -ENOEXEC;
    module_section_layout_t *layout = &internal->sections[symbol->st_shndx];
    if (!layout->alloc || symbol->st_value > layout->size) return -ENOEXEC;
    *value_out = layout->address + symbol->st_value;
    return EOK;
}

static int relocate_module(module_internal_t *internal, const module_elf_view_t *view)
{
    for (size_t section_index = 0; section_index < view->section_count; section_index++) {
        const Elf64_Shdr *relocations = &view->sections[section_index];
        if (relocations->sh_type != SHT_RELA) continue;
        if (relocations->sh_info >= view->section_count || relocations->sh_link >= view->section_count) return -ENOEXEC;
        module_section_layout_t *target = &internal->sections[relocations->sh_info];
        if (!target->alloc) continue;
        const Elf64_Shdr *symbols = &view->sections[relocations->sh_link];
        if (symbols->sh_type != SHT_SYMTAB || symbols->sh_entsize != sizeof(Elf64_Sym)) return -ENOEXEC;
        const Elf64_Sym  *symbol_table = (const Elf64_Sym *)(view->image + symbols->sh_offset);
        size_t            symbol_count = symbols->sh_size / sizeof(Elf64_Sym);
        const Elf64_Rela *table        = (const Elf64_Rela *)(view->image + relocations->sh_offset);
        size_t            count        = relocations->sh_size / sizeof(Elf64_Rela);

        for (size_t index = 0; index < count; index++) {
            uint32_t type         = ELF64_R_TYPE(table[index].r_info);
            size_t   width        = relocation_width(type);
            size_t   symbol_index = ELF64_R_SYM(table[index].r_info);
            if (width == SIZE_MAX || symbol_index >= symbol_count || table[index].r_offset > target->size || width > target->size - table[index].r_offset) return -ENOEXEC;
            const Elf64_Sym *symbol = &symbol_table[symbol_index];
            if (ELF64_ST_TYPE(symbol->st_info) == STT_GNU_IFUNC || ELF64_ST_TYPE(symbol->st_info) == STT_TLS) return -ENOEXEC;
            uint64_t value = 0;
            int      ret   = resolve_elf_symbol(internal, view, symbols, symbol, &value);
            if (ret != EOK) return ret;
            if (type == R_X86_64_SIZE32 || type == R_X86_64_SIZE64) value = symbol->st_size;
            uintptr_t place = target->address + table[index].r_offset;
            ret             = module_elf_apply_relocation(type, (void *)place, value, table[index].r_addend, place);
            if (ret != EOK) return ret;
        }
    }
    return EOK;
}

static int module_page_mapped(const module_internal_t *internal, uintptr_t address)
{
    if (address < internal->base || address - internal->base >= internal->mapped_size) return 0;
    size_t index = (address - internal->base) / PAGE_4K_SIZE;
    return index < internal->page_count && internal->frames[index] != 0;
}

static int module_range_mapped(const module_internal_t *internal, uintptr_t address, size_t size)
{
    if (!address_in_module(internal, address, size)) return 0;
    if (!size) return 1;
    uintptr_t end = address + size - 1;
    for (uintptr_t page = ALIGN_DOWN(address, PAGE_4K_SIZE); page <= ALIGN_DOWN(end, PAGE_4K_SIZE); page += PAGE_4K_SIZE) {
        if (!module_page_mapped(internal, page)) return 0;
        if (page > (uintptr_t)-1 - PAGE_4K_SIZE) break;
    }
    return 1;
}

static int module_string_valid(const module_internal_t *internal, const char *string)
{
    uintptr_t address = (uintptr_t)string;
    if (!module_page_mapped(internal, address)) return 0;
    size_t limit = internal->mapped_size - (address - internal->base);
    if (limit > MODULE_MAX_SIZE) limit = MODULE_MAX_SIZE;
    for (size_t index = 0; index < limit; index++) {
        if (!module_page_mapped(internal, address + index)) return 0;
        if (!string[index]) return 1;
    }
    return 0;
}

static int find_lifecycle(module_internal_t *internal, const module_elf_view_t *view)
{
    const Elf64_Shdr *symbols = NULL;
    for (size_t index = 0; index < view->section_count; index++) {
        if (view->sections[index].sh_type == SHT_SYMTAB) {
            symbols = &view->sections[index];
            break;
        }
    }
    if (!symbols) return -ENOEXEC;
    const Elf64_Sym *table = (const Elf64_Sym *)(view->image + symbols->sh_offset);
    size_t           count = symbols->sh_size / sizeof(Elf64_Sym);
    for (size_t index = 0; index < count; index++) {
        const char *name = NULL;
        if (symbol_name_at(view, symbols, &table[index], &name) != EOK) return -ENOEXEC;
        if (!name[0] || (!streq(name, "init_module") && !streq(name, "cleanup_module"))) continue;
        uint64_t value = 0;
        int      ret   = resolve_elf_symbol(internal, view, symbols, &table[index], &value);
        if (ret != EOK || !module_range_mapped(internal, value, 1)) return -ENOEXEC;
        if (streq(name, "init_module"))
            internal->init = (int (*)(void))value;
        else
            internal->exit = (void (*)(void))value;
    }
    return EOK;
}

static int validate_exports(module_internal_t *internal, const module_elf_view_t *view)
{
    size_t            section_index = 0;
    const Elf64_Shdr *section       = find_section(view, "__ksymtab", &section_index);
    if (!section) return EOK;
    module_section_layout_t *layout = &internal->sections[section_index];
    if (!layout->alloc || section->sh_size % sizeof(struct kernel_symbol)) return -ENOEXEC;
    internal->exports      = (const struct kernel_symbol *)layout->address;
    internal->export_count = section->sh_size / sizeof(struct kernel_symbol);

    for (size_t index = 0; index < internal->export_count; index++) {
        const struct kernel_symbol *symbol = &internal->exports[index];
        if (!module_string_valid(internal, symbol->name) || !symbol->name[0] || !module_range_mapped(internal, symbol->value, 1)
            || (symbol->namespace_name && !module_string_valid(internal, symbol->namespace_name)) || (symbol->flags & ~KERNEL_SYMBOL_GPL_ONLY))
            return -ENOEXEC;
        for (size_t prior = 0; prior < index; prior++)
            if (streq(internal->exports[prior].name, symbol->name)) return -EEXIST;
        for (const struct kernel_symbol *core = __start___ksymtab; core < __stop___ksymtab; core++)
            if (core->name && streq(core->name, symbol->name)) return -EEXIST;
    }

    uint64_t irq = spin_lock_irqsave(&module_lock);
    for (module_internal_t *item = module_list; item; item = item->next) {
        for (size_t existing = 0; existing < item->export_count; existing++) {
            for (size_t added = 0; added < internal->export_count; added++) {
                if (streq(item->exports[existing].name, internal->exports[added].name)) {
                    spin_unlock_irqrestore(&module_lock, irq);
                    return -EEXIST;
                }
            }
        }
    }
    spin_unlock_irqrestore(&module_lock, irq);
    return EOK;
}

static int parse_unsigned_value(const char *value, uint64_t *result)
{
    if (!value || !value[0] || value[0] == '-') return -EINVAL;
    unsigned int base  = 10;
    size_t       index = 0;
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        base  = 16;
        index = 2;
    } else if (value[0] == '0' && value[1]) {
        base  = 8;
        index = 1;
    }
    uint64_t number = 0;
    int      digits = 0;
    for (; value[index]; index++) {
        unsigned int digit;
        if (value[index] >= '0' && value[index] <= '9')
            digit = value[index] - '0';
        else if (value[index] >= 'a' && value[index] <= 'f')
            digit = value[index] - 'a' + 10;
        else if (value[index] >= 'A' && value[index] <= 'F')
            digit = value[index] - 'A' + 10;
        else
            return -EINVAL;
        if (digit >= base || number > (UINT64_MAX - digit) / base) return -ERANGE;
        number = number * base + digit;
        digits = 1;
    }
    if (!digits) return -EINVAL;
    *result = number;
    return EOK;
}

static int parse_signed_value(const char *value, int64_t *result)
{
    if (!value || !value[0]) return -EINVAL;
    int      negative = value[0] == '-';
    uint64_t number   = 0;
    int      ret      = parse_unsigned_value(value + negative, &number);
    if (ret != EOK) return ret;
    if ((!negative && number > INT64_MAX) || (negative && number > (uint64_t)INT64_MAX + 1)) return -ERANGE;
    *result = negative ? (number == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)number) : (int64_t)number;
    return EOK;
}

static int set_parameter(module_internal_t *internal, const struct kernel_param *parameter, const char *value)
{
    if (!module_string_valid(internal, parameter->name) || !module_range_mapped(internal, (uintptr_t)parameter->arg, 1)) return -ENOEXEC;
    uint64_t unsigned_value = 0;
    int64_t  signed_value   = 0;
    int      ret;
    switch (parameter->type) {
        case MODULE_PARAM_BYTE :
            ret = parse_unsigned_value(value, &unsigned_value);
            if (ret != EOK || unsigned_value > UINT8_MAX) return ret != EOK ? ret : -ERANGE;
            *(uint8_t *)parameter->arg = (uint8_t)unsigned_value;
            return EOK;
        case MODULE_PARAM_SHORT :
            ret = parse_signed_value(value, &signed_value);
            if (ret != EOK || signed_value < INT16_MIN || signed_value > INT16_MAX) return ret != EOK ? ret : -ERANGE;
            *(int16_t *)parameter->arg = (int16_t)signed_value;
            return EOK;
        case MODULE_PARAM_USHORT :
            ret = parse_unsigned_value(value, &unsigned_value);
            if (ret != EOK || unsigned_value > UINT16_MAX) return ret != EOK ? ret : -ERANGE;
            *(uint16_t *)parameter->arg = (uint16_t)unsigned_value;
            return EOK;
        case MODULE_PARAM_INT :
            ret = parse_signed_value(value, &signed_value);
            if (ret != EOK || signed_value < (int64_t)INT32_MIN || signed_value > (int64_t)INT32_MAX) return ret != EOK ? ret : -ERANGE;
            *(int32_t *)parameter->arg = (int32_t)signed_value;
            return EOK;
        case MODULE_PARAM_UINT :
            ret = parse_unsigned_value(value, &unsigned_value);
            if (ret != EOK || unsigned_value > (uint64_t)UINT32_MAX) return ret != EOK ? ret : -ERANGE;
            *(uint32_t *)parameter->arg = (uint32_t)unsigned_value;
            return EOK;
        case MODULE_PARAM_LONG :
            ret = parse_signed_value(value, &signed_value);
            if (ret == EOK && signed_value >= INT64_MIN && signed_value <= INT64_MAX) *(int64_t *)parameter->arg = signed_value;
            return ret;
        case MODULE_PARAM_ULONG :
            ret = parse_unsigned_value(value, &unsigned_value);
            if (ret == EOK) *(uint64_t *)parameter->arg = unsigned_value;
            return ret;
        case MODULE_PARAM_BOOL :
            if (streq(value, "1") || streq(value, "y") || streq(value, "Y") || streq(value, "yes") || streq(value, "true") || streq(value, "on"))
                *(bool *)parameter->arg = true;
            else if (streq(value, "0") || streq(value, "n") || streq(value, "N") || streq(value, "no") || streq(value, "false") || streq(value, "off"))
                *(bool *)parameter->arg = false;
            else
                return -EINVAL;
            return EOK;
        case MODULE_PARAM_CHARP : {
            char *copy = strdup(value);
            if (!copy) return -ENOMEM;
            module_string_allocation_t *allocation = calloc(1, sizeof(*allocation));
            if (!allocation) {
                free(copy);
                return -ENOMEM;
            }
            allocation->value           = copy;
            allocation->next            = internal->parameter_strings;
            internal->parameter_strings = allocation;
            *(char **)parameter->arg    = copy;
            return EOK;
        }
        default :
            return -ENOEXEC;
    }
}

static int tokenize_parameter(char **cursor_ptr, char **name_out, char **value_out)
{
    char *cursor = *cursor_ptr;
    while (IS_SPACE(*cursor)) cursor++;
    if (!*cursor) {
        *cursor_ptr = cursor;
        return 0;
    }
    char *write = cursor;
    char *start = cursor;
    char *equal = NULL;
    char  quote = 0;
    while (*cursor) {
        char c = *cursor++;
        if (c == '\\' && *cursor) {
            *write++ = *cursor++;
            continue;
        }
        if ((c == '\'' || c == '"')) {
            if (!quote) {
                quote = c;
                continue;
            }
            if (quote == c) {
                quote = 0;
                continue;
            }
        }
        if (!quote && c == '=' && !equal) {
            equal    = write;
            *write++ = 0;
            continue;
        }
        if (!quote && IS_SPACE(c)) break;
        *write++ = c;
    }
    if (quote || !equal || !start[0]) return -EINVAL;
    *write      = 0;
    *name_out   = start;
    *value_out  = equal + 1;
    *cursor_ptr = cursor;
    return 1;
}

static int apply_parameters(module_internal_t *internal, const module_elf_view_t *view, const char *arguments)
{
    size_t            section_index = 0;
    const Elf64_Shdr *section       = find_section(view, "__param", &section_index);
    if (section && section->sh_size % sizeof(struct kernel_param) == 0) {
        module_section_layout_t *layout = &internal->sections[section_index];
        if (layout->alloc) {
            internal->parameters      = (const struct kernel_param *)layout->address;
            internal->parameter_count = section->sh_size / sizeof(struct kernel_param);
        }
    }

    if (!arguments || !arguments[0]) return EOK;

    const struct kernel_param *parameters = internal->parameters;
    size_t                     count      = internal->parameter_count;
    if (!parameters || !count) return EOK;

    char *copy = strdup(arguments);
    if (!copy) return -ENOMEM;
    char *cursor = copy;
    int   result = EOK;
    for (;;) {
        char *name  = NULL;
        char *value = NULL;
        int   token = tokenize_parameter(&cursor, &name, &value);
        if (token <= 0) {
            result = token;
            break;
        }
        char *dot = strchr(name, '.');
        if (dot) {
            *dot = 0;
            if (!streq(name, internal->module->name)) {
                result = -EINVAL;
                break;
            }
            name = dot + 1;
        }
        const struct kernel_param *match = NULL;
        for (size_t index = 0; index < count; index++) {
            if (!module_string_valid(internal, parameters[index].name)) {
                result = -ENOEXEC;
                break;
            }
            if (streq(parameters[index].name, name)) match = &parameters[index];
        }
        if (result != EOK) break;
        if (!match) {
            result = -EINVAL;
            break;
        }
        result = set_parameter(internal, match, value);
        if (result != EOK) break;
    }
    free(copy);
    return result;
}

static int protect_module(module_internal_t *internal, int after_init)
{
    for (size_t section = 0; section < internal->section_count; section++) {
        module_section_layout_t *layout = &internal->sections[section];
        if (!layout->alloc || !layout->size) continue;
        if ((layout->flags & SHF_EXECINSTR) && (layout->flags & SHF_WRITE)) return -ENOEXEC;
        uint64_t flags = PTE_PRESENT;
        if (!(layout->flags & SHF_EXECINSTR)) flags |= PTE_NO_EXECUTE;
        if ((layout->flags & SHF_WRITE) && !(after_init && layout->ro_after_init)) flags |= PTE_WRITEABLE;
        size_t mapped = ALIGN_UP(layout->size, PAGE_4K_SIZE);
        for (size_t offset = 0; offset < mapped; offset += PAGE_4K_SIZE) {
            size_t page_index = (layout->address - internal->base + offset) / PAGE_4K_SIZE;
            if (page_index >= internal->page_count || !internal->frames[page_index]) return -ENOEXEC;
            page_map_to(get_kernel_pagedir(), layout->address + offset, internal->frames[page_index], flags);
        }
    }
    __asm__ volatile("mfence" ::: "memory");
    return EOK;
}

static void module_sysfs_teardown(module_internal_t *internal)
{
    module_sysfs_t *entry = internal->sysfs;
    if (!entry) return;
    internal->sysfs = NULL;
    module_sysfs_destroy(entry);
}

static void remove_registry(module_internal_t *internal)
{
    uint64_t            irq  = spin_lock_irqsave(&module_lock);
    module_internal_t **link = &module_list;
    while (*link) {
        if (*link == internal) {
            *link = internal->next;
            break;
        }
        link = &(*link)->next;
    }
    internal->next = NULL;
    spin_unlock_irqrestore(&module_lock, irq);
}

static void destroy_internal(module_internal_t *internal)
{
    if (!internal) return;
    module_sysfs_teardown(internal);
    release_dependencies(internal);
    module_string_allocation_t *allocation = internal->parameter_strings;
    while (allocation) {
        module_string_allocation_t *next = allocation->next;
        free(allocation->value);
        free(allocation);
        allocation = next;
    }
    unmap_module(internal);
    for (size_t i = 0; i < internal->section_count; i++) free(internal->sections[i].name);
    free(internal->sections);
    free(internal->license);
    free(internal->version);
    free(internal->srcversion);
    free(internal->imports);
    if (internal->module) {
        __atomic_store_n((module_internal_t **)&internal->module->loader_private, NULL, __ATOMIC_RELEASE);
        free(internal->module);
    }
    free(internal);
}

/* Public API */

/* Load a kernel module: validate, layout, map, relocate, then run init.
 * The pipeline is all-or-nothing: any failure destroys the partial state. */
int module_load(const void *image, size_t size, const char *params, unsigned int flags, const char *name_hint)
{
#if !CONFIG_MODULES
    (void)image;
    (void)size;
    (void)params;
    (void)flags;
    (void)name_hint;
    return -ENOSYS;
#else
    if (!image || !size || size > MODULE_MAX_SIZE) return !image ? -EFAULT : -EFBIG;
    if (flags & ~(MODULE_INIT_IGNORE_MODVERSIONS | MODULE_INIT_IGNORE_VERMAGIC | MODULE_INIT_COMPRESSED_FILE)) return -EINVAL;
    if (flags & MODULE_INIT_COMPRESSED_FILE) return -EOPNOTSUPP;
    int result = operation_begin();
    if (result != EOK) return result;

    module_elf_view_t view;
    result = module_elf_validate(image, size, &view);
    if (result != EOK) goto out_operation;

    module_internal_t *internal = calloc(1, sizeof(*internal));
    struct module     *module   = calloc(1, sizeof(*module));
    if (!internal || !module) {
        free(internal);
        free(module);
        result = -ENOMEM;
        goto out_operation;
    }
    internal->module       = module;
    module->loader_private = internal;
    module->state          = MODULE_STATE_UNFORMED;
    wait_queue_init(&internal->unload_wait);

    result = prepare_metadata(internal, &view, flags, name_hint);
    if (result != EOK) goto out_destroy;
    uint64_t irq       = spin_lock_irqsave(&module_lock);
    int      duplicate = module_find_locked(module->name) != NULL;
    spin_unlock_irqrestore(&module_lock, irq);
    if (duplicate) {
        result = -EEXIST;
        goto out_destroy;
    }
    result = load_declared_dependencies(internal, &view);
    if (result != EOK) goto out_destroy;
    result = layout_sections(internal, &view);
    if (result != EOK) goto out_destroy;
    result = map_sections(internal, &view);
    if (result != EOK) goto out_destroy;
    result = relocate_module(internal, &view);
    if (result != EOK) goto out_destroy;
    result = find_lifecycle(internal, &view);
    if (result != EOK) goto out_destroy;
    result = validate_exports(internal, &view);
    if (result != EOK) goto out_destroy;
    result = apply_parameters(internal, &view, params);
    if (result != EOK) goto out_destroy;
    result = protect_module(internal, 0);
    if (result != EOK) goto out_destroy;

    module->state  = MODULE_STATE_COMING;
    irq            = spin_lock_irqsave(&module_lock);
    internal->next = module_list;
    module_list    = internal;
    spin_unlock_irqrestore(&module_lock, irq);

    if (internal->init) {
        int init_result = internal->init();
        if (init_result != EOK) {
            result        = init_result < 0 ? init_result : -EINVAL;
            module->state = MODULE_STATE_GOING;
            remove_registry(internal);
            goto out_destroy;
        }
    }
    result = protect_module(internal, 1);
    if (result != EOK) {
        module->state = MODULE_STATE_GOING;
        if (internal->exit) internal->exit();
        remove_registry(internal);
        goto out_destroy;
    }
    __atomic_store_n(&module->state, MODULE_STATE_LIVE, __ATOMIC_RELEASE);
    result = module_sysfs_create(internal->module, &internal->sysfs);
    if (result != EOK) {
        __atomic_store_n(&module->state, MODULE_STATE_GOING, __ATOMIC_RELEASE);
        if (internal->exit) internal->exit();
        remove_registry(internal);
        goto out_destroy;
    }
    operation_end();
    return EOK;
out_destroy:
    plogk("module: Load of \"%s\" failed: %d\n", internal->module->name[0] ? internal->module->name : "(unnamed)", result);
    destroy_internal(internal);
out_operation:
    operation_end();
    return result;
#endif
}

/* Unload a module by name after dropping its refcount to zero */
int module_unload(const char *name, unsigned int flags)
{
#if !CONFIG_MODULES
    (void)name;
    (void)flags;
    return -ENOSYS;
#else
    if (!module_name_valid(name) || (flags & ~(MODULE_DELETE_NONBLOCK | MODULE_DELETE_FORCE))) return -EINVAL;
    if ((flags & MODULE_DELETE_FORCE) && !CONFIG_MODULE_FORCE_UNLOAD) return -EPERM;
    int result = operation_begin();
    if (result != EOK) return result;

    module_internal_t *internal;
    for (;;) {
        uint64_t irq = spin_lock_irqsave(&module_lock);
        internal     = module_find_locked(name);
        if (!internal) {
            spin_unlock_irqrestore(&module_lock, irq);
            result = -ENOENT;
            goto out;
        }
        if (internal->module->state != MODULE_STATE_LIVE) {
            spin_unlock_irqrestore(&module_lock, irq);
            result = -EBUSY;
            goto out;
        }
        uint32_t refs = module_refcount(internal->module);
        if (!refs || (flags & MODULE_DELETE_FORCE)) {
            if (refs) {
                internal->module->taints |= MODULE_TAINT_FORCED;
                __atomic_store_n(&internal->module->refcount, 0, __ATOMIC_RELEASE);
            }
            __atomic_store_n(&internal->module->state, MODULE_STATE_GOING, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&module_lock, irq);
            break;
        }
        if (flags & MODULE_DELETE_NONBLOCK) {
            spin_unlock_irqrestore(&module_lock, irq);
            result = -EWOULDBLOCK;
            goto out;
        }
        wait_queue_prepare(&internal->unload_wait);
        spin_unlock_irqrestore(&module_lock, irq);
        wait_queue_sleep();
    }

    module_sysfs_teardown(internal);
    if (internal->exit) internal->exit();
    remove_registry(internal);
    destroy_internal(internal);
    result = EOK;
out:
    operation_end();
    return result;
#endif
}

/* Format the /proc/modules listing for all loaded modules */
size_t module_format_proc(char *buffer, size_t size)
{
    if (!buffer || !size) return 0;
    size_t   written = 0;
    uint64_t irq     = spin_lock_irqsave(&module_lock);
    for (module_internal_t *item = module_list; item && written < size - 1; item = item->next) {
        char   users[256];
        size_t users_len = 0;
        for (module_internal_t *consumer = module_list; consumer; consumer = consumer->next) {
            for (module_dependency_t *dep = consumer->dependencies; dep; dep = dep->next) {
                if (dep->owner != item->module) continue;
                int n = snprintf(users + users_len, sizeof(users) - users_len, "%s%s", users_len ? "," : "", consumer->module->name);
                if (n > 0 && (size_t)n < sizeof(users) - users_len) users_len += (size_t)n;
                break;
            }
        }
        if (!users_len) {
            users[0] = '-';
            users[1] = 0;
        }
        int n = snprintf(buffer + written, size - written, "%s %zu %u %s %s 0x%llx\n", item->module->name, item->module->core_size + item->module->init_size, module_refcount(item->module), users,
                         item->module->state == MODULE_STATE_LIVE ? "Live" : module_state_name(item->module->state), (unsigned long long)item->base);
        if (n < 0) break;
        if ((size_t)n >= size - written) {
            written = size - 1;
            break;
        }
        written += (size_t)n;
    }
    spin_unlock_irqrestore(&module_lock, irq);
    buffer[written] = 0;
    return written;
}

/* Install the module signature verifier; only possible before any module loads */
int module_set_signature_verifier(module_signature_verifier_t verifier)
{
    if (!verifier) return -EINVAL;
    uint64_t irq = spin_lock_irqsave(&module_lock);
    if (signature_verifier || module_list) {
        spin_unlock_irqrestore(&module_lock, irq);
        return -EBUSY;
    }
    signature_verifier = verifier;
    spin_unlock_irqrestore(&module_lock, irq);
    return EOK;
}

/* Initialize the module registry and locate the sysfs module object */
void module_subsystem_init(void)
{
    memset(&module_lock, 0, sizeof(module_lock));
    module_list      = NULL;
    module_operation = 0;
}
