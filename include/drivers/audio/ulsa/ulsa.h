/*
 *
 *      ulsa.h
 *      ULSA (Uinxed Linux-compatible Sound Architecture) driver interface
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_AUDIO_ULSA_ULSA_H_
#define INCLUDE_DRIVERS_AUDIO_ULSA_ULSA_H_

#include <drivers/audio/ulsa/asound.h>
#include <fs/core/vfs.h>
#include <libs/std/errno.h>
#include <libs/std/stdbool.h>
#include <process/task.h>
#include <sync/mutex.h>
#include <sync/spin_lock.h>

/*
 * ULSA follows the Linux ALSA driver model closely so that drivers can be
 * reasoned about with the Linux sources at hand:
 *
 *   card      - one sound card; owns the control interface and the PCMs.
 *   pcm       - one PCM device (pcmC<card>D<dev>{p,c}), with a playback
 *               and/or a capture stream made of substreams.
 *   runtime   - per-open state of a substream: negotiated parameters, the
 *               DMA ring buffer, and the hw/appl pointers user space sees.
 *   kcontrol  - one mixer element exposed through controlC<card>.
 *
 * Drivers own the hardware ring buffer (normally allocated through the
 * managed-buffer helper) and only report where the hardware is in it
 * (ops->pointer) and when a period elapsed (ulsa_pcm_period_elapsed).
 */

typedef struct ulsa_card          ulsa_card_t;
typedef struct ulsa_pcm           ulsa_pcm_t;
typedef struct ulsa_pcm_str       ulsa_pcm_str_t;
typedef struct ulsa_pcm_substream ulsa_pcm_substream_t;
typedef struct ulsa_pcm_runtime   ulsa_pcm_runtime_t;
typedef struct ulsa_kcontrol      ulsa_kcontrol_t;
typedef struct ulsa_ctl_file      ulsa_ctl_file_t;
typedef struct ulsa_pcm_hw_rule   ulsa_pcm_hw_rule_t;
typedef struct ulsa_minor         ulsa_minor_t;

/* ---------------------------------------------------------------- card */

#define ULSA_CARD_ID_LEN 16

struct ulsa_card {
        int  number;
        char id[16];
        char driver[16];
        char shortname[32];
        char longname[80];
        char mixername[80];
        char components[128];

        void *private_data;
        void (*private_free)(ulsa_card_t *card);
        bool registered;

        /* control elements */
        mutex_t          controls_lock;
        ulsa_kcontrol_t *controls;
        unsigned int     controls_count;
        unsigned int     last_numid;
        size_t           user_ctl_alloc_size;

        /* open control files (event subscribers) */
        spinlock_t       files_lock;
        ulsa_ctl_file_t *ctl_files;

        ulsa_pcm_t *pcms[SNDRV_MINOR_PCMS];

        /* published device nodes (minor table entries) */
        ulsa_minor_t *ctl_minor;

        /* private storage of the sysfs and procfs layers */
        void *sysfs_priv;
        void *info_priv;
};

/* Allocate a card; idx < 0 picks the first free slot.  The card stays invisible until registered. */
int ulsa_card_new(int idx, const char *id, ulsa_card_t **card_ret);

/* Publish the card, its control node and every PCM created on it. */
int ulsa_card_register(ulsa_card_t *card);

/* Unpublish (if needed) and free a card with everything attached to it. */
void ulsa_card_free(ulsa_card_t *card);

/* Look up a registered card by number, or NULL. */
ulsa_card_t *ulsa_card_get(int number);

/* Append a component tag ("HDA:10ec0269 ...") to card->components. */
int ulsa_component_add(ulsa_card_t *card, const char *component);

/* ----------------------------------------------------------------- PCM */

/* Rate bits of ulsa_pcm_hardware_t.rates, in the Linux SNDRV_PCM_RATE_* order. */
#define ULSA_PCM_RATE_5512        (1U << 0)
#define ULSA_PCM_RATE_8000        (1U << 1)
#define ULSA_PCM_RATE_11025       (1U << 2)
#define ULSA_PCM_RATE_16000       (1U << 3)
#define ULSA_PCM_RATE_22050       (1U << 4)
#define ULSA_PCM_RATE_32000       (1U << 5)
#define ULSA_PCM_RATE_44100       (1U << 6)
#define ULSA_PCM_RATE_48000       (1U << 7)
#define ULSA_PCM_RATE_64000       (1U << 8)
#define ULSA_PCM_RATE_88200       (1U << 9)
#define ULSA_PCM_RATE_96000       (1U << 10)
#define ULSA_PCM_RATE_176400      (1U << 11)
#define ULSA_PCM_RATE_192000      (1U << 12)
#define ULSA_PCM_RATE_352800      (1U << 13)
#define ULSA_PCM_RATE_384000      (1U << 14)
#define ULSA_PCM_RATE_705600      (1U << 15)
#define ULSA_PCM_RATE_768000      (1U << 16)
#define ULSA_PCM_RATE_12000       (1U << 17)
#define ULSA_PCM_RATE_24000       (1U << 18)
#define ULSA_PCM_RATE_128000      (1U << 19)
#define ULSA_PCM_RATE_CONTINUOUS  (1U << 30)
#define ULSA_PCM_RATE_KNOT        (1U << 31)
#define ULSA_PCM_RATE_8000_48000  (ULSA_PCM_RATE_8000 | ULSA_PCM_RATE_11025 | ULSA_PCM_RATE_16000 | ULSA_PCM_RATE_22050 | ULSA_PCM_RATE_32000 | ULSA_PCM_RATE_44100 | ULSA_PCM_RATE_48000)
#define ULSA_PCM_RATE_8000_192000 (ULSA_PCM_RATE_8000_48000 | ULSA_PCM_RATE_64000 | ULSA_PCM_RATE_88200 | ULSA_PCM_RATE_96000 | ULSA_PCM_RATE_176400 | ULSA_PCM_RATE_192000)

#define ULSA_PCM_FMTBIT(fmt) (1ULL << (fmt))

/* Trigger commands handed to ops->trigger (atomic context, stream lock held). */
#define ULSA_PCM_TRIGGER_STOP          0
#define ULSA_PCM_TRIGGER_START         1
#define ULSA_PCM_TRIGGER_PAUSE_PUSH    3
#define ULSA_PCM_TRIGGER_PAUSE_RELEASE 4
#define ULSA_PCM_TRIGGER_SUSPEND       5
#define ULSA_PCM_TRIGGER_RESUME        6
#define ULSA_PCM_TRIGGER_DRAIN         7

/* Returned by ops->pointer when the hardware detected an xrun. */
#define ULSA_PCM_POS_XRUN ((snd_pcm_uframes_t) - 1)

/* What the hardware can do; the core turns it into hw_params constraints at open. */
typedef struct ulsa_pcm_hardware {
        unsigned int info;
        uint64_t     formats;
        uint32_t     subformats;
        unsigned int rates;
        unsigned int rate_min;
        unsigned int rate_max;
        unsigned int channels_min;
        unsigned int channels_max;
        size_t       buffer_bytes_max;
        size_t       period_bytes_min;
        size_t       period_bytes_max;
        unsigned int periods_min;
        unsigned int periods_max;
        size_t       fifo_size;
} ulsa_pcm_hardware_t;

typedef int (*ulsa_pcm_hw_rule_func_t)(struct snd_pcm_hw_params *params, ulsa_pcm_hw_rule_t *rule);

struct ulsa_pcm_hw_rule {
        unsigned int            cond;
        int                     var;
        int                     deps[5];
        ulsa_pcm_hw_rule_func_t func;
        void *private;
};

typedef struct ulsa_pcm_hw_constraints {
        struct snd_mask     masks[SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK + 1];
        struct snd_interval intervals[SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1];
        unsigned int        rules_num;
        unsigned int        rules_all;
        ulsa_pcm_hw_rule_t *rules;
} ulsa_pcm_hw_constraints_t;

typedef struct ulsa_pcm_hw_constraint_list {
        unsigned int        count;
        const unsigned int *list;
        unsigned int        mask;
} ulsa_pcm_hw_constraint_list_t;

typedef struct ulsa_ratnum {
        unsigned int num;
        unsigned int den_min, den_max, den_step;
} ulsa_ratnum_t;

typedef struct ulsa_pcm_hw_constraint_ratnums {
        unsigned int         nrats;
        const ulsa_ratnum_t *rats;
} ulsa_pcm_hw_constraint_ratnums_t;

struct ulsa_pcm_runtime {
        /* The records user space mirrors: each lives in its own page so that it can be mapped. */
        volatile struct snd_pcm_mmap_status  *status;
        volatile struct snd_pcm_mmap_control *control;
        uint64_t                              status_phys;
        uint64_t                              control_phys;

        /* hardware description and constraints, filled by the driver's open() */
        ulsa_pcm_hardware_t       hw;
        ulsa_pcm_hw_constraints_t hw_constraints;

        /* negotiated hardware parameters */
        snd_pcm_access_t    access;
        snd_pcm_format_t    format;
        snd_pcm_subformat_t subformat;
        unsigned int        channels;
        unsigned int        rate;
        snd_pcm_uframes_t   period_size;
        unsigned int        periods;
        snd_pcm_uframes_t   buffer_size;
        snd_pcm_uframes_t   min_align;
        size_t              byte_align;
        unsigned int        frame_bits;
        unsigned int        sample_bits;
        unsigned int        info;
        unsigned int        rate_num;
        unsigned int        rate_den;
        bool                no_period_wakeup;

        /* software parameters */
        int               tstamp_mode;
        unsigned int      tstamp_type;
        unsigned int      period_step;
        snd_pcm_uframes_t start_threshold;
        snd_pcm_uframes_t stop_threshold;
        snd_pcm_uframes_t silence_threshold;
        snd_pcm_uframes_t silence_size;
        snd_pcm_uframes_t boundary;

        /* pointer bookkeeping */
        snd_pcm_uframes_t   hw_ptr_base;
        snd_pcm_uframes_t   hw_ptr_interrupt;
        uint64_t            hw_ptr_wrap;
        uint64_t            hw_ptr_ns;
        uint64_t            hw_ptr_buffer_ns;
        snd_pcm_uframes_t   avail_max;
        snd_pcm_uframes_t   overrange;
        snd_pcm_uframes_t   silence_start;
        snd_pcm_uframes_t   silence_filled;
        snd_pcm_sframes_t   delay;
        snd_pcm_uframes_t   twake;
        struct snd_timespec trigger_tstamp;
        struct snd_timespec driver_tstamp;

        /* DMA ring buffer */
        uint8_t *dma_area;
        uint64_t dma_addr;
        size_t   dma_bytes;
        size_t   dma_alloc_pages;

        /* waiters: sleep = poll/drain/avail_min, tsleep = blocking transfers */
        wait_queue_t      sleep;
        wait_queue_t      tsleep;
        vfs_poll_source_t poll_source;

        int  buffer_accessing; // > 0 while copying, < 0 while hw_params/hw_free run
        int  mmap_count;
        bool stop_operating;

        void *private_data;
        void (*private_free)(ulsa_pcm_runtime_t *runtime);
};

typedef struct ulsa_pcm_ops {
        int (*open)(ulsa_pcm_substream_t *substream);
        int (*close)(ulsa_pcm_substream_t *substream);
        int (*hw_params)(ulsa_pcm_substream_t *substream, struct snd_pcm_hw_params *params);
        int (*hw_free)(ulsa_pcm_substream_t *substream);
        int (*prepare)(ulsa_pcm_substream_t *substream);
        int (*trigger)(ulsa_pcm_substream_t *substream, int cmd);
        snd_pcm_uframes_t (*pointer)(ulsa_pcm_substream_t *substream);
        int (*ack)(ulsa_pcm_substream_t *substream);
} ulsa_pcm_ops_t;

struct ulsa_pcm_substream {
        ulsa_pcm_t           *pcm;
        ulsa_pcm_str_t       *pstr;
        int                   number;
        int                   stream;
        char                  name[32];
        const ulsa_pcm_ops_t *ops;
        ulsa_pcm_runtime_t   *runtime;
        void                 *private_data;
        ulsa_pcm_substream_t *next;

        spinlock_t lock;         // stream lock: runtime pointers and state (IRQ safe)
        mutex_t    buffer_mutex; // serialises hw_params/hw_free/prepare against each other
        bool       opened;
        int        f_flags;

        /* managed buffer: allocated by the core at hw_params time */
        bool     managed_buffer;
        uint64_t dma_max_phys;
        size_t   buffer_bytes_max;

        void *timer; // PCM timer instance owned by the timer layer
        bool  timer_running;
};

struct ulsa_pcm_str {
        int                   stream;
        ulsa_pcm_t           *pcm;
        unsigned int          substream_count;
        unsigned int          substream_opened;
        ulsa_pcm_substream_t *substream;
        ulsa_minor_t         *minor;
};

struct ulsa_pcm {
        ulsa_card_t   *card;
        int            device;
        char           id[64];
        char           name[80];
        int            dev_class;
        int            dev_subclass;
        ulsa_pcm_str_t streams[2];
        mutex_t        open_mutex;
        wait_queue_t   open_wait;
        void          *private_data;
        void (*private_free)(ulsa_pcm_t *pcm);
};

/* Create PCM device number `device` with the given substream counts. */
int ulsa_pcm_new(ulsa_card_t *card, const char *id, int device, int playback_count, int capture_count, ulsa_pcm_t **pcm_ret);

/* Attach the operations of one stream direction. */
void ulsa_pcm_set_ops(ulsa_pcm_t *pcm, int direction, const ulsa_pcm_ops_t *ops);

/* Let the core allocate the ring buffer at hw_params time, below max_phys (exclusive). */
void ulsa_pcm_set_managed_buffer_all(ulsa_pcm_t *pcm, uint64_t max_phys, size_t buffer_bytes_max);

/* Report that the hardware finished a period; safe from interrupt context. */
void ulsa_pcm_period_elapsed(ulsa_pcm_substream_t *substream);

/* Same, for callers that already hold the stream lock. */
void ulsa_pcm_period_elapsed_under_stream_lock(ulsa_pcm_substream_t *substream);

/* Stop a running stream in the given state (e.g. XRUN); stream lock must be held. */
int ulsa_pcm_stop(ulsa_pcm_substream_t *substream, snd_pcm_state_t state);

/* Force an xrun from a driver (takes the stream lock). */
int ulsa_pcm_stop_xrun(ulsa_pcm_substream_t *substream);

/* Stream-lock helpers. */
uint64_t ulsa_pcm_stream_lock_irqsave(ulsa_pcm_substream_t *substream);
void     ulsa_pcm_stream_unlock_irqrestore(ulsa_pcm_substream_t *substream, uint64_t flags);

/* Constraint helpers for ops->open. */
int ulsa_pcm_hw_constraint_mask64(ulsa_pcm_runtime_t *runtime, snd_pcm_hw_param_t var, uint64_t mask);
int ulsa_pcm_hw_constraint_integer(ulsa_pcm_runtime_t *runtime, snd_pcm_hw_param_t var);
int ulsa_pcm_hw_constraint_minmax(ulsa_pcm_runtime_t *runtime, snd_pcm_hw_param_t var, unsigned int min, unsigned int max);
int ulsa_pcm_hw_constraint_list(ulsa_pcm_runtime_t *runtime, unsigned int cond, snd_pcm_hw_param_t var, const ulsa_pcm_hw_constraint_list_t *list);
int ulsa_pcm_hw_constraint_ratnums(ulsa_pcm_runtime_t *runtime, unsigned int cond, snd_pcm_hw_param_t var, const ulsa_pcm_hw_constraint_ratnums_t *r);
int ulsa_pcm_hw_constraint_step(ulsa_pcm_runtime_t *runtime, unsigned int cond, snd_pcm_hw_param_t var, unsigned long step);
int ulsa_pcm_hw_constraint_pow2(ulsa_pcm_runtime_t *runtime, unsigned int cond, snd_pcm_hw_param_t var);
int ulsa_pcm_hw_constraint_msbits(ulsa_pcm_runtime_t *runtime, unsigned int cond, unsigned int width, unsigned int msbits);
int ulsa_pcm_hw_rule_add(ulsa_pcm_runtime_t *runtime, unsigned int cond, int var, ulsa_pcm_hw_rule_func_t func, void *private, int dep, ...);

/* hw_params accessors (valid once a parameter is fixed). */
unsigned int        ulsa_params_channels(const struct snd_pcm_hw_params *p);
unsigned int        ulsa_params_rate(const struct snd_pcm_hw_params *p);
snd_pcm_format_t    ulsa_params_format(const struct snd_pcm_hw_params *p);
snd_pcm_access_t    ulsa_params_access(const struct snd_pcm_hw_params *p);
snd_pcm_subformat_t ulsa_params_subformat(const struct snd_pcm_hw_params *p);
size_t              ulsa_params_period_bytes(const struct snd_pcm_hw_params *p);
size_t              ulsa_params_buffer_bytes(const struct snd_pcm_hw_params *p);
unsigned int        ulsa_params_periods(const struct snd_pcm_hw_params *p);
unsigned int        ulsa_params_period_size(const struct snd_pcm_hw_params *p);
unsigned int        ulsa_params_buffer_size(const struct snd_pcm_hw_params *p);

/* Frame availability helpers (caller holds substream->lock). */
snd_pcm_uframes_t ulsa_pcm_playback_avail(const ulsa_pcm_runtime_t *runtime);
snd_pcm_uframes_t ulsa_pcm_capture_avail(const ulsa_pcm_runtime_t *runtime);
snd_pcm_uframes_t ulsa_pcm_avail(const ulsa_pcm_substream_t *substream);
snd_pcm_sframes_t ulsa_pcm_hw_avail(const ulsa_pcm_substream_t *substream);

/* Format helpers. */
int  ulsa_pcm_format_width(snd_pcm_format_t format);
int  ulsa_pcm_format_physical_width(snd_pcm_format_t format);
int  ulsa_pcm_format_signed(snd_pcm_format_t format);
int  ulsa_pcm_format_linear(snd_pcm_format_t format);
int  ulsa_pcm_format_set_silence(snd_pcm_format_t format, void *buf, size_t samples);
bool ulsa_pcm_running(const ulsa_pcm_substream_t *substream);

/* Byte/frame conversions of a configured runtime. */
static inline size_t ulsa_frames_to_bytes(const ulsa_pcm_runtime_t *runtime, snd_pcm_sframes_t frames)
{
    return (size_t)frames * runtime->frame_bits / 8;
}
static inline snd_pcm_sframes_t ulsa_bytes_to_frames(const ulsa_pcm_runtime_t *runtime, size_t bytes)
{
    return (snd_pcm_sframes_t)(bytes * 8 / runtime->frame_bits);
}
static inline size_t ulsa_pcm_lib_buffer_bytes(const ulsa_pcm_substream_t *substream)
{
    return ulsa_frames_to_bytes(substream->runtime, (snd_pcm_sframes_t)substream->runtime->buffer_size);
}
static inline size_t ulsa_pcm_lib_period_bytes(const ulsa_pcm_substream_t *substream)
{
    return ulsa_frames_to_bytes(substream->runtime, (snd_pcm_sframes_t)substream->runtime->period_size);
}

/* ------------------------------------------------------------- control */

typedef int (*ulsa_kcontrol_info_t)(ulsa_kcontrol_t *kcontrol, struct snd_ctl_elem_info *uinfo);
typedef int (*ulsa_kcontrol_get_t)(ulsa_kcontrol_t *kcontrol, struct snd_ctl_elem_value *ucontrol);
typedef int (*ulsa_kcontrol_put_t)(ulsa_kcontrol_t *kcontrol, struct snd_ctl_elem_value *ucontrol);

/* Template for ulsa_ctl_new1(); put() returns 1 when the value changed. */
typedef struct ulsa_kcontrol_new {
        snd_ctl_elem_iface_t iface;
        unsigned int         device;
        unsigned int         subdevice;
        const char          *name;
        unsigned int         index;
        unsigned int         access;
        unsigned int         count;
        ulsa_kcontrol_info_t info;
        ulsa_kcontrol_get_t  get;
        ulsa_kcontrol_put_t  put;
        const unsigned int  *tlv;
        unsigned long        private_value;
} ulsa_kcontrol_new_t;

typedef struct ulsa_kcontrol_volatile {
        ulsa_ctl_file_t *owner;
        unsigned int     access;
} ulsa_kcontrol_volatile_t;

struct ulsa_kcontrol {
        ulsa_kcontrol_t       *next;
        struct snd_ctl_elem_id id;
        unsigned int           count;
        ulsa_kcontrol_info_t   info;
        ulsa_kcontrol_get_t    get;
        ulsa_kcontrol_put_t    put;
        const unsigned int    *tlv;
        int (*tlv_callback)(ulsa_kcontrol_t *kcontrol, int op_flag, unsigned int size, unsigned int *user_tlv);
        unsigned long private_value;
        void         *private_data;
        void (*private_free)(ulsa_kcontrol_t *kcontrol);
        ulsa_kcontrol_volatile_t vd[];
};

/* Build a control from a template; private_data is handed to its callbacks. */
ulsa_kcontrol_t *ulsa_ctl_new1(const ulsa_kcontrol_new_t *knew, void *private_data);

/* Attach a control to a card (takes ownership, frees it on failure). */
int ulsa_ctl_add(ulsa_card_t *card, ulsa_kcontrol_t *kcontrol);

/* Remove and free a control. */
int ulsa_ctl_remove(ulsa_card_t *card, ulsa_kcontrol_t *kcontrol);

/* Find a control by name/iface/index, or NULL. */
ulsa_kcontrol_t *ulsa_ctl_find(ulsa_card_t *card, snd_ctl_elem_iface_t iface, const char *name, unsigned int index);

/* Queue an event for every subscriber (value/info/tlv changes); safe from interrupt context. */
void ulsa_ctl_notify(ulsa_card_t *card, unsigned int mask, const struct snd_ctl_elem_id *id);

/* Standard info callbacks. */
int ulsa_ctl_boolean_mono_info(ulsa_kcontrol_t *kcontrol, struct snd_ctl_elem_info *uinfo);
int ulsa_ctl_boolean_stereo_info(ulsa_kcontrol_t *kcontrol, struct snd_ctl_elem_info *uinfo);
int ulsa_ctl_enum_info(struct snd_ctl_elem_info *info, unsigned int channels, unsigned int items, const char *const names[]);

/* Build a DB_SCALE TLV: min and step in 1/100 dB, mute = lowest step is silence. */
#define ULSA_TLV_DB_SCALE(name, min, step, mute) \
    static const unsigned int name[] = {SNDRV_CTL_TLVT_DB_SCALE, 2 * sizeof(unsigned int), (unsigned int)(min), ((step) & SNDRV_CTL_TLVD_DB_SCALE_MASK) | ((mute) ? SNDRV_CTL_TLVD_DB_SCALE_MUTE : 0)}

/* ------------------------------------------------------------- general */

/* Bring up the ULSA core (global nodes); called once before any driver probes. */
#if CONFIG_AUDIO
void ulsa_init(void);
#else
static inline void ulsa_init(void) {}
#endif

#endif // INCLUDE_DRIVERS_AUDIO_ULSA_ULSA_H_
