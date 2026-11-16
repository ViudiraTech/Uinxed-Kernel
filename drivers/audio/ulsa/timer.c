/*
 *
 *      timer.c
 *      ULSA timer stub (implemented in feat/ulsa-timer)
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>

#if CONFIG_AUDIO

/* Initialize the global /dev/snd/timer node (no-op stub). */
int ulsa_timer_init(void)
{
    return EOK;
}

/* Attach a slave PCM timer instance to a substream (no-op stub). */
int ulsa_timer_pcm_new(ulsa_pcm_substream_t *substream)
{
    (void)substream;
    return EOK;
}

/* Detach the slave PCM timer instance from a substream (no-op stub). */
void ulsa_timer_pcm_free(ulsa_pcm_substream_t *substream)
{
    (void)substream;
}

/* Notify the slave PCM timer of a stream state event (no-op stub). */
void ulsa_timer_pcm_notify(ulsa_pcm_substream_t *substream, int event)
{
    (void)substream;
    (void)event;
}

/* Notify the slave PCM timer of an elapsed period interrupt (no-op stub). */
void ulsa_timer_pcm_interrupt(ulsa_pcm_substream_t *substream)
{
    (void)substream;
}

/* Update the slave PCM timer resolution after hw_params (no-op stub). */
void ulsa_timer_pcm_resolution_change(ulsa_pcm_substream_t *substream)
{
    (void)substream;
}

#endif
