/*
 *
 *      pcm_memory.c
 *      ULSA runtime records and managed DMA ring buffers
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/hhdm.h>
#include <mem/page.h>

#if CONFIG_AUDIO

/*
 * Everything here is backed by whole frames from the frame allocator: the
 * status and control records each get their own page, and the ring buffer is
 * a physically contiguous run of pages.  That is what devices need for DMA,
 * and it is also what the mmap layer needs, since a mapping hands out frames
 * rather than heap addresses.
 */

/* Allocate one zeroed page, returning its physical address (0 on failure). */
static uint64_t ulsa_alloc_page(void)
{
    uint64_t phys = alloc_frames(1);
    if (phys) memset(phys_to_virt(phys), 0, PAGE_4K_SIZE);
    return phys;
}

/* Allocate the page-backed status and control records of a runtime. */
int ulsa_pcm_runtime_alloc_records(ulsa_pcm_runtime_t *runtime)
{
    runtime->status_phys = ulsa_alloc_page();
    if (!runtime->status_phys) return -ENOMEM;
    runtime->control_phys = ulsa_alloc_page();
    if (!runtime->control_phys) {
        free_frames(runtime->status_phys, 1);
        runtime->status_phys = 0;
        return -ENOMEM;
    }
    runtime->status  = phys_to_virt(runtime->status_phys);
    runtime->control = phys_to_virt(runtime->control_phys);
    return EOK;
}

/* Free the status and control records. */
void ulsa_pcm_runtime_free_records(ulsa_pcm_runtime_t *runtime)
{
    if (runtime->status_phys) free_frames(runtime->status_phys, 1);
    if (runtime->control_phys) free_frames(runtime->control_phys, 1);
    runtime->status_phys  = 0;
    runtime->control_phys = 0;
    runtime->status       = NULL;
    runtime->control      = NULL;
}

/* Use the core-managed ring buffer for every substream of a PCM. */
void ulsa_pcm_set_managed_buffer_all(ulsa_pcm_t *pcm, uint64_t max_phys, size_t buffer_bytes_max)
{
    for (int dir = 0; dir < 2; dir++) {
        for (ulsa_pcm_substream_t *s = pcm->streams[dir].substream; s; s = s->next) {
            s->managed_buffer   = true;
            s->dma_max_phys     = max_phys;
            s->buffer_bytes_max = buffer_bytes_max ? buffer_bytes_max : 0xffffffffU;
        }
    }
}

/* Make sure the ring buffer holds at least `bytes`; reuses an allocation that is large enough. */
int ulsa_pcm_managed_alloc(ulsa_pcm_substream_t *substream, size_t bytes)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;
    size_t              pages   = (bytes + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    uint64_t            phys;

    if (!bytes || bytes > substream->buffer_bytes_max) return -EINVAL;
    if (runtime->dma_area && runtime->dma_alloc_pages >= pages) {
        runtime->dma_bytes = bytes;
        return EOK;
    }
    ulsa_pcm_managed_free(substream);

    phys = substream->dma_max_phys ? alloc_frames_below(pages, substream->dma_max_phys) : alloc_frames(pages);
    if (!phys) return -ENOMEM;
    runtime->dma_addr        = phys;
    runtime->dma_area        = phys_to_virt(phys);
    runtime->dma_bytes       = bytes;
    runtime->dma_alloc_pages = pages;
    memset(runtime->dma_area, 0, pages * PAGE_4K_SIZE);
    return EOK;
}

/* Free the managed ring buffer. */
void ulsa_pcm_managed_free(ulsa_pcm_substream_t *substream)
{
    ulsa_pcm_runtime_t *runtime = substream->runtime;

    if (!runtime || !runtime->dma_area) return;
    free_frames(runtime->dma_addr, runtime->dma_alloc_pages);
    runtime->dma_area        = NULL;
    runtime->dma_addr        = 0;
    runtime->dma_bytes       = 0;
    runtime->dma_alloc_pages = 0;
}

#endif
