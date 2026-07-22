/*
 *
 *      procfs.c
 *      Process file system
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpuid.h>
#include <arch/smp.h>
#include <drivers/tsc.h>
#include <fs/procfs.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <libs/std/stdarg.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <proc/process.h>
#include <proc/sched.h>

static int procfs_id;

/* ------------------------------------------------------------------ */
/*  Internal types                                                     */
/* ------------------------------------------------------------------ */

enum procfs_info_type {
    PROC_INFO_STAT,
    PROC_INFO_MEMINFO,
    PROC_INFO_CPUINFO,
    PROC_INFO_UPTIME,
    PROC_INFO_VERSION,
    PROC_INFO_LOADAVG,
};

enum procfs_pid_file_type {
    PROC_PID_STATUS,
    PROC_PID_MAPS,
    PROC_PID_CMDLINE,
    PROC_PID_NAME,
    PROC_PID_STAT,
    PROC_PID_MEM,
};

enum procfs_type {
    PROCFS_ROOT,
    PROCFS_PID_DIR,
    PROCFS_INFO_FILE,
    PROCFS_PID_FILE,
};

typedef struct procfs_file {
        enum procfs_type type;
        pid_t            pid;
        int              subtype;
        char            *content;
        size_t           size;
        size_t           capacity;
} procfs_file_t;

#define PROCFS_BUF_SIZE 4096

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void procfs_dummy(void)
{
}

static void clear_children(vfs_node_t parent)
{
    if (!parent || !parent->child) return;
    parent->child = clist_free_with(parent->child, (void (*)(void *))vfs_free);
}

static procfs_file_t *procfs_file_alloc(enum procfs_type type, pid_t pid, int subtype)
{
    procfs_file_t *pf = calloc(1, sizeof(procfs_file_t));
    if (!pf) return NULL;
    pf->type    = type;
    pf->pid     = pid;
    pf->subtype = subtype;
    return pf;
}

/* ------------------------------------------------------------------ */
/*  Content generators                                                 */
/* ------------------------------------------------------------------ */

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
        n = snprintf(p, remaining, "intr %llu\nctxt %llu\nbtime %llu\nprocesses %llu\nprocs_running %u\nprocs_blocked %u\n", 0ULL, 0ULL, 0ULL,
                     scheduler.next_pid, cpu_rqs[0].nr_running + 1, 0U);
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

    size_t total_kb = (frame_allocator.origin_frames * PAGE_4K_SIZE) / 1024;
    size_t free_kb  = (frame_allocator.usable_frames * PAGE_4K_SIZE) / 1024;
    int    n        = snprintf(buf, PROCFS_BUF_SIZE,
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
                               total_kb, free_kb, free_kb, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL,
                               (KERNEL_HEAP_SIZE) / 1024, 0UL, (KERNEL_HEAP_SIZE) / 1024);

    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
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
        if (cache_type == 1)
            l1d_kb = size_kb; /* data cache */
        else if (cache_type == 2)
            l1d_kb = size_kb; /* instruction cache */
        else if (cache_type == 3)
            l2_kb = size_kb; /* unified L2 */
    }

    /* CPU frequency */
    uint64_t cpu_hz     = tsc_get_cpu_frequency();
    uint64_t cpu_mhz    = cpu_hz / 1000000;
    uint64_t cpu_mhz_fp = (cpu_hz % 1000000) / 10000; /* one decimal */

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

    uint64_t ns       = tsc_nano_time();
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
    for (uint32_t i = 0; i < cpu_count; i++) { running += cpu_rqs[i].nr_running; }
    /* active threads = currently running (one per CPU) + on ready queues */
    uint64_t active  = cpu_count + running;
    uint64_t total   = scheduler.next_pid ? scheduler.next_pid - 1 : 0;
    uint64_t lastpid = scheduler.next_pid ? scheduler.next_pid - 1 : 0;

    int n = snprintf(buf, 128, "0.00 0.00 0.00 %llu/%llu %llu\n", active, total ? total : 1, lastpid);

    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 128;
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
            state_str = "R (running)";
            break;
        case TASK_RUNNING :
            state_str = "R (running)";
            break;
        case TASK_BLOCKED :
            state_str = "S (sleeping)";
            break;
        case TASK_SLEEPING :
            state_str = "S (sleeping)";
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
                     "TracerPid:\t0\n"
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
                     proc->task->name, state_str, (uint64_t)pf->pid, (uint64_t)pf->pid, (uint64_t)ppid, proc->uid, proc->uid, proc->uid,
                     proc->uid, proc->gid, proc->gid, proc->gid, proc->gid, 0U, 0U, vmsize / 1024, vmrss / 1024, vmdata / 1024, vmstack / 1024);

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
        }

        const char *region_name = "";
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

        n = snprintf(p, remaining, "%016lx-%016lx %s %08lx 00:00 0%s\n", vma->start, vma->end, perm, 0UL, region_name);
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
    (void)pf;
    char *buf = malloc(64);
    if (!buf) return;

    buf[0]       = '\0';
    pf->content  = buf;
    pf->size     = 0;
    pf->capacity = 64;
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
    process_t *proc = process_find(pf->pid);
    if (!proc) return;

    char *buf = malloc(512);
    if (!buf) return;

    char state_char = '?';
    switch (proc->task->state) {
        case TASK_READY :
            state_char = 'R';
            break;
        case TASK_RUNNING :
            state_char = 'R';
            break;
        case TASK_BLOCKED :
            state_char = 'S';
            break;
        case TASK_SLEEPING :
            state_char = 'S';
            break;
        case TASK_ZOMBIE :
            state_char = 'Z';
            break;
        case TASK_IDLE :
            state_char = 'I';
            break;
    }

    pid_t ppid = proc->parent ? proc->parent->task->pid : 0;

    int n = snprintf(buf, 512, "%llu (%s) %c %llu %llu %llu %llu -1 %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                     (uint64_t)pf->pid, proc->task->name, state_char, (uint64_t)ppid, (uint64_t)pf->pid, (uint64_t)pf->pid, 0ULL, 0ULL, 0ULL,
                     0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL);

    pf->content  = buf;
    pf->size     = n < 0 ? 0 : (size_t)n;
    pf->capacity = 512;
}

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
            }
            break;
        default :
            break;
    }

    if (pf->content && pf->size > 0) node->size = pf->size;
}

/* ------------------------------------------------------------------ */
/*  VFS callbacks                                                      */
/* ------------------------------------------------------------------ */

static int procfs_mount(const char *handle, vfs_node_t node)
{
    if (handle) return -EINVAL;

    node->fsid = procfs_id;

    procfs_file_t *root = procfs_file_alloc(PROCFS_ROOT, 0, 0);
    if (!root) return -ENOMEM;

    node->handle = root;
    node->type   = file_dir;
    return EOK;
}

static void procfs_umount(void *root)
{
    procfs_file_t *pf = root;
    if (!pf) return;
    free(pf->content);
    free(pf);
}

static void procfs_open(void *parent, const char *name, vfs_node_t node)
{
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
            if (subtype >= 0) {
                pf->type    = PROCFS_INFO_FILE;
                pf->subtype = subtype;
            } else {
                /* Try PID – numeric directory name */
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
            if (subtype >= 0) {
                pf->type    = PROCFS_PID_FILE;
                pf->pid     = ppf->pid;
                pf->subtype = subtype;
            } else {
                free(pf);
                return;
            }
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

static int procfs_stat(void *file, vfs_node_t node)
{
    procfs_file_t *pf = file;
    if (!pf) return -ENOENT;

    switch (pf->type) {
        case PROCFS_ROOT : {
            clear_children(node);
            node->type = file_dir;

            struct {
                    const char *name;
                    int         subtype;
            } info_tab[] = {
                {"stat",    PROC_INFO_STAT   },
                {"meminfo", PROC_INFO_MEMINFO},
                {"cpuinfo", PROC_INFO_CPUINFO},
                {"uptime",  PROC_INFO_UPTIME },
                {"version", PROC_INFO_VERSION},
                {"loadavg", PROC_INFO_LOADAVG},
            };
            for (size_t i = 0; i < sizeof(info_tab) / sizeof(info_tab[0]); i++) {
                vfs_node_t child = vfs_node_alloc(node, info_tab[i].name);
                if (!child) continue;
                child->type   = file_none;
                child->handle = procfs_file_alloc(PROCFS_INFO_FILE, 0, info_tab[i].subtype);
            }

            size_t     pos = 0;
            process_t *proc;
            while ((proc = process_iterate(&pos))) {
                char pid_str[16];
                snprintf(pid_str, sizeof(pid_str), "%llu", (uint64_t)proc->task->pid);
                vfs_node_t child = vfs_node_alloc(node, pid_str);
                if (!child) continue;
                child->type   = file_dir;
                child->handle = procfs_file_alloc(PROCFS_PID_DIR, proc->task->pid, 0);
            }
            break;
        }
        case PROCFS_PID_DIR : {
            if (!process_find(pf->pid)) {
                node->type = file_none;
                return -ENOENT;
            }
            clear_children(node);
            node->type = file_dir;

            struct {
                    const char *name;
                    int         subtype;
            } pid_tab[] = {
                {"status",  PROC_PID_STATUS },
                {"maps",    PROC_PID_MAPS   },
                {"cmdline", PROC_PID_CMDLINE},
                {"name",    PROC_PID_NAME   },
                {"stat",    PROC_PID_STAT   },
                {"mem",     PROC_PID_MEM    },
            };
            for (size_t i = 0; i < sizeof(pid_tab) / sizeof(pid_tab[0]); i++) {
                vfs_node_t child = vfs_node_alloc(node, pid_tab[i].name);
                if (!child) continue;
                child->type   = file_none;
                child->handle = procfs_file_alloc(PROCFS_PID_FILE, pf->pid, pid_tab[i].subtype);
            }
            break;
        }
        case PROCFS_INFO_FILE :
        case PROCFS_PID_FILE :
            if (!pf->content) procfs_gen_content(pf, node);
            node->type = file_stream;
            if (pf->content) node->size = pf->size;
            break;
    }
    return EOK;
}

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

static size_t procfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    return size;
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

static int procfs_free(void *handle)
{
    procfs_file_t *pf = handle;
    if (!pf) return EOK;
    free(pf->content);
    free(pf);
    return EOK;
}

static vfs_node_t procfs_dup(vfs_node_t node)
{
    vfs_node_t copy   = vfs_node_alloc(node->parent, node->name);
    copy->handle      = node->handle;
    copy->type        = node->type;
    copy->size        = node->size;
    copy->flags       = node->flags;
    copy->permissions = node->permissions;
    copy->owner       = node->owner;
    copy->child       = node->child;
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

/* ------------------------------------------------------------------ */
/*  Callback table                                                     */
/* ------------------------------------------------------------------ */

static struct vfs_callback procfs_callbacks = {
    .mount    = procfs_mount,
    .unmount  = procfs_umount,
    .open     = procfs_open,
    .close    = procfs_close,
    .read     = procfs_read,
    .write    = procfs_write,
    .readlink = (vfs_readlink_t)procfs_dummy,
    .mkdir    = procfs_mkdir,
    .mkfile   = procfs_mkfile,
    .link     = (vfs_mk_t)procfs_dummy,
    .symlink  = (vfs_mk_t)procfs_dummy,
    .stat     = procfs_stat,
    .ioctl    = procfs_ioctl,
    .dup      = procfs_dup,
    .poll     = procfs_poll,
    .free     = procfs_free,
    .delete   = procfs_delete,
    .rename   = procfs_rename,
};

/* ------------------------------------------------------------------ */
/*  Registration                                                       */
/* ------------------------------------------------------------------ */

void procfs_regist(void)
{
    procfs_id = vfs_regist_fs("procfs", &procfs_callbacks);
    if (procfs_id & ERRNO_MASK) plogk("procfs: Register error.\n");
}
