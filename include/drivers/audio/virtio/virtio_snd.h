/*
 *
 *      virtio_snd.h
 *      VirtIO sound (virtio-snd) driver header file
 *
 *      2026/10/04 By Yinyuan34513
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VIRTIO_SND_H_
#define INCLUDE_VIRTIO_SND_H_

#include <libs/std/stdint.h>

/* Device configuration space (virtio specification 5.14.6.1). */
struct virtio_snd_config {
        uint32_t jacks;
        uint32_t streams;
        uint32_t chmaps;
        uint32_t controls;
};

/* Device virtqueue indexes (virtio specification 5.14.6.2). */
#define VIRTIO_SND_VQ_CONTROL 0
#define VIRTIO_SND_VQ_EVENT   1
#define VIRTIO_SND_VQ_TX      2
#define VIRTIO_SND_VQ_RX      3
#define VIRTIO_SND_VQ_MAX     4

/* Dataflow directions. */
#define VIRTIO_SND_D_OUTPUT 0
#define VIRTIO_SND_D_INPUT  1

/* Control request codes. */
#define VIRTIO_SND_R_PCM_INFO       0x0100
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101
#define VIRTIO_SND_R_PCM_PREPARE    0x0102
#define VIRTIO_SND_R_PCM_RELEASE    0x0103
#define VIRTIO_SND_R_PCM_START      0x0104
#define VIRTIO_SND_R_PCM_STOP       0x0105

/* Common status codes returned in the response header. */
#define VIRTIO_SND_S_OK       0x8000
#define VIRTIO_SND_S_BAD_MSG  0x8001
#define VIRTIO_SND_S_NOT_SUPP 0x8002
#define VIRTIO_SND_S_IO_ERR   0x8003

/* Sample formats, as bit indexes into virtio_snd_pcm_info::formats. */
#define VIRTIO_SND_PCM_FMT_U8  4
#define VIRTIO_SND_PCM_FMT_S16 5

/* Frame rates, as bit indexes into virtio_snd_pcm_info::rates. */
#define VIRTIO_SND_PCM_RATE_8000   1
#define VIRTIO_SND_PCM_RATE_11025  2
#define VIRTIO_SND_PCM_RATE_16000  3
#define VIRTIO_SND_PCM_RATE_22050  4
#define VIRTIO_SND_PCM_RATE_32000  5
#define VIRTIO_SND_PCM_RATE_44100  6
#define VIRTIO_SND_PCM_RATE_48000  7
#define VIRTIO_SND_PCM_RATE_64000  8
#define VIRTIO_SND_PCM_RATE_88200  9
#define VIRTIO_SND_PCM_RATE_96000  10
#define VIRTIO_SND_PCM_RATE_176400 11
#define VIRTIO_SND_PCM_RATE_192000 12

/* Common message header. */
struct virtio_snd_hdr {
        uint32_t code;
};

/* Device-to-driver event notification. */
struct virtio_snd_event {
        struct virtio_snd_hdr hdr;
        uint32_t              data;
};

/* Query item information for a range of identifiers. */
struct virtio_snd_query_info {
        struct virtio_snd_hdr hdr;
        uint32_t              start_id;
        uint32_t              count;
        uint32_t              size;
};

/* Common item information header. */
struct virtio_snd_info {
        uint32_t hda_fn_nid;
};

/* PCM control request header (request code plus stream id). */
struct virtio_snd_pcm_hdr {
        struct virtio_snd_hdr hdr;
        uint32_t              stream_id;
};

/* PCM information returned for one stream. */
struct virtio_snd_pcm_info {
        struct virtio_snd_info hdr;
        uint32_t               features;
        uint64_t               formats;
        uint64_t               rates;
        uint8_t                direction;
        uint8_t                channels_min;
        uint8_t                channels_max;
        uint8_t                padding[5];
};

/* Configure one PCM stream (VIRTIO_SND_R_PCM_SET_PARAMS). */
struct virtio_snd_pcm_set_params {
        struct virtio_snd_pcm_hdr hdr;
        uint32_t                  buffer_bytes;
        uint32_t                  period_bytes;
        uint32_t                  features;
        uint8_t                   channels;
        uint8_t                   format;
        uint8_t                   rate;
        uint8_t                   padding;
};

/* I/O request header carried by every TX/RX message. */
struct virtio_snd_pcm_xfer {
        uint32_t stream_id;
};

/* I/O completion status carried back by the device. */
struct virtio_snd_pcm_status {
        uint32_t status;
        uint32_t latency_bytes;
};

/* Probe the sound device and publish a sound card for it. */
#if CONFIG_AUDIO && CONFIG_AUDIO_VIRTIO_SND && CONFIG_VIRTIO_PCI
void virtio_snd_init(void);
#else
static inline void virtio_snd_init(void) {}
#endif

#endif // INCLUDE_VIRTIO_SND_H_
