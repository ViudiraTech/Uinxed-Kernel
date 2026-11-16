/*
 *
 *      pcm.c
 *      ULSA PCM devices and substreams
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>
#include <kernel/debug/vsprintf.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>

#if CONFIG_AUDIO

/* Create the substreams of one direction. */
static int pcm_new_stream(ulsa_pcm_t *pcm, int stream, int count)
{
    ulsa_pcm_str_t        *pstr = &pcm->streams[stream];
    ulsa_pcm_substream_t **link = &pstr->substream;

    pstr->stream          = stream;
    pstr->pcm             = pcm;
    pstr->substream_count = (unsigned int)count;
    for (int i = 0; i < count; i++) {
        ulsa_pcm_substream_t *s = calloc(1, sizeof(*s));
        if (!s) return -ENOMEM;
        s->pcm    = pcm;
        s->pstr   = pstr;
        s->number = i;
        s->stream = stream;
        mutex_init(&s->buffer_mutex);
        (void)snprintf(s->name, sizeof(s->name), "subdevice #%d", i);
        *link = s;
        link  = &s->next;
    }
    return EOK;
}

/* Free the substreams of one direction. */
static void pcm_free_stream(ulsa_pcm_str_t *pstr)
{
    ulsa_pcm_substream_t *s = pstr->substream;

    while (s) {
        ulsa_pcm_substream_t *next = s->next;
        free(s);
        s = next;
    }
    pstr->substream       = NULL;
    pstr->substream_count = 0;
}

/* Create PCM device `device` on a card. */
int ulsa_pcm_new(ulsa_card_t *card, const char *id, int device, int playback_count, int capture_count, ulsa_pcm_t **pcm_ret)
{
    ulsa_pcm_t *pcm;
    int         err;

    if (!card || device < 0 || device >= SNDRV_MINOR_PCMS || card->pcms[device]) return -EINVAL;
    if (playback_count < 0 || capture_count < 0 || (!playback_count && !capture_count)) return -EINVAL;
    pcm = calloc(1, sizeof(*pcm));
    if (!pcm) return -ENOMEM;
    pcm->card   = card;
    pcm->device = device;
    mutex_init(&pcm->open_mutex);
    wait_queue_init(&pcm->open_wait);
    strncpy(pcm->id, id ? id : card->driver, sizeof(pcm->id) - 1);
    strncpy(pcm->name, pcm->id, sizeof(pcm->name) - 1);

    err = pcm_new_stream(pcm, SNDRV_PCM_STREAM_PLAYBACK, playback_count);
    if (err == EOK) err = pcm_new_stream(pcm, SNDRV_PCM_STREAM_CAPTURE, capture_count);
    if (err != EOK) {
        pcm_free_stream(&pcm->streams[0]);
        pcm_free_stream(&pcm->streams[1]);
        free(pcm);
        return err;
    }
    card->pcms[device] = pcm;
    if (pcm_ret) *pcm_ret = pcm;
    return EOK;
}

/* Attach the operations of one direction. */
void ulsa_pcm_set_ops(ulsa_pcm_t *pcm, int direction, const ulsa_pcm_ops_t *ops)
{
    for (ulsa_pcm_substream_t *s = pcm->streams[direction].substream; s; s = s->next) s->ops = ops;
}

/* Publish pcmC<card>D<dev>{p,c} for every PCM of a card. */
int ulsa_pcm_register_all(ulsa_card_t *card)
{
    for (int dev = 0; dev < SNDRV_MINOR_PCMS; dev++) {
        ulsa_pcm_t *pcm = card->pcms[dev];
        if (!pcm) continue;
        for (int stream = 0; stream < 2; stream++) {
            ulsa_pcm_str_t *pstr = &pcm->streams[stream];
            char            name[24];
            int             err;

            if (!pstr->substream_count || pstr->minor) continue;
            for (ulsa_pcm_substream_t *s = pstr->substream; s; s = s->next) {
                if (!s->ops) return -EINVAL;
                if (!s->buffer_bytes_max) s->buffer_bytes_max = 0xffffffffU;
            }
            (void)snprintf(name, sizeof(name), "pcmC%dD%d%c", card->number, dev, stream == SNDRV_PCM_STREAM_PLAYBACK ? 'p' : 'c');
            err = ulsa_register_device(stream == SNDRV_PCM_STREAM_PLAYBACK ? ULSA_DEVICE_PCM_PLAYBACK : ULSA_DEVICE_PCM_CAPTURE, card, pcm, stream, dev, name, &ulsa_pcm_device_ops, &pstr->minor);
            if (err < 0) return err;
        }
    }
    return EOK;
}

/* Unpublish and free every PCM of a card. */
void ulsa_pcm_free_all(ulsa_card_t *card)
{
    for (int dev = 0; dev < SNDRV_MINOR_PCMS; dev++) {
        ulsa_pcm_t *pcm = card->pcms[dev];
        if (!pcm) continue;
        for (int stream = 0; stream < 2; stream++) {
            if (pcm->streams[stream].minor) ulsa_unregister_device(pcm->streams[stream].minor);
            pcm->streams[stream].minor = NULL;
            pcm_free_stream(&pcm->streams[stream]);
        }
        if (pcm->private_free) pcm->private_free(pcm);
        card->pcms[dev] = NULL;
        free(pcm);
    }
}

/* SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE: the next PCM device after `device`, or -1. */
int ulsa_pcm_next_device(ulsa_card_t *card, int device)
{
    for (int dev = device < 0 ? 0 : device + 1; dev < SNDRV_MINOR_PCMS; dev++)
        if (card->pcms[dev]) return dev;
    return -1;
}

#endif
