/*
 *
 *      pcm_mmap.c
 *      ULSA PCM mmap stub (implemented in feat/ulsa-mmap)
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>

#if CONFIG_AUDIO

/* Report whether the substream supports user-space mmap; stub disables mmap so alsa-lib falls back to SYNC_PTR. */
bool ulsa_pcm_mmap_supported(const ulsa_pcm_substream_t *substream)
{
    (void)substream;
    return false;
}

/* Map the DMA ring buffer or status/control records; stub fails with -ENODEV. */
void *ulsa_pcm_mmap(ulsa_pcm_substream_t *substream, size_t offset, size_t size, int flags, struct vm_area *vma)
{
    (void)substream;
    (void)offset;
    (void)size;
    (void)flags;
    (void)vma;
    return NULL;
}

#endif
