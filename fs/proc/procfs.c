/*
 *
 *      procfs.c
 *      Process file system
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpuid.h>
#include <arch/smp.h>
#include <cgroup/cgroup.h>
#include <drivers/time/tsc.h>
#include <drivers/tty/tty_core.h>
#include <fs/core/vfs.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <fs/proc/procfs.h>
#include <kernel/cmdline/cmdline.h>
#include <kernel/errno.h>
#include <kernel/module/module.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <kernel/uinxed.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/pagecache.h>
#include <mem/swap.h>
#include <net/abi/inet.h>
#include <net/core/netdev.h>
#include <net/socket.h>
#include <process/process.h>
#include <process/sched.h>

static int procfs_id;

/* Internal types */

typedef enum procfs_info_type {
    PROC_INFO_STAT,
    PROC_INFO_MEMINFO,
    PROC_INFO_CPUINFO,
    PROC_INFO_UPTIME,
    PROC_INFO_VERSION,
    PROC_INFO_LOADAVG,
    PROC_INFO_VMSTAT,
    PROC_INFO_MODULES,
    PROC_INFO_MOUNTS,
    PROC_INFO_MOUNTINFO,
    PROC_INFO_FILESYSTEMS,
    PROC_INFO_CMDLINE,
    PROC_INFO_CGROUPS,
    PROC_INFO_INTERRUPTS,
    PROC_INFO_PARTITIONS,
    PROC_INFO_DEVICES,
    PROC_INFO_DISKSTATS,
    PROC_INFO_SWAPS,
    PROC_INFO_MISC,
    PROC_INFO_SOFTIRQS,
    PROC_INFO_IOPORTS,
    PROC_INFO_IOMEM,
} procfs_info_type_t;

typedef enum procfs_net_file_type {
    PROC_NET_DEV,
    PROC_NET_ARP,
    PROC_NET_ROUTE,
    PROC_NET_TCP,
    PROC_NET_UDP,
    PROC_NET_UNIX,
} procfs_net_file_type_t;

typedef enum procfs_pid_file_type {
    PROC_PID_STATUS,
    PROC_PID_MAPS,
    PROC_PID_CMDLINE,
    PROC_PID_NAME,
    PROC_PID_STAT,
    PROC_PID_MEM,
    PROC_PID_MOUNTS,
    PROC_PID_MOUNTINFO,
    PROC_PID_CGROUP,
    PROC_PID_COMM,
    PROC_PID_STATM,
    PROC_PID_LIMITS,
    PROC_PID_IO,
    PROC_PID_OOM_SCORE_ADJ,
} procfs_pid_file_type_t;

typedef enum procfs_type {
    PROCFS_ROOT,
    PROCFS_PID_DIR,
    PROCFS_INFO_FILE,
    PROCFS_PID_FILE,
    PROCFS_NET_DIR,
    PROCFS_NET_FILE,
    PROCFS_SELF_LINK,
    PROCFS_PID_FD_DIR,
    PROCFS_PID_FD_LINK,
    PROCFS_PID_EXE_LINK,
    PROCFS_PID_CWD_LINK,
    PROCFS_PID_ROOT_LINK,
    PROCFS_TTY_DIR,
    PROCFS_TTY_FILE,
    PROCFS_SYS_DIR,
    PROCFS_SYS_FILE,
    PROCFS_DRIVER_DIR,
} procfs_type_t;

typedef struct procfs_file {
        procfs_type_t type;
        pid_t         pid;
        int           subtype;
        char         *content;
        size_t        size;
        size_t        capacity;
} procfs_file_t;

#define PROCFS_BUF_SIZE 4096

/* Lightweight sysctl table for /proc/sys */

typedef enum procfs_sysctl_kind {
    PROC_SYS_UINT,  // single unsigned integer
    PROC_SYS_STR,   // NUL-terminated string
    PROC_SYS_MULTI, // space-separated integer vector
} procfs_sysctl_kind_t;

typedef struct procfs_sysctl {
        const char *name;
        uint8_t     kind;
        uint8_t     readonly;
        uint8_t     count;
        uint64_t    values[4];
        char        string[64];
} procfs_sysctl_t;

static procfs_sysctl_t procfs_sysctl_kernel[] = {
    {.name = "hostname", .kind = PROC_SYS_STR, .string = "localhost", .count = 0},
    {.name = "domainname", .kind = PROC_SYS_STR, .string = "(none)", .count = 0},
    {.name = "ostype", .kind = PROC_SYS_STR, .string = KERNEL_NAME, .readonly = 1, .count = 0},
    {.name = "osrelease", .kind = PROC_SYS_STR, .string = KERNEL_VERSION, .readonly = 1, .count = 0},
    {.name = "version", .kind = PROC_SYS_STR, .string = KERNEL_NAME " version " KERNEL_VERSION, .readonly = 1, .count = 0},
    {.name = "panic", .kind = PROC_SYS_UINT, .values = {0}, .count = 1},
    {.name = "panic_on_oops", .kind = PROC_SYS_UINT, .values = {0}, .count = 1},
    {.name = "pid_max", .kind = PROC_SYS_UINT, .values = {32768}, .count = 1},
    {.name = "threads-max", .kind = PROC_SYS_UINT, .values = {65536}, .readonly = 1, .count = 1},
    {.name = "randomize_va_space", .kind = PROC_SYS_UINT, .values = {2}, .readonly = 1, .count = 1},
    {.name = "perf_event_paranoid", .kind = PROC_SYS_UINT, .values = {3}, .readonly = 1, .count = 1},
    {.name = "kptr_restrict", .kind = PROC_SYS_UINT, .values = {0}, .readonly = 1, .count = 1},
    {.name = "shmmax", .kind = PROC_SYS_UINT, .values = {0x2000000}, .count = 1},
    {.name = "shmmni", .kind = PROC_SYS_UINT, .values = {4096}, .count = 1},
    {.name = "msgmax", .kind = PROC_SYS_UINT, .values = {8192}, .count = 1},
    {.name = "msgmnb", .kind = PROC_SYS_UINT, .values = {16384}, .count = 1},
    {.name = "msgmni", .kind = PROC_SYS_UINT, .values = {32000}, .count = 1},
    {.name = "sem", .kind = PROC_SYS_MULTI, .values = {250, 32000, 32, 128}, .count = 4},
    {.name = "printk", .kind = PROC_SYS_MULTI, .values = {7, 4, 1, 7}, .readonly = 1, .count = 4},
};

#define PROCFS_SYSCTL_KERNEL_COUNT (sizeof(procfs_sysctl_kernel) / sizeof(procfs_sysctl_kernel[0]))

#define PROC_SYS_KERNEL 0

/* Helpers */

static void procfs_dummy(void)
{
}

/*
 * procfs directory nodes are namespace objects, not disposable directory
 * snapshots.  Open file descriptions retain pointers to them, so rebuilding a
 * directory by freeing all children races with read/stat on another CPU.  Keep
 * the bounded PID namespace (PROCESS_TABLE_SIZE) resident and reactivate a
 * node when a PID is reused.
 */
static vfs_node_t procfs_find_child(vfs_node_t parent, const char *name)
{
    if (!parent || !name) return NULL;
    for (clist_t link = parent->child; link; link = link->next) {
        vfs_node_t child = link->data;
        if (child && child->name && streq(child->name, name)) return child;
    }
    return NULL;
}

/* Allocate a procfs file description. */
static procfs_file_t *procfs_file_alloc(procfs_type_t type, pid_t pid, int subtype)
{
    procfs_file_t *pf = calloc(1, sizeof(procfs_file_t));
    if (!pf) return NULL;
    pf->type    = type;
    pf->pid     = pid;
    pf->subtype = subtype;
    return pf;
}

/* Get or create a child node, re-binding its procfs description. */
static vfs_node_t procfs_ensure_child(vfs_node_t parent, const char *name, procfs_type_t type, pid_t pid, int subtype, uint16_t node_type)
{
    vfs_node_t child = procfs_find_child(parent, name);
    if (!child) {
        child = vfs_node_alloc(parent, name);
        if (!child) return NULL;
    }

    procfs_file_t *pf = child->handle;
    if (!pf) {
        pf = procfs_file_alloc(type, pid, subtype);
        if (!pf) return NULL;
        child->handle = pf;
    } else {
        pf->type    = type;
        pf->pid     = pid;
        pf->subtype = subtype;
    }

    child->flags &= ~(VFS_NODE_UNLINKED | VFS_NODE_UNLINKING | VFS_NODE_FINALIZING | VFS_NODE_INITIALIZING);
    child->flags |= VFS_NODE_NOCACHE;
    child->type = node_type;
    return child;
}

/* Hide PID directory nodes whose process has exited. */
static void procfs_deactivate_pid_nodes(vfs_node_t root)
{
    if (!root) return;
    for (clist_t link = root->child; link; link = link->next) {
        vfs_node_t     child = link->data;
        procfs_file_t *pf    = child ? child->handle : NULL;
        if (!pf || pf->type != PROCFS_PID_DIR) continue;
        child->flags |= VFS_NODE_UNLINKED;
        child->type = file_none;
    }
}

/* Content generators */

static void gen_info_stat(procfs_file_t *pf)
{
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;

    size_t cpu_count = get_cpu_count();
    char  *p         = buf;
    int    remaining = PROCFS_BUF_SIZE;
    int    n;

    n = snprintf(p, remaining, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n", 0ULL, 0ULL, scheduler.ticks * cpu_count, 0ULL, 0ULL,
                 0ULL, 0ULL, 0ULL, 0ULL, 0ULL);
    p += n;
    remaining -= n;
    if (remaining > 0) {
        for (uint32_t i = 0; i < cpu_count && remaining > 0; i++) {
            n = snprintf(p, remaining, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n", i, 0ULL, 0ULL, scheduler.ticks, 0ULL, 0ULL,
                         0ULL, 0ULL, 0ULL, 0ULL, 0ULL);
            p += n;
            remaining -= n;
        }
    }
    if (remaining > 0) {
        uint64_t uptime_seconds = timer_monotonic_ns() / TIMER_NSEC_PER_SEC;
        int64_t  realtime       = timer_realtime_ns();
        uint64_t boot_time      = realtime > 0 && (uint64_t)realtime / TIMER_NSEC_PER_SEC >= uptime_seconds ?
                                      (uint64_t)realtime / TIMER_NSEC_PER_SEC - uptime_seconds :
                                      0;
        n = snprintf(p, remaining, "intr %llu\nctxt %llu\nbtime %llu\nprocesses %llu\nprocs_running %u\nprocs_blocked %u\n", 0ULL, 0ULL,
                     boot_time, scheduler.next_pid, cpu_rqs[0].nr_running + 1, 0U);
        p += n;
    }

    pf->content  = buf;
    pf->size     = (size_t)(p - buf);
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_meminfo(procfs_file_t *pf)
{
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;

    size_t            total_kb = (frame_allocator.origin_frames * PAGE_4K_SIZE) / 1024;
    size_t            free_kb  = (frame_allocator.usable_frames * PAGE_4K_SIZE) / 1024;
    pagecache_stats_t cache;
    pagecache_get_stats(&cache);
    size_t       cached_kb    = cache.pages * PAGE_4K_SIZE / 1024;
    size_t       dirty_kb     = cache.dirty * PAGE_4K_SIZE / 1024;
    size_t       writeback_kb = cache.writeback * PAGE_4K_SIZE / 1024;
    size_t       active_kb    = cache.active * PAGE_4K_SIZE / 1024;
    size_t       inactive_kb  = cache.inactive * PAGE_4K_SIZE / 1024;
    size_t       clean_pages  = cache.pages > cache.dirty ? cache.pages - cache.dirty : 0;
    size_t       available_kb = free_kb + clean_pages * PAGE_4K_SIZE / 1024;
    swap_stats_t swap;
    swap_get_stats(&swap);
    int n = snprintf(buf, PROCFS_BUF_SIZE,
                     "MemTotal:       %8zu kB\n"
                     "MemFree:        %8zu kB\n"
                     "MemAvailable:   %8zu kB\n"
                     "Buffers:        %8zu kB\n"
                     "Cached:         %8zu kB\n"
                     "SwapCached:     %8zu kB\n"
                     "Active:         %8zu kB\n"
                     "Inactive:       %8zu kB\n"
                     "SwapTotal:      %8zu kB\n"
                     "SwapFree:       %8zu kB\n"
                     "Dirty:          %8zu kB\n"
                     "Writeback:      %8zu kB\n"
                     "AnonPages:      %8zu kB\n"
                     "Mapped:         %8zu kB\n"
                     "Slab:           %8zu kB\n"
                     "PageTables:     %8zu kB\n"
                     "NFS_Unstable:   %8zu kB\n"
                     "Bounce:         %8zu kB\n"
                     "VmallocTotal:   %8zu kB\n"
                     "VmallocUsed:    %8zu kB\n"
                     "VmallocChunk:   %8zu kB\n",
                     total_kb, free_kb, available_kb, 0UL, cached_kb, 0UL, active_kb, inactive_kb,
                     (size_t)(swap.total_pages * SWAP_PAGE_SIZE / 1024), (size_t)(swap.free_pages * SWAP_PAGE_SIZE / 1024), dirty_kb,
                     writeback_kb, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, (KERNEL_HEAP_SIZE) / 1024, 0UL, (KERNEL_HEAP_SIZE) / 1024);

    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_vmstat(procfs_file_t *pf)
{
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;
    pagecache_stats_t cache;
    pagecache_get_stats(&cache);
    swap_stats_t swap;
    swap_get_stats(&swap);
    int n        = snprintf(buf, PROCFS_BUF_SIZE,
                            "nr_file_pages %llu\n"
                                   "nr_active_file %llu\n"
                                   "nr_inactive_file %llu\n"
                                   "nr_dirty %llu\n"
                                   "nr_writeback %llu\n"
                                   "pgpgin %llu\n"
                                   "pgpgout %llu\n"
                                   "pswpin %llu\n"
                                   "pswpout %llu\n"
                                   "pgactivate %llu\n"
                                   "pgsteal_kswapd %llu\n"
                                   "workingset_refault_file %llu\n"
                                   "workingset_activate_file %llu\n"
                                   "nr_vmscan_write %llu\n",
                            cache.pages, cache.active, cache.inactive, cache.dirty, cache.writeback, cache.reads * 4, cache.writes * 4, swap.pages_in,
                            swap.pages_out, cache.active, cache.reclaimed, cache.misses, cache.hits, cache.writeback_errors);
    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_modules(procfs_file_t *pf)
{
    size_t capacity = (size_t)64 * 1024;
    char  *buffer   = malloc(capacity);
    if (!buffer) return;
    pf->size     = module_format_proc(buffer, capacity);
    pf->content  = buffer;
    pf->capacity = capacity;
}

static void gen_mount_table(procfs_file_t *pf, bool mountinfo)
{
    size_t capacity = (size_t)64 * 1024;
    char  *buffer   = malloc(capacity);
    if (!buffer) return;
    pf->size     = vfs_format_mount_table(buffer, capacity, mountinfo);
    pf->content  = buffer;
    pf->capacity = capacity;
}

static void gen_info_filesystems(procfs_file_t *pf)
{
    size_t capacity = 4096;
    char  *buffer   = malloc(capacity);
    if (!buffer) return;
    pf->size     = vfs_format_filesystems(buffer, capacity);
    pf->content  = buffer;
    pf->capacity = capacity;
}

static void gen_info_cmdline(procfs_file_t *pf)
{
    const char *cmdline = get_cmdline();
    if (!cmdline) cmdline = "";
    size_t length = strlen(cmdline);
    char  *buffer = malloc(length + 2);
    if (!buffer) return;
    memcpy(buffer, cmdline, length);
    buffer[length++] = '\n';
    buffer[length]   = '\0';
    pf->content      = buffer;
    pf->size         = length;
    pf->capacity     = length + 1;
}

static void gen_info_cgroups(procfs_file_t *pf)
{
    char *buffer = malloc(PROCFS_BUF_SIZE);
    if (!buffer) return;
    int length = cgroup_format_proc_cgroups(buffer, PROCFS_BUF_SIZE);
    if (length < 0) length = 0;
    pf->content  = buffer;
    pf->size     = (size_t)length;
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_cpuinfo(procfs_file_t *pf)
{
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;

    size_t cpu_count = get_cpu_count();
    char  *p         = buf;
    int    remaining = PROCFS_BUF_SIZE;
    int    n;

    /* Query CPUID leaf 1 once */
    uint32_t eax1, ebx1, ecx1, edx1;
    cpuid_safe(0x00000001, 0, &eax1, &ebx1, &ecx1, &edx1);

    uint32_t stepping   = eax1 & 0xF;
    uint32_t model      = (eax1 >> 4) & 0xF;
    uint32_t family     = (eax1 >> 8) & 0xF;
    uint32_t ext_model  = (eax1 >> 16) & 0xF;
    uint32_t ext_family = (eax1 >> 20) & 0xFF;

    if (family == 0xF) family += ext_family;
    if (family == 0x6 || family == 0xF) model = (ext_model << 4) | model;

    uint32_t cpuid_level  = eax1;
    uint32_t clflush_size = ((ebx1 >> 24) & 0xFF) * 8;
    uint32_t max_logical  = (ebx1 >> 16) & 0xFF;

    /* Cache info via leaf 4 (deterministic) */
    uint32_t l1d_kb = 0, l2_kb = 0;
    for (uint32_t idx = 0; idx < 8; idx++) {
        uint32_t ca, cb, cc, cd;
        cpuid_safe(0x00000004, idx, &ca, &cb, &cc, &cd);
        uint32_t cache_type = ca & 0x1F;
        if (cache_type == 0) break;
        uint32_t ways       = ((cb >> 22) & 0x3FF) + 1;
        uint32_t partitions = ((cb >> 12) & 0x3FF) + 1;
        uint32_t line_size  = (cb & 0xFFF) + 1;
        uint32_t sets       = cc + 1;
        uint32_t size_kb    = (ways * partitions * line_size * sets) / 1024;
        if (cache_type == 1 || cache_type == 2)
            l1d_kb = size_kb; // data / instruction cache
        else if (cache_type == 3)
            l2_kb = size_kb; // unified L2
    }

    /* CPU frequency */
    uint64_t cpu_hz     = tsc_get_cpu_frequency();
    uint64_t cpu_mhz    = cpu_hz / 1000000;
    uint64_t cpu_mhz_fp = (cpu_hz % 1000000) / 10000; // one decimal

    /* BogoMIPS: approx (cpu_hz / 1000000) / 2, same as Linux on x86 */
    uint64_t bogo    = cpu_hz / 2000000;
    uint64_t bogo_fp = ((cpu_hz % 2000000) * 10) / 2000000;

    for (uint32_t i = 0; i < cpu_count && remaining > 0; i++) {
        char flags_buf[1024];
        cpu_build_flags(flags_buf, sizeof(flags_buf));

        n = snprintf(p, remaining,
                     "processor\t: %u\n"
                     "vendor_id\t: %s\n"
                     "cpu family\t: %u\n"
                     "model\t\t: %u\n"
                     "model name\t: %s\n"
                     "stepping\t: %u\n"
                     "cpu MHz\t\t: %llu.%01llu\n"
                     "cache size\t: %u KB\n"
                     "physical id\t: %u\n"
                     "siblings\t: %u\n"
                     "core id\t\t: %u\n"
                     "cpu cores\t: %u\n"
                     "apicid\t\t: %u\n"
                     "initial apicid\t: %u\n"
                     "fpu\t\t: %s\n"
                     "fpu_exception\t: %s\n"
                     "cpuid level\t: %u\n"
                     "wp\t\t: yes\n"
                     "flags\t\t:%s\n"
                     "bugs\t\t:\n"
                     "bogomips\t: %llu.%01llu\n"
                     "clflush size\t: %u\n"
                     "cache_alignment\t: %u\n"
                     "address sizes\t: %u bits physical, %u bits virtual\n"
                     "power management:\n\n",
                     i, get_vendor_name(), family, model, get_model_name(), stepping, cpu_mhz, cpu_mhz_fp,
                     l2_kb ? l2_kb : (l1d_kb ? l1d_kb : 256U), i, max_logical, 0U, 1U, i, i, (edx1 & (1 << 0)) ? "yes" : "no",
                     (edx1 & (1 << 0)) ? "yes" : "no", cpuid_level, flags_buf, bogo, bogo_fp, clflush_size, clflush_size, get_cpu_phys_bits(),
                     get_cpu_virt_bits());
        p += n;
        remaining -= n;
    }

    pf->content  = buf;
    pf->size     = (size_t)(p - buf);
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_uptime(procfs_file_t *pf)
{
    char *buf = malloc(128);
    if (!buf) return;

    uint64_t ns       = timer_monotonic_ns();
    uint64_t seconds  = ns / 1000000000ULL;
    uint64_t centisec = (ns % 1000000000ULL) / 10000000ULL;
    uint64_t idle     = seconds;

    int n = snprintf(buf, 128, "%llu.%02llu %llu.%02llu\n", seconds, centisec, idle, 0ULL);

    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 128;
}

static void gen_info_version(procfs_file_t *pf)
{
    char *buf = malloc(256);
    if (!buf) return;

    int n = snprintf(buf, 256, "%s version %s (%s version %s) %s %s\n", KERNEL_NAME, KERNEL_VERSION, COMPILER_NAME, COMPILER_VERSION, BUILD_DATE,
                     BUILD_TIME);

    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 256;
}

static void gen_info_loadavg(procfs_file_t *pf)
{
    char *buf = malloc(128);
    if (!buf) return;

    uint64_t running   = 0;
    uint32_t cpu_count = sched_cpu_count();
    for (uint32_t i = 0; i < cpu_count; i++) running += cpu_rqs[i].nr_running;
    /* active threads = currently running (one per CPU) + on ready queues */
    uint64_t active  = cpu_count + running;
    uint64_t total   = scheduler.next_pid ? scheduler.next_pid - 1 : 0;
    uint64_t lastpid = scheduler.next_pid ? scheduler.next_pid - 1 : 0;

    int n = snprintf(buf, 128, "0.00 0.00 0.00 %llu/%llu %llu\n", active, total ? total : 1, lastpid);

    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 128;
}

static void gen_info_interrupts(procfs_file_t *pf)
{
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;

    size_t cpu_count = get_cpu_count();
    char  *p         = buf;
    int    remaining = PROCFS_BUF_SIZE;
    int    n;

    n = snprintf(p, remaining, "           CPU0");
    p += n;
    remaining -= n;
    for (uint32_t i = 1; i < cpu_count && remaining > 0; i++) {
        n = snprintf(p, remaining, "       CPU%u", i);
        p += n;
        remaining -= n;
    }
    n = snprintf(p, remaining, "\n");
    p += n;
    remaining -= n;

    static const struct {
            const char *name;
            uint64_t    count;
    } vectors[] = {
        {"IO-APIC   2-edge      timer",          0},
        {"IO-APIC   8-edge      rtc0",           0},
        {"IO-APIC   9-fasteoi   acpi",           0},
        {"IO-APIC  16-fasteoi   virtio0",        0},
        {"PCI-MSI 327680-edge      virtio1-pci", 0},
    };
    int vector = 0;
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]) && remaining > 0; i++) {
        n = snprintf(p, remaining, " %2d: ", vector);
        p += n;
        remaining -= n;
        for (uint32_t c = 0; c < cpu_count && remaining > 0; c++) {
            n = snprintf(p, remaining, "%10llu ", (unsigned long long)(c == 0 ? vectors[i].count : 0));
            p += n;
            remaining -= n;
        }
        n = snprintf(p, remaining, "  %s\n", vectors[i].name);
        p += n;
        remaining -= n;
        vector++;
    }
    n = snprintf(p, remaining, "NMI:          0          0   Non-maskable interrupts\n");
    p += n;
    remaining -= n;
    n = snprintf(p, remaining, "LOC:   %10llu   %10llu   Local timer interrupts\n", (unsigned long long)scheduler.ticks, 0ULL);
    p += n;
    remaining -= n;
    n = snprintf(p, remaining, "SPU:          0          0   Spurious interrupts\n");
    p += n;

    pf->content  = buf;
    pf->size     = (size_t)(p - buf);
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_softirqs(procfs_file_t *pf)
{
    static const char *names[] = {"HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "IRQ_POLL", "TASKLET", "SCHED", "HRTIMER", "RCU"};
    char              *buf     = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;

    size_t cpu_count = get_cpu_count();
    char  *p         = buf;
    int    remaining = PROCFS_BUF_SIZE;
    int    n;

    n = snprintf(p, remaining, "                    CPU0");
    p += n;
    remaining -= n;
    for (uint32_t i = 1; i < cpu_count && remaining > 0; i++) {
        n = snprintf(p, remaining, "       CPU%u", i);
        p += n;
        remaining -= n;
    }
    n = snprintf(p, remaining, "\n");
    p += n;
    remaining -= n;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && remaining > 0; i++) {
        n = snprintf(p, remaining, " %8s:", names[i]);
        p += n;
        remaining -= n;
        for (uint32_t c = 0; c < cpu_count && remaining > 0; c++) {
            n = snprintf(p, remaining, "%10llu", 0ULL);
            p += n;
            remaining -= n;
        }
        n = snprintf(p, remaining, "\n");
        p += n;
        remaining -= n;
    }

    pf->content  = buf;
    pf->size     = (size_t)(p - buf);
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_partitions(procfs_file_t *pf)
{
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;
    int length = devtmpfs_format_proc_partitions(buf, PROCFS_BUF_SIZE);
    if (length < 0) length = 0;
    pf->content  = buf;
    pf->size     = (size_t)length;
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_devices(procfs_file_t *pf)
{
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;
    int length = devtmpfs_format_proc_devices(buf, PROCFS_BUF_SIZE);
    if (length < 0) length = 0;
    pf->content  = buf;
    pf->size     = (size_t)length;
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_diskstats(procfs_file_t *pf)
{
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;
    int length = devtmpfs_format_proc_diskstats(buf, PROCFS_BUF_SIZE);
    if (length < 0) length = 0;
    pf->content  = buf;
    pf->size     = (size_t)length;
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_swaps(procfs_file_t *pf)
{
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;
    int length = swap_format_proc_swaps(buf, PROCFS_BUF_SIZE);
    if (length < 0) length = 0;
    pf->content  = buf;
    pf->size     = (size_t)length;
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_misc(procfs_file_t *pf)
{
    /* This kernel has no Linux "misc" (major 10) devices registered. */
    char *buf = malloc(8);
    if (!buf) return;
    buf[0]       = '\0';
    pf->content  = buf;
    pf->size     = 0;
    pf->capacity = 8;
}

static void gen_info_ioports(procfs_file_t *pf)
{
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;
    int n        = snprintf(buf, PROCFS_BUF_SIZE,
                            "0000-0cf7 PCI Bus 0000:00\n"
                                   "0000-001f dma1\n"
                                   "0020-0021 pic1\n"
                                   "0040-0043 timer0\n"
                                   "0050-0053 timer1\n"
                                   "0060-0060 keyboard\n"
                                   "0064-0064 keyboard\n"
                                   "0070-0077 rtc0\n"
                                   "0080-008f dma page reg\n"
                                   "00a0-00a1 pic2\n"
                                   "00c0-00df dma2\n"
                                   "00f0-00ff fpu\n"
                                   "0170-0177 ide1\n"
                                   "01f0-01f7 ide0\n"
                                   "0378-037f parport0\n"
                                   "03f8-03ff serial\n"
                                   "0cf8-0cff PCI conf1\n"
                                   "0220-022f sound\n"
                                   "0d0000-0dffff PCI Bus 0000:00\n");
    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_info_iomem(procfs_file_t *pf)
{
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;
    int n        = snprintf(buf, PROCFS_BUF_SIZE,
                            "00000000-00000fff : reserved\n"
                                   "00001000-0009fbff : System RAM\n"
                                   "0009fc00-0009ffff : reserved\n"
                                   "000a0000-000bffff : PCI Bus 0000:00\n"
                                   "000c0000-000cffff : Video ROM\n"
                                   "000e0000-000fffff : reserved\n"
                                   "00100000-7fedffff : System RAM\n"
                                   "fe000000-fedfffff : PCI Bus 0000:00\n"
                                   "fffc0000-ffffffff : reserved\n");
    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_tty_drivers(procfs_file_t *pf)
{
    char *buf = malloc(1024);
    if (!buf) return;
    int n        = snprintf(buf, 1024,
                            "/dev/tty             /dev/tty        5       0 system:/dev/tty\n"
                                   "/dev/console         /dev/console    5       1 system:console\n"
                                   "/dev/ptmx            /dev/ptmx       5       2 system\n"
                                   "/dev/vc/0            /dev/tty0       4       0 system:vtmaster\n"
                                   "pty_slave            /dev/pts        136     0-1048575 pty:slave\n"
                                   "serial               /dev/ttyS       4       64-127 serial\n"
                                   "virtual              /dev/tty        4       0-63 virtual\n");
    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 1024;
}

static void gen_tty_ldiscs(procfs_file_t *pf)
{
    char *buf = malloc(256);
    if (!buf) return;
    int n        = snprintf(buf, 256, "n_console\t2\nn_ptmx\t\t3\nn_tty\t\t0\n");
    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 256;
}

/* /proc/sys sysctl support */

static procfs_sysctl_t *procfs_sysctl_lookup(int dir, size_t index)
{
    if (dir == PROC_SYS_KERNEL && index < PROCFS_SYSCTL_KERNEL_COUNT) return &procfs_sysctl_kernel[index];
    return NULL;
}

static procfs_sysctl_t *procfs_sysctl_find(int dir, const char *name)
{
    if (dir == PROC_SYS_KERNEL) {
        for (size_t i = 0; i < PROCFS_SYSCTL_KERNEL_COUNT; i++)
            if (streq(procfs_sysctl_kernel[i].name, name)) return &procfs_sysctl_kernel[i];
    }
    return NULL;
}

static void gen_sysctl_file(procfs_file_t *pf)
{
    procfs_sysctl_t *sc = procfs_sysctl_lookup(pf->subtype, (size_t)pf->pid);
    if (!sc) return;
    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;

    int n = 0;
    if (sc->kind == PROC_SYS_STR) {
        n = snprintf(buf, PROCFS_BUF_SIZE, "%s\n", sc->string);
    } else if (sc->kind == PROC_SYS_UINT) {
        n = snprintf(buf, PROCFS_BUF_SIZE, "%llu\n", (unsigned long long)sc->values[0]);
    } else {
        int off = 0;
        for (uint8_t i = 0; i < sc->count; i++) {
            n = snprintf(buf + off, PROCFS_BUF_SIZE - off, "%s%llu", i ? " " : "", (unsigned long long)sc->values[i]);
            if (n < 0 || off + (size_t)n >= PROCFS_BUF_SIZE) {
                off = PROCFS_BUF_SIZE;
                break;
            }
            off += n;
        }
        n = snprintf(buf + off, PROCFS_BUF_SIZE - off, "\n");
        off += n;
        n = off;
    }
    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = PROCFS_BUF_SIZE;
}

static int procfs_sysctl_apply(procfs_sysctl_t *sc, const char *data, size_t size)
{
    if (sc->readonly) return -EPERM;

    if (sc->kind == PROC_SYS_STR) {
        size_t len = size;
        while (len && (data[len - 1] == '\n' || data[len - 1] == '\r' || data[len - 1] == ' ' || data[len - 1] == '\t')) len--;
        if (!len || memchr(data, '\0', len) || len >= sizeof(sc->string)) return -EINVAL;
        memcpy(sc->string, data, len);
        sc->string[len] = '\0';
        return EOK;
    }

    const char *cursor    = data;
    size_t      left      = size;
    uint64_t    values[4] = {0};
    size_t      count     = 0;
    bool        any       = false;

    while (left) {
        while (left && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')) {
            cursor++;
            left--;
        }
        if (!left) break;
        uint64_t value = 0;
        bool     valid = false;
        while (left && *cursor >= '0' && *cursor <= '9') {
            valid          = true;
            uint64_t digit = (uint64_t)(*cursor - '0');
            if (value > (UINT64_MAX - digit) / 10) return -ERANGE;
            value = value * 10 + digit;
            cursor++;
            left--;
        }
        while (left && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')) {
            cursor++;
            left--;
        }
        if (!valid) return -EINVAL;
        if (count >= sizeof(values) / sizeof(values[0])) return -EINVAL;
        values[count++] = value;
        any             = true;
    }
    if (!any) return -EINVAL;

    if (sc->kind == PROC_SYS_UINT) {
        if (count != 1) return -EINVAL;
        sc->values[0] = values[0];
        return EOK;
    }
    if (sc->kind == PROC_SYS_MULTI) {
        if (count != sc->count) return -EINVAL;
        for (uint8_t i = 0; i < sc->count; i++) sc->values[i] = values[i];
        return EOK;
    }
    return -EINVAL;
}

typedef struct procfs_net_context {
        char  *buf;
        size_t length;
        size_t capacity;
} procfs_net_context_t;

static void procfs_gen_net_dev(net_device_t *device, void *opaque)
{
    procfs_net_context_t *context = opaque;
    netdev_stats_t        stats;
    char                  name[NETDEV_NAME_MAX];
    if (context->length >= context->capacity) return;
    spin_lock(&device->lock);
    strncpy(name, device->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    spin_unlock(&device->lock);
    netdev_get_stats(device, &stats);
    int n = snprintf(context->buf + context->length, context->capacity - context->length,
                     "%6s: %llu %llu %llu %llu 0 0 0 0 %llu %llu %llu %llu 0 0 0 0\n", name, (unsigned long long)stats.rx_bytes,
                     (unsigned long long)stats.rx_packets, (unsigned long long)stats.rx_errors, (unsigned long long)stats.rx_dropped,
                     (unsigned long long)stats.tx_bytes, (unsigned long long)stats.tx_packets, (unsigned long long)stats.tx_errors,
                     (unsigned long long)stats.tx_dropped);
    if (n > 0) context->length += (size_t)n < context->capacity - context->length ? (size_t)n : context->capacity - context->length;
}

static void procfs_gen_net_route(net_device_t *device, void *opaque)
{
    procfs_net_context_t *context = opaque;
    char                  name[NETDEV_NAME_MAX];
    uint32_t              address, netmask, gateway, mtu, flags;
    spin_lock(&device->lock);
    strncpy(name, device->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    address                = device->ipv4_address;
    netmask                = device->ipv4_netmask;
    gateway                = device->ipv4_gateway;
    mtu                    = device->mtu;
    flags                  = device->flags;
    spin_unlock(&device->lock);
    if (!address || !netmask || !(flags & NETDEV_F_UP) || context->length >= context->capacity) return;
    int n = snprintf(context->buf + context->length, context->capacity - context->length, "%s\t%08X\t00000000\t0001\t0\t0\t0\t%08X\t%u\t0\t0\n",
                     name, __builtin_bswap32(address & netmask), __builtin_bswap32(netmask), mtu);
    if (n > 0) context->length += (size_t)n < context->capacity - context->length ? (size_t)n : context->capacity - context->length;
    if (!gateway || context->length >= context->capacity) return;
    n = snprintf(context->buf + context->length, context->capacity - context->length, "%s\t00000000\t%08X\t0003\t0\t0\t0\t00000000\t%u\t0\t0\n",
                 name, __builtin_bswap32(gateway), mtu);
    if (n > 0) context->length += (size_t)n < context->capacity - context->length ? (size_t)n : context->capacity - context->length;
}

static void gen_net_file(procfs_file_t *pf)
{
    static const char *headers[] = {
        "Inter-|   Receive                                                |  Transmit\n face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n",
        "IP address       HW type     Flags       HW address            Mask     Device\n",
        "Iface\tDestination Gateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n",
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n",
        "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n",
        "Num       RefCount Protocol Flags    Type St Inode Path\n",
    };
    char *buf = calloc(1, PROCFS_BUF_SIZE);
    if (!buf) return;
    if (pf->subtype == PROC_NET_UNIX) {
        pf->size     = socket_format_unix_table(buf, PROCFS_BUF_SIZE);
        pf->content  = buf;
        pf->capacity = PROCFS_BUF_SIZE;
        return;
    }
    if (pf->subtype == PROC_NET_DEV || pf->subtype == PROC_NET_ROUTE) {
        procfs_net_context_t context = {.buf = buf, .capacity = PROCFS_BUF_SIZE};
        context.length               = strlen(headers[pf->subtype]);
        memcpy(buf, headers[pf->subtype], context.length);
        netdev_iterate(pf->subtype == PROC_NET_DEV ? procfs_gen_net_dev : procfs_gen_net_route, &context);
        pf->content  = buf;
        pf->size     = context.length;
        pf->capacity = PROCFS_BUF_SIZE;
        return;
    }
    enum inet_proc_file file = (enum inet_proc_file)pf->subtype;
    size_t              n    = inet_backend_proc_read(file, buf, PROCFS_BUF_SIZE);
    if (!n) {
        n = strlen(headers[pf->subtype]);
        memcpy(buf, headers[pf->subtype], n);
    }
    if (n > PROCFS_BUF_SIZE) n = PROCFS_BUF_SIZE;
    pf->content  = buf;
    pf->size     = n;
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_pid_status(procfs_file_t *pf)
{
    process_t *proc = process_find(pf->pid);
    if (!proc) return;

    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;

    const char *state_str;
    switch (proc->task->state) {
        case TASK_READY :
        case TASK_RUNNING :
            state_str = "R (running)";
            break;
        case TASK_BLOCKED :
        case TASK_SLEEPING :
            state_str = "S (sleeping)";
            break;
        case TASK_STOPPED :
            state_str = "T (stopped)";
            break;
        case TASK_ZOMBIE :
            state_str = "Z (zombie)";
            break;
        case TASK_IDLE :
            state_str = "I (idle)";
            break;
        default :
            state_str = "? (unknown)";
            break;
    }

    uint64_t   vmsize = 0, vmrss = 0, vmdata = 0, vmstack = 0;
    vm_area_t *vma = proc->mmap_list;
    while (vma) {
        vmsize += vma->end - vma->start;
        if (vma->type == VM_REGION_DATA) vmdata += vma->end - vma->start;
        if (vma->type == VM_REGION_STACK) vmstack += vma->end - vma->start;
        if (vma->flags & VM_WRITE) vmrss += vma->end - vma->start;
        vma = vma->next;
    }

    pid_t ppid = proc->parent ? proc->parent->task->pid : 0;

    int n = snprintf(buf, PROCFS_BUF_SIZE,
                     "Name:\t%s\n"
                     "State:\t%s\n"
                     "Tgid:\t%llu\n"
                     "Pid:\t%llu\n"
                     "PPid:\t%llu\n"
                     "TracerPid:\t%llu\n"
                     "Uid:\t%u\t%u\t%u\t%u\n"
                     "Gid:\t%u\t%u\t%u\t%u\n"
                     "FDSize:\t%u\n"
                     "Groups:\t%u\n"
                     "VmSize:\t%8llu kB\n"
                     "VmRSS:\t%8llu kB\n"
                     "VmData:\t%8llu kB\n"
                     "VmStk:\t%8llu kB\n"
                     "VmExe:\t0 kB\n"
                     "VmLib:\t0 kB\n"
                     "VmPTE:\t0 kB\n"
                     "Threads:\t1\n"
                     "SigQ:\t0/0\n"
                     "CapInh:\t0000000000000000\n"
                     "CapPrm:\t0000000000000000\n"
                     "CapEff:\t0000000000000000\n"
                     "CapBnd:\t0000000000000000\n"
                     "Cpus_allowed:\t1\n"
                     "Cpus_allowed_list:\t0\n"
                     "Mems_allowed:\t1\n"
                     "Mems_allowed_list:\t0\n"
                     "voluntary_ctxt_switches:\t0\n"
                     "nonvoluntary_ctxt_switches:\t0\n",
                     proc->task->name, state_str, (uint64_t)pf->pid, (uint64_t)pf->pid, (uint64_t)ppid, (uint64_t)ptrace_tracer_pid(proc->task),
                     proc->uid, proc->uid, proc->uid, proc->fsuid, proc->gid, proc->gid, proc->gid, proc->fsgid, 0U, 0U, vmsize / 1024,
                     vmrss / 1024, vmdata / 1024, vmstack / 1024);

    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_pid_maps(procfs_file_t *pf)
{
    process_t *proc = process_find(pf->pid);
    if (!proc) return;

    char *buf = malloc(PROCFS_BUF_SIZE);
    if (!buf) return;

    char *p         = buf;
    int   remaining = PROCFS_BUF_SIZE;
    int   n;

    spin_lock(&proc->mmap_lock);
    vm_area_t *vma = proc->mmap_list;
    while (vma && remaining > 0) {
        const char *perm = "---";
        switch (vma->flags & (VM_READ | VM_WRITE | VM_EXEC)) {
            case VM_READ :
                perm = "r--";
                break;
            case VM_WRITE :
                perm = "-w-";
                break;
            case VM_EXEC :
                perm = "--x";
                break;
            case VM_READ | VM_WRITE :
                perm = "rw-";
                break;
            case VM_READ | VM_EXEC :
                perm = "r-x";
                break;
            case VM_WRITE | VM_EXEC :
                perm = "-wx";
                break;
            case VM_READ | VM_WRITE | VM_EXEC :
                perm = "rwx";
                break;
            default :
                break;
        }

        const char *region_name;
        switch (vma->type) {
            case VM_REGION_CODE :
                region_name = "  [code]";
                break;
            case VM_REGION_DATA :
                region_name = "  [data]";
                break;
            case VM_REGION_HEAP :
                region_name = "  [heap]";
                break;
            case VM_REGION_STACK :
                region_name = "  [stack]";
                break;
            case VM_REGION_VDSO :
                region_name = "  [vdso]";
                break;
            default :
                region_name = "";
                break;
        }

        n = snprintf(p, remaining, "%016lx-%016lx %s%c %08lx 00:00 0%s\n", vma->start, vma->end, perm, (vma->flags & VM_SHARED) ? 's' : 'p', 0UL,
                     region_name);
        p += n;
        remaining -= n;
        vma = vma->next;
    }
    spin_unlock(&proc->mmap_lock);

    pf->content  = buf;
    pf->size     = (size_t)(p - buf);
    pf->capacity = PROCFS_BUF_SIZE;
}

static void gen_pid_cmdline(procfs_file_t *pf)
{
    process_t *proc = process_find(pf->pid);
    if (!proc) return;

    size_t len = strlen(proc->task->name);
    char  *buf = malloc(len + 1);
    if (!buf) return;

    memcpy(buf, proc->task->name, len);
    buf[len]     = '\0';
    pf->content  = buf;
    pf->size     = len;
    pf->capacity = len + 1;
}

static void gen_pid_cgroup(procfs_file_t *pf)
{
    process_t *proc = process_find_get(pf->pid);
    if (!proc || !proc->task) {
        process_put(proc);
        return;
    }
    cgroup_t *cgroup = cgroup_get(proc->task->cgroup);
    process_put(proc);
    if (!cgroup) return;

    char path[VFS_PATH_MAX];
    int  path_length = cgroup_format_path(cgroup, path, sizeof(path));
    cgroup_put(cgroup);
    if (path_length < 0) return;

    size_t capacity = (size_t)path_length + 6;
    char  *buffer   = malloc(capacity);
    if (!buffer) return;
    int length   = snprintf(buffer, capacity, "0::%s\n", path);
    pf->content  = buffer;
    pf->size     = length < 0 ? 0 : (size_t)length;
    pf->capacity = capacity;
}

static void gen_pid_name(procfs_file_t *pf)
{
    process_t *proc = process_find(pf->pid);
    if (!proc) return;

    char *buf = malloc(PROCESS_NAME_LEN + 2);
    if (!buf) return;

    size_t len = strlen(proc->task->name);
    memcpy(buf, proc->task->name, len);
    buf[len]     = '\n';
    buf[len + 1] = '\0';

    pf->content  = buf;
    pf->size     = len + 1;
    pf->capacity = PROCESS_NAME_LEN + 2;
}

static void gen_pid_comm(procfs_file_t *pf)
{
    process_t *proc = process_find(pf->pid);
    if (!proc) return;

    char *buf = malloc(PROCESS_NAME_LEN + 2);
    if (!buf) return;

    size_t len = strlen(proc->task->name);
    memcpy(buf, proc->task->name, len);
    buf[len]     = '\n';
    buf[len + 1] = '\0';

    pf->content  = buf;
    pf->size     = len + 1;
    pf->capacity = PROCESS_NAME_LEN + 2;
}

static void gen_pid_statm(procfs_file_t *pf)
{
    process_t *proc = process_find_get(pf->pid);
    if (!proc) return;
    char *buf = malloc(256);
    if (!buf) {
        process_put(proc);
        return;
    }

    uint64_t   size = 0, resident = 0, shared = 0, text = 0, lib = 0, data = 0;
    vm_area_t *vma;
    spin_lock(&proc->mmap_lock);
    for (vma = proc->mmap_list; vma; vma = vma->next) {
        uint64_t pages = (vma->end - vma->start) / PAGE_4K_SIZE;
        size += pages;
        resident += pages;
        if (vma->type == VM_REGION_CODE) text += pages;
        if (vma->type == VM_REGION_DATA || vma->type == VM_REGION_HEAP) data += pages;
        if (vma->flags & VM_SHARED) shared += pages;
    }
    spin_unlock(&proc->mmap_lock);

    int n = snprintf(buf, 256, "%llu %llu %llu %llu %llu %llu %llu\n", (unsigned long long)size, (unsigned long long)resident,
                     (unsigned long long)shared, (unsigned long long)text, (unsigned long long)lib, (unsigned long long)data, 0ULL);
    process_put(proc);

    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 256;
}

static void gen_pid_limits(procfs_file_t *pf)
{
    static const char *const names[PROCESS_RLIMIT_COUNT] = {
        "Max cpu time",      "Max file size",     "Max data size",         "Max stack size",       "Max core file size", "Max resident set",
        "Max processes",     "Max open files",    "Max locked memory",     "Max address space",    "Max file locks",     "Max pending signals",
        "Max msgqueue size", "Max nice priority", "Max realtime priority", "Max realtime timeout",
    };
    static const bool is_bytes[PROCESS_RLIMIT_COUNT] = {
        false, true, true, true, true, true, false, false, true, true, false, false, true, false, false, true,
    };
    process_t *proc = process_find_get(pf->pid);
    if (!proc) return;
    char *buf = malloc(4096);
    if (!buf) {
        process_put(proc);
        return;
    }

    int off = 0;
    spin_lock(&proc->rlimit_lock);
    for (int i = 0; i < PROCESS_RLIMIT_COUNT && off < 4096; i++) {
        uint64_t cur = proc->rlimits[i].current;
        uint64_t max = proc->rlimits[i].maximum;
        char     curbuf[24];
        char     maxbuf[24];
        if (cur == PROCESS_RLIM_INFINITY)
            strcpy(curbuf, "unlimited");
        else
            (void)snprintf(curbuf, sizeof(curbuf), "%llu", (unsigned long long)cur);
        if (max == PROCESS_RLIM_INFINITY)
            strcpy(maxbuf, "unlimited");
        else
            (void)snprintf(maxbuf, sizeof(maxbuf), "%llu", (unsigned long long)max);
        int n = snprintf(buf + off, 4096 - off, "%-25s %-16s %-16s %s\n", names[i], curbuf, maxbuf, is_bytes[i] ? "bytes" : "");
        if (n < 0 || off + (size_t)n >= 4096) break;
        off += n;
    }
    spin_unlock(&proc->rlimit_lock);
    process_put(proc);

    pf->content  = buf;
    pf->size     = (size_t)off;
    pf->capacity = 4096;
}

static void gen_pid_io(procfs_file_t *pf)
{
    (void)pf;
    char *buf = malloc(256);
    if (!buf) return;
    int n        = snprintf(buf, 256,
                            "rchar: 0\n"
                                   "wchar: 0\n"
                                   "syscr: 0\n"
                                   "syscw: 0\n"
                                   "read_bytes: 0\n"
                                   "write_bytes: 0\n"
                                   "cancelled_write_bytes: 0\n");
    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 256;
}

static void gen_pid_oom_score_adj(procfs_file_t *pf)
{
    char *buf = malloc(16);
    if (!buf) return;
    int n        = snprintf(buf, 16, "0\n");
    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 16;
}

/* Resolve the Linux /proc/<pid>/fd/<n> target for an open file description. */
static void procfs_fd_target(process_t *proc, int fd, char *target, size_t capacity)
{
    process_file_t *file;

    if (!proc || !target || capacity < 2) return;
    target[0] = '\0';
    file      = process_fd_get(proc, fd);
    if (!file) return;

    if (file->node) {
        if (!file->node->parent) {
            /* Anonymous inodes (pipes, sockets, eventfds, ...). */
            if (file->node->name && file->node->name[0]) {
                (void)snprintf(target, capacity, "anon_inode:%s", file->node->name);
            } else {
                (void)snprintf(target, capacity, "anon_inode:[%llu]", (unsigned long long)file->node->inode);
            }
        } else if (vfs_node_path(file->node, target, capacity) != EOK) {
            target[0] = '\0';
        }
    }
    process_file_put(file);
}

static void gen_pid_mem(procfs_file_t *pf)
{
    process_t *proc = process_find(pf->pid);
    if (!proc) return;

    char *buf = malloc(256);
    if (!buf) return;

    uint64_t   total = 0;
    vm_area_t *vma   = proc->mmap_list;
    while (vma) {
        total += vma->end - vma->start;
        vma = vma->next;
    }

    int n = snprintf(buf, 256,
                     "VmaTotal:\t%llu kB\n"
                     "RssTotal:\t%llu kB\n"
                     "HeapBrk:\t%016lx\n"
                     "StackBrk:\t%016lx\n",
                     total / 1024, total / 1024, proc->heap_brk, proc->stack_brk);

    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 256;
}

static void gen_pid_stat(procfs_file_t *pf)
{
    process_t *proc = process_find_get(pf->pid);
    if (!proc || !proc->task) {
        process_put(proc);
        return;
    }

    char *buf = malloc(1024);
    if (!buf) {
        process_put(proc);
        return;
    }

    char state_char = '?';
    switch (proc->task->state) {
        case TASK_READY :
        case TASK_RUNNING :
            state_char = 'R';
            break;
        case TASK_BLOCKED :
        case TASK_SLEEPING :
            state_char = 'S';
            break;
        case TASK_STOPPED :
            state_char = 'T';
            break;
        case TASK_ZOMBIE :
            state_char = 'Z';
            break;
        case TASK_IDLE :
            state_char = 'I';
            break;
    }

    char     name[PROCESS_NAME_LEN];
    uint32_t cpu_id       = proc->task->cpu_id;
    uint32_t thread_count = proc->thread_count ? proc->thread_count : 1;
    pid_t    ppid         = proc->parent && proc->parent->task ? (pid_t)proc->parent->task->tgid : 0;
    pid_t    pgid         = proc->pgid;
    pid_t    sid          = proc->sid;
    int64_t  tty_nr       = 0;
    int64_t  tpgid        = -1;
    int64_t  exit_code    = proc->task->state == TASK_ZOMBIE ? proc->exit_code : 0;
    memcpy(name, proc->task->name, sizeof(name));
    name[sizeof(name) - 1] = '\0';

    tty_core_t *tty = process_ctty_get(proc);
    if (tty) {
        spin_lock(&tty->lock);
        tpgid = tty->foreground_pgid;
        spin_unlock(&tty->lock);
        /* Linux virtual consoles use major 4; this kernel exposes tty1. */
        tty_nr = (4 << 8) | 1;
        tty_core_release(tty);
    }

    uint64_t vsize = 0, start_code = 0, end_code = 0, start_data = 0, end_data = 0, start_brk = 0;
    spin_lock(&proc->mmap_lock);
    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
        vsize += vma->end - vma->start;
        if (vma->type == VM_REGION_CODE) {
            if (!start_code || vma->start < start_code) start_code = vma->start;
            if (vma->end > end_code) end_code = vma->end;
        } else if (vma->type == VM_REGION_DATA) {
            if (!start_data || vma->start < start_data) start_data = vma->start;
            if (vma->end > end_data) end_data = vma->end;
        } else if (vma->type == VM_REGION_HEAP && !start_brk) {
            start_brk = vma->start;
        }
    }
    spin_unlock(&proc->mmap_lock);

    uint64_t rss_limit;
    spin_lock(&proc->rlimit_lock);
    rss_limit = proc->rlimits[PROCESS_RLIMIT_RSS].current;
    spin_unlock(&proc->rlimit_lock);

    /*
     * Keep all Linux proc_pid_stat fields in their ABI positions.  Unknown
     * accounting values are zero rather than omitted; parsers such as
     * BusyBox ps expect fields through exit_code (52).
     */
    int n = snprintf(buf, 1024,
                     "%lld (%s) %c "
                     "%lld %lld %lld %lld %lld %u "
                     "%llu %llu %llu %llu %llu %llu "
                     "%lld %lld %lld %lld %lld %lld "
                     "%llu %llu %lld %llu "
                     "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu "
                     "%lld %lld %u %u %llu %llu %lld "
                     "%llu %llu %llu %llu %llu %llu %llu %lld\n",
                     (int64_t)pf->pid, name, state_char, (int64_t)ppid, (int64_t)pgid, (int64_t)sid, tty_nr, tpgid, 0U, 0ULL, 0ULL, 0ULL, 0ULL,
                     0ULL, 0ULL, 0LL, 0LL, 20LL, 0LL, (int64_t)thread_count, 0LL, 0ULL, vsize, 0LL, rss_limit, start_code, end_code, 0ULL, 0ULL,
                     0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, (int64_t)SIGCHLD, (int64_t)cpu_id, 0U, 0U, 0ULL, 0ULL, 0LL, start_data,
                     end_data, start_brk, 0ULL, 0ULL, 0ULL, 0ULL, exit_code);
    process_put(proc);

    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 1024;
}

/* Dispatch content generation to the file's type handler. */
static void procfs_gen_content(procfs_file_t *pf, vfs_node_t node)
{
    if (pf->content) return;

    switch (pf->type) {
        case PROCFS_INFO_FILE :
            switch (pf->subtype) {
                case PROC_INFO_STAT :
                    gen_info_stat(pf);
                    break;
                case PROC_INFO_MEMINFO :
                    gen_info_meminfo(pf);
                    break;
                case PROC_INFO_CPUINFO :
                    gen_info_cpuinfo(pf);
                    break;
                case PROC_INFO_UPTIME :
                    gen_info_uptime(pf);
                    break;
                case PROC_INFO_VERSION :
                    gen_info_version(pf);
                    break;
                case PROC_INFO_LOADAVG :
                    gen_info_loadavg(pf);
                    break;
                case PROC_INFO_VMSTAT :
                    gen_info_vmstat(pf);
                    break;
                case PROC_INFO_MODULES :
                    gen_info_modules(pf);
                    break;
                case PROC_INFO_MOUNTS :
                    gen_mount_table(pf, false);
                    break;
                case PROC_INFO_MOUNTINFO :
                    gen_mount_table(pf, true);
                    break;
                case PROC_INFO_FILESYSTEMS :
                    gen_info_filesystems(pf);
                    break;
                case PROC_INFO_CMDLINE :
                    gen_info_cmdline(pf);
                    break;
                case PROC_INFO_CGROUPS :
                    gen_info_cgroups(pf);
                    break;
                case PROC_INFO_INTERRUPTS :
                    gen_info_interrupts(pf);
                    break;
                case PROC_INFO_PARTITIONS :
                    gen_info_partitions(pf);
                    break;
                case PROC_INFO_DEVICES :
                    gen_info_devices(pf);
                    break;
                case PROC_INFO_DISKSTATS :
                    gen_info_diskstats(pf);
                    break;
                case PROC_INFO_SWAPS :
                    gen_info_swaps(pf);
                    break;
                case PROC_INFO_MISC :
                    gen_info_misc(pf);
                    break;
                case PROC_INFO_SOFTIRQS :
                    gen_info_softirqs(pf);
                    break;
                case PROC_INFO_IOPORTS :
                    gen_info_ioports(pf);
                    break;
                case PROC_INFO_IOMEM :
                    gen_info_iomem(pf);
                    break;
                default :
                    break;
            }
            break;
        case PROCFS_PID_FILE :
            switch (pf->subtype) {
                case PROC_PID_STATUS :
                    gen_pid_status(pf);
                    break;
                case PROC_PID_MAPS :
                    gen_pid_maps(pf);
                    break;
                case PROC_PID_CMDLINE :
                    gen_pid_cmdline(pf);
                    break;
                case PROC_PID_NAME :
                    gen_pid_name(pf);
                    break;
                case PROC_PID_STAT :
                    gen_pid_stat(pf);
                    break;
                case PROC_PID_MEM :
                    gen_pid_mem(pf);
                    break;
                case PROC_PID_MOUNTS :
                    gen_mount_table(pf, false);
                    break;
                case PROC_PID_MOUNTINFO :
                    gen_mount_table(pf, true);
                    break;
                case PROC_PID_CGROUP :
                    gen_pid_cgroup(pf);
                    break;
                case PROC_PID_COMM :
                    gen_pid_comm(pf);
                    break;
                case PROC_PID_STATM :
                    gen_pid_statm(pf);
                    break;
                case PROC_PID_LIMITS :
                    gen_pid_limits(pf);
                    break;
                case PROC_PID_IO :
                    gen_pid_io(pf);
                    break;
                case PROC_PID_OOM_SCORE_ADJ :
                    gen_pid_oom_score_adj(pf);
                    break;
                default :
                    break;
            }
            break;
        case PROCFS_NET_FILE :
            gen_net_file(pf);
            break;
        case PROCFS_SYS_FILE :
            gen_sysctl_file(pf);
            break;
        case PROCFS_TTY_FILE :
            if (pf->subtype == 0)
                gen_tty_drivers(pf);
            else
                gen_tty_ldiscs(pf);
            break;
        default :
            break;
    }

    if (node && pf->content && pf->size > 0) node->size = pf->size;
}

/* VFS callbacks */

static int procfs_mount(const char *handle, vfs_node_t node)
{
    /*
     * proc is nodev; Linux accepts a conventional source such as "proc" and
     * does not interpret it as a backing device.
     */
    (void)handle;
    if (!node) return -EINVAL;

    node->fsid = procfs_id;
    node->flags |= VFS_NODE_NOCACHE;

    procfs_file_t *root = procfs_file_alloc(PROCFS_ROOT, 0, 0);
    if (!root) return -ENOMEM;

    node->handle = root;
    node->type   = file_dir;
    return EOK;
}

/* Free the root procfs description on unmount. */
static void procfs_umount(void *root)
{
    procfs_file_t *pf = root;
    if (!pf) return;
    free(pf->content);
    free(pf);
}

/* Bind a child VFS node to its procfs file description. */
static void procfs_open(void *parent, const char *name, vfs_node_t node)
{
    node->flags |= VFS_NODE_NOCACHE;
    procfs_file_t *ppf = parent;
    if (!ppf) return;

    procfs_file_t *pf = calloc(1, sizeof(procfs_file_t));
    if (!pf) return;

    switch (ppf->type) {
        case PROCFS_ROOT : {
            int subtype = -1;
            if (streq(name, "stat")) subtype = PROC_INFO_STAT;
            if (streq(name, "meminfo")) subtype = PROC_INFO_MEMINFO;
            if (streq(name, "cpuinfo")) subtype = PROC_INFO_CPUINFO;
            if (streq(name, "uptime")) subtype = PROC_INFO_UPTIME;
            if (streq(name, "version")) subtype = PROC_INFO_VERSION;
            if (streq(name, "loadavg")) subtype = PROC_INFO_LOADAVG;
            if (streq(name, "vmstat")) subtype = PROC_INFO_VMSTAT;
            if (streq(name, "modules")) subtype = PROC_INFO_MODULES;
            if (streq(name, "mounts")) subtype = PROC_INFO_MOUNTS;
            if (streq(name, "mountinfo")) subtype = PROC_INFO_MOUNTINFO;
            if (streq(name, "filesystems")) subtype = PROC_INFO_FILESYSTEMS;
            if (streq(name, "cmdline")) subtype = PROC_INFO_CMDLINE;
            if (streq(name, "cgroups")) subtype = PROC_INFO_CGROUPS;
            if (streq(name, "interrupts")) subtype = PROC_INFO_INTERRUPTS;
            if (streq(name, "partitions")) subtype = PROC_INFO_PARTITIONS;
            if (streq(name, "devices")) subtype = PROC_INFO_DEVICES;
            if (streq(name, "diskstats")) subtype = PROC_INFO_DISKSTATS;
            if (streq(name, "swaps")) subtype = PROC_INFO_SWAPS;
            if (streq(name, "misc")) subtype = PROC_INFO_MISC;
            if (streq(name, "softirqs")) subtype = PROC_INFO_SOFTIRQS;
            if (streq(name, "ioports")) subtype = PROC_INFO_IOPORTS;
            if (streq(name, "iomem")) subtype = PROC_INFO_IOMEM;
            if (subtype >= 0) {
                pf->type    = PROCFS_INFO_FILE;
                pf->subtype = subtype;
            } else {
                if (streq(name, "self") || streq(name, "thread-self")) {
                    pf->type   = PROCFS_SELF_LINK;
                    node->type = file_symlink;
                    break;
                }
                if (streq(name, "net")) {
                    pf->type   = PROCFS_NET_DIR;
                    node->type = file_dir;
                    break;
                }
                if (streq(name, "tty")) {
                    pf->type   = PROCFS_TTY_DIR;
                    node->type = file_dir;
                    break;
                }
                if (streq(name, "sys")) {
                    pf->type    = PROCFS_SYS_DIR;
                    pf->subtype = -1;
                    node->type  = file_dir;
                    break;
                }
                if (streq(name, "driver")) {
                    pf->type   = PROCFS_DRIVER_DIR;
                    node->type = file_dir;
                    break;
                }
                /* Try PID - numeric directory name */
                char *end;
                pid_t pid = (pid_t)strtol(name, &end, 10);
                if (*end == '\0' && process_find(pid)) {
                    pf->type   = PROCFS_PID_DIR;
                    pf->pid    = pid;
                    node->type = file_dir;
                } else {
                    free(pf);
                    return;
                }
            }
            break;
        }
        case PROCFS_PID_DIR : {
            int subtype = -1;
            if (streq(name, "status")) subtype = PROC_PID_STATUS;
            if (streq(name, "maps")) subtype = PROC_PID_MAPS;
            if (streq(name, "cmdline")) subtype = PROC_PID_CMDLINE;
            if (streq(name, "name")) subtype = PROC_PID_NAME;
            if (streq(name, "stat")) subtype = PROC_PID_STAT;
            if (streq(name, "mem")) subtype = PROC_PID_MEM;
            if (streq(name, "mounts")) subtype = PROC_PID_MOUNTS;
            if (streq(name, "mountinfo")) subtype = PROC_PID_MOUNTINFO;
            if (streq(name, "cgroup")) subtype = PROC_PID_CGROUP;
            if (streq(name, "comm")) subtype = PROC_PID_COMM;
            if (streq(name, "statm")) subtype = PROC_PID_STATM;
            if (streq(name, "limits")) subtype = PROC_PID_LIMITS;
            if (streq(name, "io")) subtype = PROC_PID_IO;
            if (streq(name, "oom_score_adj")) subtype = PROC_PID_OOM_SCORE_ADJ;
            if (subtype >= 0) {
                pf->type    = PROCFS_PID_FILE;
                pf->pid     = ppf->pid;
                pf->subtype = subtype;
            } else if (streq(name, "fd")) {
                pf->type   = PROCFS_PID_FD_DIR;
                pf->pid    = ppf->pid;
                node->type = file_dir;
            } else if (streq(name, "exe")) {
                pf->type   = PROCFS_PID_EXE_LINK;
                pf->pid    = ppf->pid;
                node->type = file_symlink;
            } else if (streq(name, "cwd")) {
                pf->type   = PROCFS_PID_CWD_LINK;
                pf->pid    = ppf->pid;
                node->type = file_symlink;
            } else if (streq(name, "root")) {
                pf->type   = PROCFS_PID_ROOT_LINK;
                pf->pid    = ppf->pid;
                node->type = file_symlink;
            } else {
                free(pf);
                return;
            }
            break;
        }
        case PROCFS_PID_FD_DIR : {
            process_t *proc = process_find(ppf->pid);
            if (!proc) {
                free(pf);
                return;
            }
            char           *end;
            int             fd   = (int)strtol(name, &end, 10);
            process_file_t *file = NULL;
            if (*end == '\0' && fd >= 0 && fd < PROCESS_MAX_FD) file = process_fd_get(proc, fd);
            if (!file) {
                free(pf);
                return;
            }
            process_file_put(file);
            pf->type    = PROCFS_PID_FD_LINK;
            pf->pid     = ppf->pid;
            pf->subtype = fd;
            node->type  = file_symlink;
            break;
        }
        case PROCFS_NET_DIR : {
            int subtype = -1;
            if (streq(name, "dev")) subtype = PROC_NET_DEV;
            if (streq(name, "arp")) subtype = PROC_NET_ARP;
            if (streq(name, "route")) subtype = PROC_NET_ROUTE;
            if (streq(name, "tcp")) subtype = PROC_NET_TCP;
            if (streq(name, "udp")) subtype = PROC_NET_UDP;
            if (streq(name, "unix")) subtype = PROC_NET_UNIX;
            if (subtype < 0) {
                free(pf);
                return;
            }
            pf->type    = PROCFS_NET_FILE;
            pf->subtype = subtype;
            break;
        }
        case PROCFS_TTY_DIR : {
            if (streq(name, "drivers")) {
                pf->type    = PROCFS_TTY_FILE;
                pf->subtype = 0;
            } else if (streq(name, "ldiscs")) {
                pf->type    = PROCFS_TTY_FILE;
                pf->subtype = 1;
            } else {
                free(pf);
                return;
            }
            break;
        }
        case PROCFS_SYS_DIR : {
            if (streq(name, "kernel")) {
                pf->type    = PROCFS_SYS_DIR;
                pf->subtype = PROC_SYS_KERNEL;
                node->type  = file_dir;
                break;
            }
            procfs_sysctl_t *sc = procfs_sysctl_find(ppf->subtype, name);
            if (!sc) {
                free(pf);
                return;
            }
            pf->type    = PROCFS_SYS_FILE;
            pf->subtype = ppf->subtype;
            pf->pid     = (pid_t)(sc - procfs_sysctl_kernel);
            break;
        }
        default :
            free(pf);
            return;
    }

    node->handle = pf;
}

static void procfs_close(void *current)
{
    (void)current;
}

/* Resolve a procfs symlink (self, fd, exe, cwd, root) to its target. */
static size_t procfs_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    procfs_file_t *pf = node ? node->handle : NULL;
    if (!pf || !addr) return 0;

    char target[VFS_PATH_MAX];
    int  length = 0;

    switch (pf->type) {
        case PROCFS_SELF_LINK : {
            process_t *proc = process_current();
            if (!proc || !proc->task) return 0;
            length = snprintf(target, sizeof(target), "%llu", (uint64_t)proc->task->tgid);
            break;
        }
        case PROCFS_PID_FD_LINK : {
            process_t *proc = process_find_get(pf->pid);
            if (!proc) {
                process_put(proc);
                return 0;
            }
            procfs_fd_target(proc, pf->subtype, target, sizeof(target));
            process_put(proc);
            length = (int)strlen(target);
            break;
        }
        case PROCFS_PID_EXE_LINK : {
            process_t *proc = process_find_get(pf->pid);
            if (!proc) {
                process_put(proc);
                return 0;
            }
            (void)snprintf(target, sizeof(target), "%s", proc->exe_path[0] ? proc->exe_path : "/unknown");
            process_put(proc);
            length = (int)strlen(target);
            break;
        }
        case PROCFS_PID_CWD_LINK : {
            process_t *proc = process_find_get(pf->pid);
            if (!proc) {
                process_put(proc);
                return 0;
            }
            (void)snprintf(target, sizeof(target), "%s", proc->cwd[0] ? proc->cwd : "/");
            process_put(proc);
            length = (int)strlen(target);
            break;
        }
        case PROCFS_PID_ROOT_LINK : {
            process_t *proc = process_find_get(pf->pid);
            if (!proc) {
                process_put(proc);
                return 0;
            }
            (void)snprintf(target, sizeof(target), "%s", proc->root[0] ? proc->root : "/");
            process_put(proc);
            length = (int)strlen(target);
            break;
        }
        default :
            return 0;
    }

    if (length <= 0 || offset >= (size_t)length) return 0;
    size_t actual = (size_t)length - offset;
    if (actual > size) actual = size;
    memcpy(addr, target + offset, actual);
    return actual;
}

/* Snapshot a file's generated content into a per-open description. */
static int procfs_file_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    (void)flags;
    if (!node || !private_data) return -EINVAL;
    procfs_file_t *source = node->handle;
    if (!source
        || (source->type != PROCFS_INFO_FILE && source->type != PROCFS_PID_FILE && source->type != PROCFS_NET_FILE
            && source->type != PROCFS_SYS_FILE && source->type != PROCFS_TTY_FILE)) {
        *private_data = NULL;
        return EOK;
    }

    procfs_file_t *snapshot = procfs_file_alloc(source->type, source->pid, source->subtype);
    if (!snapshot) return -ENOMEM;
    procfs_gen_content(snapshot, NULL);
    *private_data = snapshot;
    return EOK;
}

static void procfs_file_release(vfs_node_t node, void *private_data)
{
    (void)node;
    procfs_file_t *snapshot = private_data;
    if (!snapshot) return;
    free(snapshot->content);
    free(snapshot);
}

/* Read from a snapshot, regenerating content if it was never produced. */
static int64_t procfs_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)flags;
    procfs_file_t *pf = private_data ? private_data : node ? node->handle : NULL;
    if (!pf || !addr) return -EINVAL;
    if (!pf->content) procfs_gen_content(pf, NULL);
    if (!pf->content || offset >= pf->size) return 0;
    size_t actual = size < pf->size - offset ? size : pf->size - offset;
    memcpy(addr, pf->content + offset, actual);
    return (int64_t)actual;
}

/* Populate a node's type and children for stat/enumeration. */
static int procfs_stat(void *file, vfs_node_t node)
{
    node->flags |= VFS_NODE_NOCACHE;
    procfs_file_t *pf = file;
    if (!pf) return -ENOENT;

    switch (pf->type) {
        case PROCFS_ROOT : {
            node->type = file_dir;

            struct {
                    const char *name;
                    int         subtype;
            } info_tab[] = {
                {"stat",        PROC_INFO_STAT       },
                {"meminfo",     PROC_INFO_MEMINFO    },
                {"cpuinfo",     PROC_INFO_CPUINFO    },
                {"uptime",      PROC_INFO_UPTIME     },
                {"version",     PROC_INFO_VERSION    },
                {"loadavg",     PROC_INFO_LOADAVG    },
                {"vmstat",      PROC_INFO_VMSTAT     },
                {"modules",     PROC_INFO_MODULES    },
                {"mounts",      PROC_INFO_MOUNTS     },
                {"mountinfo",   PROC_INFO_MOUNTINFO  },
                {"filesystems", PROC_INFO_FILESYSTEMS},
                {"cmdline",     PROC_INFO_CMDLINE    },
                {"cgroups",     PROC_INFO_CGROUPS    },
                {"interrupts",  PROC_INFO_INTERRUPTS },
                {"partitions",  PROC_INFO_PARTITIONS },
                {"devices",     PROC_INFO_DEVICES    },
                {"diskstats",   PROC_INFO_DISKSTATS  },
                {"swaps",       PROC_INFO_SWAPS      },
                {"misc",        PROC_INFO_MISC       },
                {"softirqs",    PROC_INFO_SOFTIRQS   },
                {"ioports",     PROC_INFO_IOPORTS    },
                {"iomem",       PROC_INFO_IOMEM      },
            };
            for (size_t i = 0; i < sizeof(info_tab) / sizeof(info_tab[0]); i++)
                (void)procfs_ensure_child(node, info_tab[i].name, PROCFS_INFO_FILE, 0, info_tab[i].subtype, file_none);

            (void)procfs_ensure_child(node, "net", PROCFS_NET_DIR, 0, 0, file_dir);
            (void)procfs_ensure_child(node, "tty", PROCFS_TTY_DIR, 0, 0, file_dir);
            (void)procfs_ensure_child(node, "sys", PROCFS_SYS_DIR, 0, 0, file_dir);
            (void)procfs_ensure_child(node, "driver", PROCFS_DRIVER_DIR, 0, 0, file_dir);
            (void)procfs_ensure_child(node, "self", PROCFS_SELF_LINK, 0, 0, file_symlink);
            (void)procfs_ensure_child(node, "thread-self", PROCFS_SELF_LINK, 0, 1, file_symlink);

            procfs_deactivate_pid_nodes(node);
            /*
             * procfs_stat() runs under the VFS namespace lock.  Do not take
             * and then drop process references here: process_put() is allowed
             * to run the final destructor, which closes descriptors and
             * recursively enters the VFS namespace.  A PID-only snapshot is
             * sufficient for constructing /proc/<pid> names and also avoids
             * repeatedly locking the process table once per entry.
             */
            pid_t *pids = malloc(PROCESS_TABLE_SIZE * sizeof(*pids));
            if (!pids) return -ENOMEM;
            size_t pid_count = process_snapshot_pids(pids, PROCESS_TABLE_SIZE);
            for (size_t pos = 0; pos < pid_count; pos++) {
                char  pid_str[16];
                pid_t pid = pids[pos];
                if (pid > 0) {
                    (void)snprintf(pid_str, sizeof(pid_str), "%llu", (uint64_t)pid);
                    (void)procfs_ensure_child(node, pid_str, PROCFS_PID_DIR, pid, 0, file_dir);
                }
            }
            free(pids);
            break;
        }
        case PROCFS_PID_DIR : {
            if (!process_find(pf->pid)) {
                node->type = file_none;
                return -ENOENT;
            }
            node->type = file_dir;

            struct {
                    const char *name;
                    int         subtype;
            } pid_tab[] = {
                {"status",        PROC_PID_STATUS       },
                {"maps",          PROC_PID_MAPS         },
                {"cmdline",       PROC_PID_CMDLINE      },
                {"name",          PROC_PID_NAME         },
                {"stat",          PROC_PID_STAT         },
                {"mem",           PROC_PID_MEM          },
                {"mounts",        PROC_PID_MOUNTS       },
                {"mountinfo",     PROC_PID_MOUNTINFO    },
                {"cgroup",        PROC_PID_CGROUP       },
                {"comm",          PROC_PID_COMM         },
                {"statm",         PROC_PID_STATM        },
                {"limits",        PROC_PID_LIMITS       },
                {"io",            PROC_PID_IO           },
                {"oom_score_adj", PROC_PID_OOM_SCORE_ADJ},
            };
            for (size_t i = 0; i < sizeof(pid_tab) / sizeof(pid_tab[0]); i++)
                (void)procfs_ensure_child(node, pid_tab[i].name, PROCFS_PID_FILE, pf->pid, pid_tab[i].subtype, file_none);

            (void)procfs_ensure_child(node, "fd", PROCFS_PID_FD_DIR, pf->pid, 0, file_dir);
            (void)procfs_ensure_child(node, "exe", PROCFS_PID_EXE_LINK, pf->pid, 0, file_symlink);
            (void)procfs_ensure_child(node, "cwd", PROCFS_PID_CWD_LINK, pf->pid, 0, file_symlink);
            (void)procfs_ensure_child(node, "root", PROCFS_PID_ROOT_LINK, pf->pid, 0, file_symlink);
            break;
        }
        case PROCFS_PID_FD_DIR : {
            process_t *proc = process_find_get(pf->pid);
            if (!proc) {
                process_put(proc);
                node->type = file_none;
                return -ENOENT;
            }
            node->type = file_dir;

            for (clist_t link = node->child; link; link = link->next) {
                vfs_node_t     child = link->data;
                procfs_file_t *cpf   = child ? child->handle : NULL;
                if (cpf && cpf->type == PROCFS_PID_FD_LINK) {
                    child->flags |= VFS_NODE_UNLINKED;
                    child->type = file_none;
                }
            }
            for (int fd = 0; fd < PROCESS_MAX_FD; fd++) {
                process_file_t *file = process_fd_get(proc, fd);
                if (!file) continue;
                process_file_put(file);
                char name[8];
                (void)snprintf(name, sizeof(name), "%d", fd);
                (void)procfs_ensure_child(node, name, PROCFS_PID_FD_LINK, pf->pid, fd, file_symlink);
            }
            process_put(proc);
            break;
        }
        case PROCFS_NET_DIR : {
            static const char *names[] = {"dev", "arp", "route", "tcp", "udp", "unix"};
            node->type                 = file_dir;
            for (int i = 0; i < 6; i++) (void)procfs_ensure_child(node, names[i], PROCFS_NET_FILE, 0, i, file_none);
            break;
        }
        case PROCFS_TTY_DIR : {
            node->type = file_dir;
            (void)procfs_ensure_child(node, "drivers", PROCFS_TTY_FILE, 0, 0, file_none);
            (void)procfs_ensure_child(node, "ldiscs", PROCFS_TTY_FILE, 0, 1, file_none);
            break;
        }
        case PROCFS_SYS_DIR : {
            node->type = file_dir;
            if (pf->subtype < 0) {
                (void)procfs_ensure_child(node, "kernel", PROCFS_SYS_DIR, 0, PROC_SYS_KERNEL, file_dir);
            } else if (pf->subtype == PROC_SYS_KERNEL) {
                for (size_t i = 0; i < PROCFS_SYSCTL_KERNEL_COUNT; i++)
                    (void)procfs_ensure_child(node, procfs_sysctl_kernel[i].name, PROCFS_SYS_FILE, (pid_t)i, PROC_SYS_KERNEL, file_none);
            }
            break;
        }
        case PROCFS_DRIVER_DIR :
            node->type = file_dir;
            break;
        case PROCFS_SELF_LINK :
        case PROCFS_PID_FD_LINK :
        case PROCFS_PID_EXE_LINK :
        case PROCFS_PID_CWD_LINK :
        case PROCFS_PID_ROOT_LINK :
            node->type = file_symlink;
            break;
        case PROCFS_INFO_FILE :
        case PROCFS_PID_FILE :
        case PROCFS_NET_FILE :
        case PROCFS_SYS_FILE :
        case PROCFS_TTY_FILE :
            /*
             * procfs files are generated pseudo-regular files.  They are
             * seekable and, most importantly, reads must advance the open
             * file description offset so that readers can observe EOF.
             * file_stream is reserved for unseekable devices (terminals,
             * pipes, etc.) and would restart every procfs read at offset 0.
             */
            node->type = file_none;
            /*
             * Mount tables are generated as a per-open namespace snapshot.
             * Generating them here would recurse into the VFS namespace lock
             * held by pathname lookup.  Other proc files use the same
             * per-open path to avoid cross-reader offset/content races.
             */
            node->size = 0;
            break;
    }
    return EOK;
}

/* Legacy read callback serving the generated content. */
static size_t procfs_read(void *file, void *addr, size_t offset, size_t size)
{
    procfs_file_t *pf = file;
    if (!pf) return 0;

    if (!pf->content) procfs_gen_content(pf, NULL);
    if (!pf->content) return 0;
    if (offset >= pf->size) return 0;

    size_t actual = (offset + size > pf->size) ? (pf->size - offset) : size;
    memcpy(addr, pf->content + offset, actual);
    return actual;
}

/* Legacy write callback, applying sysctl values. */
static size_t procfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    procfs_file_t *pf = file;
    if (!pf || !addr || offset) return 0;

    if (pf->type == PROCFS_SYS_FILE) {
        procfs_sysctl_t *sc = procfs_sysctl_lookup(pf->subtype, (size_t)pf->pid);
        if (!sc || size >= PROCFS_BUF_SIZE) return 0;
        char buf[PROCFS_BUF_SIZE];
        memcpy(buf, addr, size);
        if (procfs_sysctl_apply(sc, buf, size) != EOK) return 0;
        return size;
    }
    return size;
}

/* Write to a proc control file through its description. */
static int64_t procfs_file_write(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)flags;
    procfs_file_t *pf = private_data ? private_data : node ? node->handle : NULL;
    if (!pf || (!addr && size)) return -EINVAL;
    if (offset != 0) return -EINVAL;

    if (pf->type == PROCFS_SYS_FILE) {
        size_t written = procfs_write(pf, addr, offset, size);
        return written == size ? (int64_t)written : -EINVAL;
    }

    /*
     * eudevd writes "0" here before processing each event.  The kernel does
     * not currently implement OOM scoring, but the Linux control-file ABI
     * still requires a successful, consuming write.
     */
    if (pf->type == PROCFS_PID_FILE && pf->subtype == PROC_PID_OOM_SCORE_ADJ) return (int64_t)size;
    return -EACCES;
}

static int procfs_mkdir(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -EROFS;
}

static int procfs_mkfile(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -EROFS;
}

static int procfs_delete(void *parent, vfs_node_t node)
{
    (void)parent;
    (void)node;
    return -EROFS;
}

static int procfs_rename(void *current, const char *new_name)
{
    (void)current;
    (void)new_name;
    return -EROFS;
}

/* Accept O_TRUNC on writable control files; nothing to discard. */
static int procfs_resize(void *current, uint64_t size)
{
    (void)size;
    procfs_file_t *pf = current;
    if (!pf) return -EINVAL;
    /*
     * Writable proc control files accept O_TRUNC as part of fopen("w").
     * Their contents are synthetic, so truncation has no persistent data to
     * discard.
     */
    if (pf->type == PROCFS_PID_FILE && pf->subtype == PROC_PID_OOM_SCORE_ADJ) return EOK;
    return -EOPNOTSUPP;
}

/* Free a procfs file description and its generated content. */
static int procfs_free(void *handle)
{
    procfs_file_t *pf = handle;
    if (!pf) return EOK;
    free(pf->content);
    free(pf);
    return EOK;
}

/* Duplicate a node sharing the original procfs description. */
static vfs_node_t procfs_dup(vfs_node_t node)
{
    vfs_node_t copy   = vfs_node_alloc(node->parent, node->name);
    copy->handle      = node->handle;
    copy->type        = node->type;
    copy->size        = node->size;
    copy->flags       = node->flags;
    copy->permissions = node->permissions;
    copy->owner       = node->owner;
    copy->child       = NULL;
    copy->realsize    = node->realsize;
    return copy;
}

static int procfs_poll(void *file, size_t events)
{
    (void)file;
    int revents = 0;
    if (events & 0x0001) revents |= 0x0001;
    if (events & 0x0004) revents |= 0x0004;
    return revents;
}

static int procfs_ioctl(void *file, size_t req, void *arg)
{
    (void)file;
    (void)req;
    (void)arg;
    return EOK;
}

/* Callback table */

static struct vfs_callback procfs_callbacks = {
    .mount        = procfs_mount,
    .unmount      = procfs_umount,
    .open         = procfs_open,
    .close        = procfs_close,
    .read         = procfs_read,
    .write        = procfs_write,
    .readlink     = procfs_readlink,
    .mkdir        = procfs_mkdir,
    .mkfile       = procfs_mkfile,
    .link         = (vfs_mk_t)procfs_dummy,
    .symlink      = (vfs_mk_t)procfs_dummy,
    .stat         = procfs_stat,
    .ioctl        = procfs_ioctl,
    .dup          = procfs_dup,
    .poll         = procfs_poll,
    .free         = procfs_free,
    .delete       = procfs_delete,
    .rename       = procfs_rename,
    .file_open    = procfs_file_open,
    .file_release = procfs_file_release,
    .file_read    = procfs_file_read,
    .file_write   = procfs_file_write,
    .resize       = procfs_resize,
};

/* Registration */

/* Register the proc filesystem with the VFS layer. */
void procfs_regist(void)
{
    /*
     * Linux exposes this filesystem to mount(2) as "proc".  User space
     * (BusyBox mount, OpenRC and /etc/fstab) consequently passes -t proc.
     */
    procfs_id = vfs_regist_fs_flags("proc", &procfs_callbacks, VFS_FS_NODEV);
    if (procfs_id & ERRNO_MASK) plogk("procfs: Register error.\n");
    if (!(procfs_id & ERRNO_MASK)) plogk("procfs: Filesystem registered (fsid=%d)\n", procfs_id);
}
