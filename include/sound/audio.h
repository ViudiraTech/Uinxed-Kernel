/*
 *
 *      audio.h
 *      Generic audio subsystem interfaces
 *
 *      2026/7/20 By Rainy101112
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SOUND_AUDIO_H_
#define INCLUDE_SOUND_AUDIO_H_

#include <stddef.h>
#include <stdint.h>
#include <tmpfs.h>

#define AUDIO_MAX_CARDS 4
#define AUDIO_NAME_MAX  32

#define AUDIO_IOCTL_GET_INFO   0x4101
#define AUDIO_IOCTL_SET_FORMAT 0x4102
#define AUDIO_IOCTL_STOP       0x4103
#define AUDIO_IOCTL_SET_VOLUME 0x4104
#define AUDIO_IOCTL_GET_VOLUME 0x4105

typedef enum {
    audio_node_control,
    audio_node_pcm_playback,
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

struct audio_card;

typedef struct {
        size_t (*pcm_read)(struct audio_card *card, void *addr, size_t offset, size_t size);
        size_t (*pcm_write)(struct audio_card *card, const void *addr, size_t offset, size_t size);
        int (*set_format)(struct audio_card *card, const audio_pcm_format_t *format);
        int (*stop)(struct audio_card *card);
        int (*set_volume)(struct audio_card *card, const audio_volume_t *volume);
        int (*get_volume)(struct audio_card *card, audio_volume_t *volume);
} audio_card_ops_t;

typedef struct audio_card {
        uint32_t                id;
        char                    name[AUDIO_NAME_MAX];
        audio_pcm_format_t      format;
        const audio_card_ops_t *ops;
        void                   *driver_data;
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

size_t audio_device_read(void *ctx, void *addr, size_t offset, size_t size);
size_t audio_device_write(void *ctx, const void *addr, size_t offset, size_t size);
int    audio_device_poll(void *ctx, size_t events);
int    audio_device_ioctl(void *ctx, size_t req, void *arg);

#endif /* INCLUDE_SOUND_AUDIO_H_ */
