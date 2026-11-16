/*
 *
 *      pcm_mmap.c
 *      ULSA PCM user-space memory mapping (DMA ring, status, control)
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <libs/std/stdlib.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/page.h>
#include <process/process.h>

#if CONFIG_AUDIO

/* Tracks live DATA VMAs even if the PCM fd is closed before munmap(2). */
struct ulsa_pcm_mmap_tracker {
        spinlock_t          lock;
        int                 refcount;
        ulsa_pcm_runtime_t *runtime;
};

/* Acquire an extra VMA reference when a DATA mapping is forked or split. */
static void ulsa_mmap_vma_get(void *data)
{
    struct ulsa_pcm_mmap_tracker *t = data;

    if (!t) return;
    spin_lock(&t->lock);
    t->refcount++;
    if (t->runtime) __atomic_add_fetch(&t->runtime->mmap_count, 1, __ATOMIC_SEQ_CST);
    spin_unlock(&t->lock);
}

/* Drop a DATA VMA reference on munmap(2) or task exit. */
static void ulsa_mmap_vma_put(void *data)
{
    struct ulsa_pcm_mmap_tracker *t       = data;
    bool                          free_it = false;

    if (!t) return;
    spin_lock(&t->lock);
    if (t->runtime) __atomic_sub_fetch(&t->runtime->mmap_count, 1, __ATOMIC_SEQ_CST);
    if (--t->refcount == 0) free_it = true;
    spin_unlock(&t->lock);
    if (free_it) free(t);
}

/* Map the read-only status record page (OLD and NEW ABI offsets). */
static void *ulsa_pcm_mmap_status(ulsa_pcm_runtime_t *runtime, size_t size, int flags, struct vm_area *vma)
{
    if (!runtime->status || !(flags & VM_READ) || (flags & VM_WRITE)) return NULL;
    if (size != PAGE_4K_SIZE) return NULL;
    vma->end = vma->start + PAGE_4K_SIZE;
    return (void *)runtime->status;
}

/* Map the shared control record page (OLD and NEW ABI offsets). */
static void *ulsa_pcm_mmap_control(ulsa_pcm_runtime_t *runtime, size_t size, int flags, struct vm_area *vma)
{
    if (!runtime->control || !(flags & VM_READ)) return NULL;
    if ((flags & VM_WRITE) && !(flags & VM_SHARED)) return NULL;
    if (size != PAGE_4K_SIZE) return NULL;
    vma->end = vma->start + PAGE_4K_SIZE;
    return (void *)runtime->control;
}

/* Map the DMA ring buffer at offset 0 (or page-aligned subrange) with refcount tracking. */
static void *ulsa_pcm_mmap_data(ulsa_pcm_substream_t *substream, size_t offset, size_t size, int flags, struct vm_area *vma)
{
    ulsa_pcm_runtime_t           *runtime = substream->runtime;
    struct ulsa_pcm_mmap_tracker *cand    = NULL;
    struct ulsa_pcm_mmap_tracker *t;
    size_t                        dma_bytes;
    size_t                        avail;
    size_t                        map_len;
    uint64_t                      lflags;

    const int need = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ? (VM_READ | VM_WRITE) : VM_READ;

    if (!(flags & need)) return NULL;
    if ((flags & VM_WRITE) && !(flags & VM_SHARED)) return NULL;

    /* Pre-allocate the tracker outside the spinlock if this is the first DATA map. */
    lflags = spin_lock_irqsave(&substream->lock);
    if (!runtime->mmap_priv) {
        spin_unlock_irqrestore(&substream->lock, lflags);
        cand = calloc(1, sizeof(*cand));
        if (!cand) return NULL;
        lflags = spin_lock_irqsave(&substream->lock);
    }

    if (runtime->status->state == SNDRV_PCM_STATE_OPEN || runtime->status->state == SNDRV_PCM_STATE_DISCONNECTED || runtime->buffer_accessing < 0) {
        spin_unlock_irqrestore(&substream->lock, lflags);
        free(cand);
        return NULL;
    }
    if (!(runtime->hw.info & SNDRV_PCM_INFO_MMAP) || runtime->access == SNDRV_PCM_ACCESS_RW_INTERLEAVED || runtime->access == SNDRV_PCM_ACCESS_RW_NONINTERLEAVED) {
        spin_unlock_irqrestore(&substream->lock, lflags);
        free(cand);
        return NULL;
    }
    if (!runtime->dma_area || !runtime->dma_bytes) {
        spin_unlock_irqrestore(&substream->lock, lflags);
        free(cand);
        return NULL;
    }

    dma_bytes = ALIGN_UP(runtime->dma_bytes, PAGE_4K_SIZE);
    if (offset >= dma_bytes) {
        spin_unlock_irqrestore(&substream->lock, lflags);
        free(cand);
        return NULL;
    }
    avail   = dma_bytes - offset;
    map_len = size < avail ? size : avail;
    if (!map_len) {
        spin_unlock_irqrestore(&substream->lock, lflags);
        free(cand);
        return NULL;
    }

    if (!runtime->mmap_priv) {
        cand->refcount     = 1; // owned by runtime until detach
        cand->runtime      = runtime;
        runtime->mmap_priv = cand;
        cand               = NULL;
    }
    t = runtime->mmap_priv;
    spin_lock(&t->lock);
    t->refcount++;
    __atomic_add_fetch(&runtime->mmap_count, 1, __ATOMIC_SEQ_CST);
    spin_unlock(&t->lock);
    spin_unlock_irqrestore(&substream->lock, lflags);

    free(cand);
    vma->end             = vma->start + map_len;
    vma->vm_private_data = t;
    vma->vm_private_get  = ulsa_mmap_vma_get;
    vma->vm_private_put  = ulsa_mmap_vma_put;
    return (void *)(runtime->dma_area + offset);
}

/* Report whether the substream's hardware advertises MMAP support. */
bool ulsa_pcm_mmap_supported(const ulsa_pcm_substream_t *substream)
{
    return substream && substream->runtime && (substream->runtime->hw.info & SNDRV_PCM_INFO_MMAP) != 0;
}

/* Detach the runtime from any surviving DATA VMAs when the substream closes. */
void ulsa_pcm_mmap_detach(ulsa_pcm_runtime_t *runtime)
{
    struct ulsa_pcm_mmap_tracker *t;
    bool                          free_it = false;

    if (!runtime || !runtime->mmap_priv) return;
    t                  = runtime->mmap_priv;
    runtime->mmap_priv = NULL;

    spin_lock(&t->lock);
    t->runtime = NULL;
    if (--t->refcount == 0) free_it = true;
    spin_unlock(&t->lock);
    if (free_it) free(t);
}

/* Map the DMA ring buffer or status/control records for user space. */
void *ulsa_pcm_mmap(ulsa_pcm_substream_t *substream, size_t offset, size_t size, int flags, struct vm_area *vma)
{
    ulsa_pcm_runtime_t *runtime;

    if (!substream || !substream->runtime || !vma || !size) return NULL;
    runtime = substream->runtime;

    switch (offset) {
        case SNDRV_PCM_MMAP_OFFSET_STATUS_OLD :
        case SNDRV_PCM_MMAP_OFFSET_STATUS_NEW :
            return ulsa_pcm_mmap_status(runtime, size, flags, vma);
        case SNDRV_PCM_MMAP_OFFSET_CONTROL_OLD :
        case SNDRV_PCM_MMAP_OFFSET_CONTROL_NEW :
            return ulsa_pcm_mmap_control(runtime, size, flags, vma);
        default :
            if (offset >= SNDRV_PCM_MMAP_OFFSET_STATUS_OLD) return NULL;
            return ulsa_pcm_mmap_data(substream, offset, size, flags, vma);
    }
}

#endif
