/*
 *
 *      module.h
 *      Loadable kernel module ABI and lifecycle interface
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_MODULE_H_
#define INCLUDE_MODULE_H_

#include <kernel/uinxed.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define MODULE_NAME_LEN                64
#define MODULE_PARAM_MAX               4096
#define MODULE_INIT_IGNORE_MODVERSIONS 0x0001U
#define MODULE_INIT_IGNORE_VERMAGIC    0x0002U
#define MODULE_INIT_COMPRESSED_FILE    0x0004U
#define MODULE_DELETE_NONBLOCK         0x0800U
#define MODULE_DELETE_FORCE            0x0200U

enum module_state {
    MODULE_STATE_UNFORMED,
    MODULE_STATE_COMING,
    MODULE_STATE_LIVE,
    MODULE_STATE_GOING,
};

enum module_taint {
    MODULE_TAINT_PROPRIETARY = 1U << 0,
    MODULE_TAINT_FORCED      = 1U << 1,
    MODULE_TAINT_UNSIGNED    = 1U << 2,
    MODULE_TAINT_OUT_OF_TREE = 1U << 3,
};

struct module {
        char              name[MODULE_NAME_LEN];
        enum module_state state;
        volatile uint32_t refcount;
        uint32_t          taints;
        size_t            core_size;
        size_t            init_size;
        void             *loader_private;
};

struct kernel_symbol {
        uintptr_t   value;
        const char *name;
        const char *namespace_name;
        uint32_t    flags;
        uint32_t    crc;
};

#define KERNEL_SYMBOL_GPL_ONLY (1U << 0)

enum module_param_type {
    MODULE_PARAM_BYTE,
    MODULE_PARAM_SHORT,
    MODULE_PARAM_USHORT,
    MODULE_PARAM_INT,
    MODULE_PARAM_UINT,
    MODULE_PARAM_LONG,
    MODULE_PARAM_ULONG,
    MODULE_PARAM_BOOL,
    MODULE_PARAM_CHARP,
};

struct kernel_param {
        const char            *name;
        void                  *arg;
        enum module_param_type type;
        uint16_t               perm;
};

typedef int (*module_signature_verifier_t)(const void *image, size_t image_size, const void *signature, size_t signature_size);

/* Bring up the registry and /sys/module integration. */
void module_subsystem_init(void);

/* Load and unload module images. All failures are negative errno values. */
int module_load(const void *image, size_t size, const char *params, unsigned int flags, const char *name_hint);
int module_unload(const char *name, unsigned int flags);

/* Runtime ownership. A successful get must be paired with module_put(). */
int            try_module_get(struct module *module);
void           __module_get(struct module *module);
void           module_put(struct module *module);
uint32_t       module_refcount(const struct module *module);
struct module *module_find_get(const char *name);

/* Resolve an exported symbol and pin its owner until module_symbol_put(). */
void *module_symbol_get(const char *name, struct module **owner);
void  module_symbol_put(struct module *owner);

/* Generate Linux-compatible /proc/modules content. */
size_t module_format_proc(char *buffer, size_t size);

/* Install a platform signature verifier before loading signed modules. */
int module_set_signature_verifier(module_signature_verifier_t verifier);

#define __MODULE_JOIN_INNER(a, b) a##b
#define __MODULE_JOIN(a, b)       __MODULE_JOIN_INNER(a, b)
#define __MODULE_STRING_INNER(x)  #x
#define __MODULE_STRING(x)        __MODULE_STRING_INNER(x)
#define __module_used             __attribute__((used))
#define __module_section(name)    __attribute__((section(name)))

#define __MODULE_INFO(tag, info, counter) \
    static const char __MODULE_JOIN(__modinfo_, counter)[] __module_used __module_section(".modinfo") = #tag "=" info
#define MODULE_INFO(tag, info) __MODULE_INFO(tag, info, __COUNTER__)

#define MODULE_NAME(name)        MODULE_INFO(name, name)
#define MODULE_LICENSE(license)  MODULE_INFO(license, license)
#define MODULE_AUTHOR(author)    MODULE_INFO(author, author)
#define MODULE_DESCRIPTION(desc) MODULE_INFO(description, desc)
#define MODULE_VERSION(version)  MODULE_INFO(version, version)
#define MODULE_ALIAS(alias)      MODULE_INFO(alias, alias)
#define MODULE_FIRMWARE(file)    MODULE_INFO(firmware, file)
#define MODULE_SOFTDEP(value)    MODULE_INFO(softdep, value)
#define MODULE_IMPORT_NS(ns)     MODULE_INFO(import_ns, __MODULE_STRING(ns))
#define MODULE_DEPENDS(value)    MODULE_INFO(depends, value)

#define __EXPORT_SYMBOL(symbol, namespace_value, export_flags)                                            \
    static const char __kstrtab_##symbol[] __module_used __module_section("__ksymtab_strings") = #symbol; \
    static const struct kernel_symbol __ksymtab_##symbol __module_used __module_section("__ksymtab")      \
        = {(uintptr_t) & (symbol), __kstrtab_##symbol, namespace_value, export_flags, 0}
#define EXPORT_SYMBOL(symbol)            __EXPORT_SYMBOL(symbol, NULL, 0)
#define EXPORT_SYMBOL_GPL(symbol)        __EXPORT_SYMBOL(symbol, NULL, KERNEL_SYMBOL_GPL_ONLY)
#define EXPORT_SYMBOL_NS(symbol, ns)     __EXPORT_SYMBOL(symbol, __MODULE_STRING(ns), 0)
#define EXPORT_SYMBOL_NS_GPL(symbol, ns) __EXPORT_SYMBOL(symbol, __MODULE_STRING(ns), KERNEL_SYMBOL_GPL_ONLY)

#define __MODULE_PARAM(name, kind, permissions)                                                       \
    static const char __param_str_##name[] __module_used __module_section("__param_strings") = #name; \
    static const struct kernel_param __param_##name __module_used __module_section("__param")         \
        = {__param_str_##name, &(name), MODULE_PARAM_##kind, permissions}
#define module_param(name, type, permissions) __MODULE_PARAM(name, type, permissions)
#define MODULE_PARAM_byte                     MODULE_PARAM_BYTE
#define MODULE_PARAM_short                    MODULE_PARAM_SHORT
#define MODULE_PARAM_ushort                   MODULE_PARAM_USHORT
#define MODULE_PARAM_int                      MODULE_PARAM_INT
#define MODULE_PARAM_uint                     MODULE_PARAM_UINT
#define MODULE_PARAM_long                     MODULE_PARAM_LONG
#define MODULE_PARAM_ulong                    MODULE_PARAM_ULONG
#define MODULE_PARAM_bool                     MODULE_PARAM_BOOL
#define MODULE_PARAM_charp                    MODULE_PARAM_CHARP

#define MODULE_PARM_DESC(name, description) MODULE_INFO(parm, #name ":" description)

#define __init                __attribute__((section(".init.text")))
#define __exit                __attribute__((section(".exit.text")))
#define module_init(function) int init_module(void) __attribute__((alias(#function)))
#define module_exit(function) void cleanup_module(void) __attribute__((alias(#function)))

#ifndef UINXED_MODULE_CORE
__attribute__((weak, used, section(".gnu.linkonce.this_module"))) struct module __this_module;
#    define THIS_MODULE (&__this_module)
static const char __module_vermagic[] __module_used __module_section(".modinfo") = "vermagic=" KERNEL_VERSION;
#endif

#endif // INCLUDE_MODULE_H_
