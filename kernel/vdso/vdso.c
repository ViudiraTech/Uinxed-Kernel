/*
 *
 *      vdso.c
 *      Map the vDSO image and publish the time data page
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <kernel/timer/timer.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <kernel/printk.h>
#include <kernel/vdso/vdso.h>
#include <libs/std/string.h>
#include <mem/page.h>
#include <process/process.h>

/* Emitted by scripts/vdso2c.py from the ELF the vdso/ build produces. */
extern const uint8_t  vdso_image[];
extern const uint32_t vdso_image_len;

typedef struct {
        uint8_t  ident[16];
        uint16_t type;
        uint16_t machine;
        uint32_t version;
        uint64_t entry;
        uint64_t phoff;
        uint64_t shoff;
        uint32_t flags;
        uint16_t ehsize;
        uint16_t phentsize;
        uint16_t phnum;
} vdso_ehdr_t;

typedef struct {
        uint32_t type;
        uint32_t flags;
        uint64_t offset;
        uint64_t vaddr;
        uint64_t paddr;
        uint64_t filesz;
        uint64_t memsz;
        uint64_t align;
} vdso_phdr_t;

#define VDSO_PT_LOAD 1

/* The single data page every process maps; written by the tick, read by userspace. */
static struct vdso_data *vdso_page;
static uint64_t           vdso_page_phys;

/*
 * The code half of the image is identical for every process and never written,
 * so it is copied into frames once and shared, exactly like the data page.
 */
static uint64_t vdso_code_phys;

static uint32_t vdso_image_filesz; // bytes of image the ELF actually contains
static uint32_t vdso_image_size;   // bytes of image to map, page rounded
static uint32_t vdso_data_offset;  // where the data page sits inside the image

static uint64_t vdso_cal_start_cycles;
static uint64_t vdso_cal_start_mono;
static uint32_t vdso_cal_ticks;
static uint64_t vdso_ns_per_sec; // cycles per second once calibrated
static bool     vdso_ready;

/* Read a little-endian 64-bit field out of the image. */
static uint64_t vdso_read64(const uint8_t *base, uint64_t offset)
{
    uint64_t value = 0;

    for (int i = 7; i >= 0; i--) value = (value << 8) | base[offset + (uint64_t)i];
    return value;
}

/* Read a little-endian 32-bit field out of the image. */
static uint32_t vdso_read32(const uint8_t *base, uint64_t offset)
{
    return (uint32_t)base[offset] | ((uint32_t)base[offset + 1] << 8) | ((uint32_t)base[offset + 2] << 16) | ((uint32_t)base[offset + 3] << 24);
}

/*
 * Read the image's single loadable segment.  There must be exactly one, and it
 * has to start at virtual address 0: the dynamic linker takes the image's
 * link-time base from the program headers, so a second segment -- or one that
 * does not begin at 0 -- makes it resolve every dynamic entry a page off.
 */
static int vdso_locate_segments(void)
{
    const vdso_ehdr_t *ehdr       = (const vdso_ehdr_t *)vdso_image;
    int                loads      = 0;

    if (vdso_image_len < sizeof(*ehdr) || ehdr->ident[0] != 0x7f || ehdr->ident[1] != 'E' || ehdr->ident[2] != 'L' || ehdr->ident[3] != 'F') return -1;
    if (ehdr->phentsize < sizeof(vdso_phdr_t)) return -1;

    for (uint16_t i = 0; i < ehdr->phnum; i++) {
        uint64_t    offset = ehdr->phoff + (uint64_t)i * ehdr->phentsize;
        vdso_phdr_t phdr;

        if (offset + sizeof(phdr) > vdso_image_len) return -1;
        phdr.type   = vdso_read32(vdso_image, offset);
        phdr.offset = vdso_read64(vdso_image, offset + 8);
        phdr.vaddr  = vdso_read64(vdso_image, offset + 16);
        phdr.filesz = vdso_read64(vdso_image, offset + 32);

        if (phdr.type != VDSO_PT_LOAD) continue;

        /*
         * The identity of file offset and virtual address is what lets the
         * kernel copy the file straight into frames and have every vaddr line
         * up, so both have to be zero.
         */
        if (phdr.vaddr || phdr.offset || !phdr.filesz || phdr.filesz > vdso_image_len) return -1;
        vdso_image_filesz = (uint32_t)phdr.filesz;
        vdso_image_size   = (uint32_t)ALIGN_UP(phdr.filesz, PAGE_4K_SIZE);
        loads++;
    }

    if (loads != 1) return -1;
    // The data page has to sit inside the image for the kernel to map it over.
    if (vdso_image_filesz <= VDSO_DATA_OFFSET || vdso_image_size < VDSO_DATA_OFFSET + PAGE_4K_SIZE) return -1;
    vdso_data_offset = VDSO_DATA_OFFSET;
    return 0;
}

/* Publish one snapshot of every clock, bracketed by the seqlock. */
static void vdso_publish(void)
{
    struct vdso_data *page = vdso_page;
    int64_t           real = timer_realtime_ns();
    uint64_t          mono = timer_monotonic_ns();

    if (!page) return;

    uint32_t seq = __atomic_load_n(&page->seq, __ATOMIC_RELAXED) + 1; // odd: write in progress
    __atomic_store_n(&page->seq, seq, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    page->clock_mode = vdso_ns_per_sec ? VDSO_CLOCKMODE_CYCLES : VDSO_CLOCKMODE_NONE;
    page->cycle_last = vdso_ns_per_sec ? rdtsc() : 0;
    page->mult       = vdso_ns_per_sec ? (uint32_t)(((1000000000ULL << 32) / vdso_ns_per_sec)) : 0;
    page->shift      = 32;
    /*
     * A snapshot is republished every tick, so a reader whose counter is more
     * than a few tens of milliseconds ahead is not reading a comparable one.
     */
    page->max_cycles = vdso_ns_per_sec / 50;
    page->real_sec   = (uint64_t)(real / 1000000000LL);
    page->real_nsec  = (uint64_t)(real % 1000000000LL);
    page->mono_sec   = mono / 1000000000ULL;
    page->mono_nsec  = mono % 1000000000ULL;
    page->boot_sec   = page->mono_sec;
    page->boot_nsec  = page->mono_nsec;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&page->seq, seq + 1, __ATOMIC_RELEASE); // even: consistent
}

/*
 * Derive cycles per second from the monotonic clock over a settling window.  The
 * exact TSC frequency is not advertised on this platform, so it is measured
 * instead; until the window closes the vDSO serves the tick snapshot alone.
 */
static void vdso_calibrate(void)
{
    if (vdso_ns_per_sec || !vdso_page) return;

    if (!vdso_cal_ticks) {
        vdso_cal_start_cycles = rdtsc();
        vdso_cal_start_mono   = timer_monotonic_ns();
        vdso_cal_ticks        = 1;
        return;
    }

    if (++vdso_cal_ticks < 200) return; // ~200 ms at CONFIG_TIMER_HZ=1000

    uint64_t elapsed_ns = timer_monotonic_ns() - vdso_cal_start_mono;
    uint64_t cycles     = rdtsc() - vdso_cal_start_cycles;

    if (elapsed_ns >= 100000000ULL) {
        vdso_ns_per_sec = (cycles * 1000000000ULL) / elapsed_ns;
        plogk("vdso: cycle counter calibrated at %llu Hz\n", (unsigned long long)vdso_ns_per_sec);
    }
}

/* Called from the timer tick: keep the published snapshot current. */
void vdso_tick(void)
{
    if (!vdso_ready) return;
    vdso_calibrate();
    vdso_publish();
}

/* Locate the image, copy it into shared frames, and allocate the data page. */
void vdso_init(void)
{
    if (vdso_locate_segments()) {
        plogk("vdso: image has no usable segments, disabled\n");
        return;
    }

    vdso_page_phys = alloc_frames(1);
    if (!vdso_page_phys) {
        plogk("vdso: no memory for the data page\n");
        return;
    }

    vdso_code_phys = alloc_frames(vdso_image_size / PAGE_4K_SIZE);
    if (!vdso_code_phys) {
        free_frames(vdso_page_phys, 1);
        plogk("vdso: no memory for the image\n");
        return;
    }

    /* The tail past the ELF is padding, so it is cleared rather than copied. */
    memset(phys_to_virt(vdso_code_phys), 0, vdso_image_size);
    memcpy(phys_to_virt(vdso_code_phys), vdso_image, vdso_image_filesz);

    vdso_page = (struct vdso_data *)phys_to_virt(vdso_page_phys);
    memset(vdso_page, 0, PAGE_4K_SIZE);
    vdso_page->res_nsec = timer_monotonic_resolution_ns();
    vdso_page->seq      = 2; // even: no writer active

    vdso_ready = true;
    vdso_publish();
    plogk("vdso: image %u bytes, data page 0x%x bytes in, at %p\n", vdso_image_size, vdso_data_offset, (void *)vdso_page);
}

/*
 * Map the image and its data page into the process and return the address the
 * image's virtual address 0 landed at.  The image is mapped whole, header
 * first, because the dynamic linker resolves symbols and dynamic entries
 * relative to that base; the data page is mapped over the image's own
 * placeholder at vdso_data_offset.
 */
uintptr_t vdso_map_process(process_t *proc)
{
    if (!vdso_ready || !proc || !proc->user_page_dir) return 0;

    size_t    length = (size_t)vdso_data_offset + PAGE_4K_SIZE;
    uintptr_t base   = ALIGN_DOWN(PROCESS_USER_STACK_TOP - CONFIG_PROCESS_STACK_SIZE, PAGE_4K_SIZE) - length;

    /*
     * Park the image directly below the stack.  The dynamic loader grows its
     * library mappings upward from PROCESS_MMAP_BASE, so a slot there would sit
     * in its way and a later mapping of a large program's libraries would fail.
     */
    spin_lock(&proc->mmap_lock);
    for (vm_area_t *vma = proc->mmap_list; vma; vma = vma->next) {
        if (vma->end <= base || vma->start >= base + length) continue;
        spin_unlock(&proc->mmap_lock);
        return 0; // occupied: run without a vDSO rather than fail the exec
    }
    spin_unlock(&proc->mmap_lock);

    vm_area_t *vma = calloc(1, sizeof(*vma));
    if (!vma) return 0;
    vma->start = base;
    vma->end   = base + length;
    vma->flags = VM_READ | VM_EXEC;
    vma->type  = VM_REGION_VDSO;
    if (vm_area_insert(proc, vma)) {
        free(vma);
        return 0;
    }

    /*
     * Use the failing variant: the VMA list does not cover every mapping the
     * loader installs, so an occupied leaf has to be an error rather than
     * something page_map_to() would silently replace.
     */
    /*
     * Both ranges are kernel owned: the image frames were allocated once at
     * init and the data page is written by the tick.  Marking the leaves shared
     * keeps process teardown from releasing either of them.
     */
    for (uint32_t done = 0; done < vdso_image_size; done += PAGE_4K_SIZE) {
        // The data page's slot gets the shared page below, not the image's copy.
        if (done == vdso_data_offset) continue;
        if (page_map_new_to(proc->user_page_dir, base + done, vdso_code_phys + done, PTE_PRESENT | PTE_USER | PTE_SHARED)) {
            plogk("vdso: image page at %p already mapped, skipping\n", (void *)(base + done));
            return 0;
        }
    }

    if (page_map_new_to(proc->user_page_dir, base + vdso_data_offset, vdso_page_phys, PTE_PRESENT | PTE_USER | PTE_SHARED)) {
        plogk("vdso: data page at %p already mapped, skipping\n", (void *)(base + vdso_data_offset));
        return 0;
    }

    /* AT_SYSINFO_EHDR names the image's virtual address 0, which is `base`. */
    return base;
}
