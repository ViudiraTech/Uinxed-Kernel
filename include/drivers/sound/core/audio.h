/*
 *
 *      audio.h
 *      Generic audio subsystem interfaces
 *
 *      2026/7/20 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_AUDIO_H_
#define INCLUDE_AUDIO_H_

#include <fs/tmpfs/tmpfs.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <process/task.h>
#include <sync/spin_lock.h>
#include <syscall/poll.h>

#define AUDIO_MAX_CARDS 4
#define AUDIO_NAME_MAX  32

/* ALSA-compatible ioctl numbers */
#define SNDRV_PCM_IOCTL_PVERSION      0x80044100
#define SNDRV_PCM_IOCTL_INFO          0x80044101
#define SNDRV_PCM_IOCTL_HW_PARAMS     0xC0444111
#define SNDRV_PCM_IOCTL_HW_FREE       0x4112
#define SNDRV_PCM_IOCTL_SW_PARAMS     0xC0444113
#define SNDRV_PCM_IOCTL_STATUS        0x80444120
#define SNDRV_PCM_IOCTL_PREPARE       0x4140
#define SNDRV_PCM_IOCTL_START         0x4142
#define SNDRV_PCM_IOCTL_DROP          0x4143
#define SNDRV_PCM_IOCTL_DRAIN         0x4144
#define SNDRV_PCM_IOCTL_PAUSE         0x40044145
#define SNDRV_PCM_IOCTL_SYNC_PTR      0xC0444123
#define SNDRV_PCM_IOCTL_TSTAMP        0x40044102
#define SNDRV_PCM_IOCTL_HWSYNC        0x4122
#define SNDRV_PCM_IOCTL_DELAY         0x80044121
#define SNDRV_PCM_IOCTL_RESET         0x4141
#define SNDRV_PCM_IOCTL_REWIND        0x40044146
#define SNDRV_PCM_IOCTL_FORWARD       0x40044149
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES 0x40044150
#define SNDRV_PCM_IOCTL_READI_FRAMES  0x80044151
#define SNDRV_PCM_IOCTL_LINK          0x40044160
#define SNDRV_PCM_IOCTL_UNLINK        0x4161

/* Legacy custom ioctl numbers */
#define AUDIO_IOCTL_GET_INFO   0x4101
#define AUDIO_IOCTL_SET_FORMAT 0x4102
#define AUDIO_IOCTL_STOP       0x4103
#define AUDIO_IOCTL_SET_VOLUME 0x4104
#define AUDIO_IOCTL_GET_VOLUME 0x4105
#define AUDIO_IOCTL_START      0x4106
#define AUDIO_IOCTL_DRAIN      0x4107
#define AUDIO_IOCTL_GET_POS    0x4108
#define AUDIO_IOCTL_SET_PARAMS 0x4109

/* PCM state */
#define SNDRV_PCM_STATE_OPEN     0
#define SNDRV_PCM_STATE_SETUP    1
#define SNDRV_PCM_STATE_PREPARED 2
#define SNDRV_PCM_STATE_RUNNING  3
#define SNDRV_PCM_STATE_XRUN     4
#define SNDRV_PCM_STATE_DRAINING 5
#define SNDRV_PCM_STATE_PAUSED   6

/* PCM stream type */
#define SNDRV_PCM_STREAM_PLAYBACK       0
#define SNDRV_PCM_STREAM_CAPTURE        1
#define SNDRV_PCM_ACCESS_RW_INTERLEAVED 2
#define SNDRV_PCM_FORMAT_U8             0
#define SNDRV_PCM_FORMAT_S16_LE         2

typedef uint64_t snd_pcm_uframes_t;
typedef int64_t  snd_pcm_sframes_t;

/* ALSA data structures (simplified, layout compatible) */
typedef int snd_pcm_state_t;

struct snd_pcm_info {
        unsigned int  device;
        unsigned int  subdevice;
        int           stream;
        int           card;
        unsigned char id[64];
        unsigned char name[80];
        unsigned char subname[32];
        int           dev_class;
        int           dev_subclass;
        unsigned int  subdevices_count;
        unsigned int  subdevices_avail;
        unsigned char reserved[64];
};

struct snd_pcm_hw_params {
        unsigned int      flags;
        unsigned int      mask;
        unsigned int      fmt_mask;
        unsigned int      msbits;
        unsigned int      rate_num;
        unsigned int      rate_den;
        snd_pcm_uframes_t fifo_size;
        unsigned char     reserved[64];
        unsigned int      access;
        unsigned int      format;
        unsigned int      subformat;
        unsigned int      channels;
        unsigned int      rate;
        unsigned int      period_size;
        unsigned int      buffer_size;
        unsigned int      period_time;
        unsigned int      period_bytes;
        unsigned int      periods;
        unsigned int      buffer_time;
        unsigned int      sample_bits;
        unsigned int      frame_bits;
        unsigned int      ticks;
};

struct snd_pcm_sw_params {
        int               tstamp_mode;
        unsigned int      period_step;
        unsigned int      sleep_min;
        snd_pcm_uframes_t avail_min;
        snd_pcm_uframes_t xfer_align;
        snd_pcm_uframes_t start_threshold;
        snd_pcm_uframes_t stop_threshold;
        snd_pcm_uframes_t silence_threshold;
        snd_pcm_uframes_t silence_size;
        snd_pcm_uframes_t boundary;
        unsigned int      proto;
        unsigned int      tstamp_type;
        unsigned char     reserved[56];
};

struct snd_pcm_status {
        snd_pcm_state_t   state;
        int64_t           trigger_tstamp_sec;
        int64_t           trigger_tstamp_nsec;
        int64_t           tstamp_sec;
        int64_t           tstamp_nsec;
        snd_pcm_uframes_t appl_ptr;
        snd_pcm_uframes_t hw_ptr;
        snd_pcm_sframes_t delay;
        snd_pcm_uframes_t avail;
        snd_pcm_uframes_t avail_max;
        snd_pcm_uframes_t overrange;
        int               suspended;
        int               audio_tstamp_data;
        int64_t           audio_tstamp_sec;
        int64_t           audio_tstamp_nsec;
        int64_t           driver_tstamp_sec;
        int64_t           driver_tstamp_nsec;
        unsigned char     reserved[48];
};

struct snd_pcm_sync_ptr {
        volatile struct snd_pcm_status *status;
        volatile struct snd_pcm_status *control;
};

struct snd_xferi {
        snd_pcm_sframes_t result;
        void             *buf;
        snd_pcm_uframes_t frames;
};

/* Local audio subsystem types */
typedef enum {
    audio_node_control,
    audio_node_pcm_playback,
    audio_node_pcm_capture,
    audio_node_mixer,
} audio_node_type_t;

typedef struct {
        uint32_t sample_rate;
        uint8_t  bits;
        uint8_t  channels;
} audio_pcm_format_t;

typedef struct {
        uint8_t left;
        uint8_t right;
} audio_volume_t;

typedef struct {
        uint32_t card;
        char     name[AUDIO_NAME_MAX];
        uint32_t sample_rate;
        uint8_t  bits;
        uint8_t  channels;
} audio_card_info_t;

/* PCM ring buffer (per-open-file instance) */
typedef struct audio_pcm_file {
        struct audio_card     *card;
        audio_node_type_t      type;
        int                    state;
        audio_pcm_format_t     fmt;
        uint8_t               *ring_buf;
        size_t                 ring_buf_size;
        size_t                 period_bytes;
        snd_pcm_uframes_t      appl_ptr;
        snd_pcm_uframes_t      hw_ptr;
        snd_pcm_uframes_t      boundary;
        snd_pcm_uframes_t      period_size;
        snd_pcm_uframes_t      avail_min;
        snd_pcm_uframes_t      start_threshold;
        spinlock_t             lock;
        wait_queue_t           read_wait;
        wait_queue_t           write_wait;
        int                    nonblock;
        int                    period_event;
        int                    xrun;
        struct audio_pcm_file *next;
} audio_pcm_file_t;

struct audio_card;

typedef struct {
        size_t (*pcm_read)(struct audio_card *card, void *addr, size_t offset, size_t size);
        size_t (*pcm_write)(struct audio_card *card, const void *addr, size_t offset, size_t size);
        int (*set_format)(struct audio_card *card, const audio_pcm_format_t *format);
        int (*start)(struct audio_card *card);
        int (*stop)(struct audio_card *card);
        int (*drain)(struct audio_card *card);
        int (*set_volume)(struct audio_card *card, const audio_volume_t *volume);
        int (*get_volume)(struct audio_card *card, audio_volume_t *volume);
        int (*get_position)(struct audio_card *card, snd_pcm_uframes_t *pos);
        int (*get_avail)(struct audio_card *card, size_t *avail);
        int (*set_params)(struct audio_card *card, const audio_pcm_format_t *fmt, size_t buffer_bytes, size_t period_bytes);
} audio_card_ops_t;

typedef struct audio_card {
        uint32_t                id;
        char                    name[AUDIO_NAME_MAX];
        audio_pcm_format_t      format;
        const audio_card_ops_t *ops;
        void                   *driver_data;
        audio_volume_t          volume;
        audio_pcm_file_t       *pcm_files;
        spinlock_t              pcm_lock;
} audio_card_t;

typedef struct {
        audio_card_t      *card;
        audio_node_type_t  type;
        const char        *name;
        tmpfs_device_ops_t tmpfs_ops;
} audio_device_node_t;

int           audio_register_card(const char *name, const audio_pcm_format_t *format, const audio_card_ops_t *ops, void *driver_data);
audio_card_t *audio_get_card(uint32_t card);
size_t        audio_card_count(void);

size_t               audio_device_node_count(void);
audio_device_node_t *audio_get_device_node(size_t index);

/* Per-open instance callbacks */
int     audio_file_open(vfs_node_t node, uint64_t flags, void **private_data);
void    audio_file_release(vfs_node_t node, void *private_data);
int64_t audio_file_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size);
int64_t audio_file_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size);
int     audio_file_poll(void *ctx, void *private_data, uint64_t flags, size_t events);
int     audio_file_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg);

/* Legacy device callbacks */
size_t audio_device_read(void *ctx, void *addr, size_t offset, size_t size);
size_t audio_device_write(void *ctx, const void *addr, size_t offset, size_t size);
int    audio_device_poll(void *ctx, size_t events);
int    audio_device_ioctl(void *ctx, size_t req, void *arg);

/* PCM ring buffer helpers */
int               pcm_ring_buffer_init(audio_pcm_file_t *pf, size_t size_frames);
void              pcm_ring_buffer_destroy(audio_pcm_file_t *pf);
size_t            pcm_ring_buffer_write_frames(audio_pcm_file_t *pf, const void *data, size_t frames);
size_t            pcm_ring_buffer_read_frames(audio_pcm_file_t *pf, void *data, size_t frames);
snd_pcm_sframes_t pcm_ring_buffer_avail(audio_pcm_file_t *pf);
snd_pcm_sframes_t pcm_ring_buffer_space(audio_pcm_file_t *pf);
void              pcm_ring_buffer_advance_hw(audio_pcm_file_t *pf, snd_pcm_uframes_t frames);

/* Mixer helpers */
size_t audio_mix_interleaved_s16(int16_t *dst, const int16_t *src, size_t frames, unsigned int channels);

#endif // INCLUDE_AUDIO_H_
