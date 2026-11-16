/*
 *
 *      internal.h
 *      ULSA core internals shared between the core source files
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_AUDIO_ULSA_INTERNAL_H_
#define INCLUDE_DRIVERS_AUDIO_ULSA_INTERNAL_H_

#include <drivers/audio/ulsa/ulsa.h>
#include <fs/tmpfs/tmpfs.h>
#include <kernel/kdev_t.h>

/* Kinds of /dev/snd nodes. */
enum ulsa_device_type {
    ULSA_DEVICE_CONTROL,
    ULSA_DEVICE_PCM_PLAYBACK,
    ULSA_DEVICE_PCM_CAPTURE,
    ULSA_DEVICE_TIMER,
};

/* One published /dev/snd node; the tmpfs device context points here. */
struct ulsa_minor {
        enum ulsa_device_type type;
        int                   minor;
        ulsa_card_t          *card;
        ulsa_pcm_t           *pcm;
        int                   stream;
        char                  name[24];
        void                 *sysfs_dev;
        bool                  published;
};

/* An open PCM file. */
typedef struct ulsa_pcm_file {
        ulsa_pcm_substream_t *substream;
        unsigned int          user_pversion;
} ulsa_pcm_file_t;

/* Device nodes (core.c). */
int           ulsa_register_device(enum ulsa_device_type type, ulsa_card_t *card, ulsa_pcm_t *pcm, int stream, int dev, const char *name, const tmpfs_device_ops_t *ops, ulsa_minor_t **minor_ret);
void          ulsa_unregister_device(ulsa_minor_t *minor);
int           ulsa_card_count_registered(void);
ulsa_minor_t *ulsa_minor_get(int number);

/* PCM device ops and registration (pcm.c / pcm_native.c). */
extern const tmpfs_device_ops_t ulsa_pcm_device_ops;
int                             ulsa_pcm_register_all(ulsa_card_t *card);
void                            ulsa_pcm_free_all(ulsa_card_t *card);
int                             ulsa_pcm_next_device(ulsa_card_t *card, int device);
int                             ulsa_pcm_info(ulsa_pcm_substream_t *substream, struct snd_pcm_info *info);
int                             ulsa_pcm_open_substream(ulsa_pcm_t *pcm, int stream, int f_flags, ulsa_pcm_substream_t **substream_ret);
void                            ulsa_pcm_release_substream(ulsa_pcm_substream_t *substream);

/* Control device (control.c). */
extern const tmpfs_device_ops_t ulsa_ctl_device_ops;
int                             ulsa_ctl_register(ulsa_card_t *card);
void                            ulsa_ctl_free_all(ulsa_card_t *card);

/* Parameter space (pcm_params.c). */
#define ULSA_MASK_BITS 64
int                        ulsa_mask_empty(const struct snd_mask *mask);
int                        ulsa_mask_single(const struct snd_mask *mask);
unsigned int               ulsa_mask_min(const struct snd_mask *mask);
unsigned int               ulsa_mask_max(const struct snd_mask *mask);
int                        ulsa_mask_test(const struct snd_mask *mask, unsigned int val);
void                       ulsa_mask_any(struct snd_mask *mask);
void                       ulsa_mask_none(struct snd_mask *mask);
void                       ulsa_mask_set(struct snd_mask *mask, unsigned int val);
void                       ulsa_mask_reset(struct snd_mask *mask, unsigned int val);
int                        ulsa_mask_refine(struct snd_mask *mask, const struct snd_mask *v);
void                       ulsa_interval_any(struct snd_interval *i);
int                        ulsa_interval_single(const struct snd_interval *i);
int                        ulsa_interval_value(const struct snd_interval *i);
int                        ulsa_interval_test(const struct snd_interval *i, unsigned int val);
int                        ulsa_interval_refine(struct snd_interval *i, const struct snd_interval *v);
int                        ulsa_interval_setinteger(struct snd_interval *i);
int                        ulsa_interval_list(struct snd_interval *i, unsigned int count, const unsigned int *list, unsigned int mask);
struct snd_mask           *ulsa_hw_param_mask(struct snd_pcm_hw_params *params, snd_pcm_hw_param_t var);
struct snd_interval       *ulsa_hw_param_interval(struct snd_pcm_hw_params *params, snd_pcm_hw_param_t var);
const struct snd_mask     *ulsa_hw_param_mask_c(const struct snd_pcm_hw_params *params, snd_pcm_hw_param_t var);
const struct snd_interval *ulsa_hw_param_interval_c(const struct snd_pcm_hw_params *params, snd_pcm_hw_param_t var);
void                       ulsa_pcm_hw_params_any(struct snd_pcm_hw_params *params);
int                        ulsa_pcm_hw_constraints_init(ulsa_pcm_substream_t *substream);
int                        ulsa_pcm_hw_constraints_complete(ulsa_pcm_substream_t *substream);
void                       ulsa_pcm_hw_constraints_free(ulsa_pcm_runtime_t *runtime);
int                        ulsa_pcm_hw_refine(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params);
int                        ulsa_pcm_hw_params_choose(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params);
int                        ulsa_pcm_fixup_unreferenced_params(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params);

/* Pointer arithmetic and transfers (pcm_lib.c). */
int               ulsa_pcm_update_hw_ptr(ulsa_pcm_substream_t *substream);
int               ulsa_pcm_update_state(ulsa_pcm_substream_t *substream);
void              ulsa_pcm_playback_silence(ulsa_pcm_substream_t *substream, snd_pcm_uframes_t new_hw_ptr);
int               ulsa_pcm_apply_appl_ptr(ulsa_pcm_substream_t *substream, snd_pcm_uframes_t appl_ptr);
snd_pcm_sframes_t ulsa_pcm_lib_xfer(ulsa_pcm_substream_t *substream, void *const *ubufs, bool interleaved, snd_pcm_uframes_t size, bool nonblock);
void              ulsa_pcm_gettime(const ulsa_pcm_runtime_t *runtime, struct snd_timespec *ts);
void              ulsa_pcm_wake_sleepers(ulsa_pcm_substream_t *substream);
void              ulsa_pcm_xrun(ulsa_pcm_substream_t *substream);
int               ulsa_pcm_drain_done(ulsa_pcm_substream_t *substream);
int               ulsa_pcm_start(ulsa_pcm_substream_t *substream);

/* Runtime records and ring buffers (pcm_memory.c). */
int  ulsa_pcm_runtime_alloc_records(ulsa_pcm_runtime_t *runtime);
void ulsa_pcm_runtime_free_records(ulsa_pcm_runtime_t *runtime);
int  ulsa_pcm_managed_alloc(ulsa_pcm_substream_t *substream, size_t bytes);
void ulsa_pcm_managed_free(ulsa_pcm_substream_t *substream);

/*
 * Optional layers.  Each one lives in its own source file behind the small
 * interface below, so the mmap, timer and procfs features can be developed
 * and reviewed independently of the core; until they land, those files only
 * report the feature as absent.
 */

/* pcm_mmap.c: whether the data/status/control records can be mapped, and the mmap entry point. */
bool  ulsa_pcm_mmap_supported(const ulsa_pcm_substream_t *substream);
void *ulsa_pcm_mmap(ulsa_pcm_substream_t *substream, size_t offset, size_t size, int flags, struct vm_area *vma);

/* timer.c: the /dev/snd/timer device and the per-substream PCM timers. */
int  ulsa_timer_init(void);
int  ulsa_timer_pcm_new(ulsa_pcm_substream_t *substream);
void ulsa_timer_pcm_free(ulsa_pcm_substream_t *substream);
void ulsa_timer_pcm_notify(ulsa_pcm_substream_t *substream, int event);
void ulsa_timer_pcm_interrupt(ulsa_pcm_substream_t *substream);
void ulsa_timer_pcm_resolution_change(ulsa_pcm_substream_t *substream);

/* info.c: /proc/asound. */
int  ulsa_info_init(void);
int  ulsa_info_card_register(ulsa_card_t *card);
void ulsa_info_card_free(ulsa_card_t *card);

#endif // INCLUDE_DRIVERS_AUDIO_ULSA_INTERNAL_H_
