/*
 *
 *      asound.h
 *      ULSA user-space ABI (binary compatible with Linux ALSA on x86-64)
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_AUDIO_ULSA_ASOUND_H_
#define INCLUDE_DRIVERS_AUDIO_ULSA_ASOUND_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <syscall/ioctl.h>

/*
 * Every structure, constant and ioctl number below is part of the ALSA
 * kernel/user ABI that alsa-lib, tinyalsa, PulseAudio and PipeWire are built
 * against.  The layouts are those an x86-64 user space sees; the sizes and
 * ioctl numbers are pinned by the static assertions at the end of the file,
 * whose values were taken from the system <sound/asound.h> (Linux 7.3).
 */

#define SNDRV_PROTOCOL_VERSION(major, minor, subminor) (((major) << 16) | ((minor) << 8) | (subminor))
#define SNDRV_PROTOCOL_MAJOR(version)                  (((version) >> 16) & 0xffff)
#define SNDRV_PROTOCOL_MINOR(version)                  (((version) >> 8) & 0xff)
#define SNDRV_PROTOCOL_MICRO(version)                  ((version) & 0xff)

/* Character device major shared by every sound node. */
#define SNDRV_MAJOR 116

/* Static minor layout: 32 minors per card, plus the global devices. */
#define SNDRV_MINOR_DEVICES       32
#define SNDRV_MINOR_CARD(minor)   ((minor) >> 5)
#define SNDRV_MINOR_DEVICE(minor) ((minor) & 0x1f)
#define SNDRV_MINOR(card, dev)    (((card) << 5) | (dev))
#define SNDRV_MINOR_CONTROL       0
#define SNDRV_MINOR_SEQUENCER     1
#define SNDRV_MINOR_TIMER         33
#define SNDRV_MINOR_HWDEP         4
#define SNDRV_MINOR_RAWMIDI       8
#define SNDRV_MINOR_PCM_PLAYBACK  16
#define SNDRV_MINOR_PCM_CAPTURE   24
#define SNDRV_MINOR_PCMS          8
#define SNDRV_CARDS               8

/* ------------------------------------------------------------------ PCM */

#define SNDRV_PCM_VERSION SNDRV_PROTOCOL_VERSION(2, 0, 18)

typedef unsigned long snd_pcm_uframes_t;
typedef signed long   snd_pcm_sframes_t;
typedef int           snd_pcm_state_t;
typedef int           snd_pcm_access_t;
typedef int           snd_pcm_format_t;
typedef int           snd_pcm_subformat_t;
typedef int           snd_pcm_hw_param_t;

#define SNDRV_PCM_CLASS_GENERIC        0
#define SNDRV_PCM_CLASS_MULTI          1
#define SNDRV_PCM_CLASS_MODEM          2
#define SNDRV_PCM_CLASS_DIGITIZER      3
#define SNDRV_PCM_SUBCLASS_GENERIC_MIX 0

#define SNDRV_PCM_STREAM_PLAYBACK 0
#define SNDRV_PCM_STREAM_CAPTURE  1
#define SNDRV_PCM_STREAM_LAST     SNDRV_PCM_STREAM_CAPTURE

#define SNDRV_PCM_ACCESS_MMAP_INTERLEAVED    0
#define SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED 1
#define SNDRV_PCM_ACCESS_MMAP_COMPLEX        2
#define SNDRV_PCM_ACCESS_RW_INTERLEAVED      3
#define SNDRV_PCM_ACCESS_RW_NONINTERLEAVED   4
#define SNDRV_PCM_ACCESS_LAST                SNDRV_PCM_ACCESS_RW_NONINTERLEAVED

#define SNDRV_PCM_FORMAT_S8                 0
#define SNDRV_PCM_FORMAT_U8                 1
#define SNDRV_PCM_FORMAT_S16_LE             2
#define SNDRV_PCM_FORMAT_S16_BE             3
#define SNDRV_PCM_FORMAT_U16_LE             4
#define SNDRV_PCM_FORMAT_U16_BE             5
#define SNDRV_PCM_FORMAT_S24_LE             6
#define SNDRV_PCM_FORMAT_S24_BE             7
#define SNDRV_PCM_FORMAT_U24_LE             8
#define SNDRV_PCM_FORMAT_U24_BE             9
#define SNDRV_PCM_FORMAT_S32_LE             10
#define SNDRV_PCM_FORMAT_S32_BE             11
#define SNDRV_PCM_FORMAT_U32_LE             12
#define SNDRV_PCM_FORMAT_U32_BE             13
#define SNDRV_PCM_FORMAT_FLOAT_LE           14
#define SNDRV_PCM_FORMAT_FLOAT_BE           15
#define SNDRV_PCM_FORMAT_FLOAT64_LE         16
#define SNDRV_PCM_FORMAT_FLOAT64_BE         17
#define SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE 18
#define SNDRV_PCM_FORMAT_IEC958_SUBFRAME_BE 19
#define SNDRV_PCM_FORMAT_MU_LAW             20
#define SNDRV_PCM_FORMAT_A_LAW              21
#define SNDRV_PCM_FORMAT_IMA_ADPCM          22
#define SNDRV_PCM_FORMAT_MPEG               23
#define SNDRV_PCM_FORMAT_GSM                24
#define SNDRV_PCM_FORMAT_S20_LE             25
#define SNDRV_PCM_FORMAT_S20_BE             26
#define SNDRV_PCM_FORMAT_U20_LE             27
#define SNDRV_PCM_FORMAT_U20_BE             28
#define SNDRV_PCM_FORMAT_SPECIAL            31
#define SNDRV_PCM_FORMAT_S24_3LE            32
#define SNDRV_PCM_FORMAT_S24_3BE            33
#define SNDRV_PCM_FORMAT_U24_3LE            34
#define SNDRV_PCM_FORMAT_U24_3BE            35
#define SNDRV_PCM_FORMAT_S20_3LE            36
#define SNDRV_PCM_FORMAT_S20_3BE            37
#define SNDRV_PCM_FORMAT_U20_3LE            38
#define SNDRV_PCM_FORMAT_U20_3BE            39
#define SNDRV_PCM_FORMAT_S18_3LE            40
#define SNDRV_PCM_FORMAT_S18_3BE            41
#define SNDRV_PCM_FORMAT_U18_3LE            42
#define SNDRV_PCM_FORMAT_U18_3BE            43
#define SNDRV_PCM_FORMAT_G723_24            44
#define SNDRV_PCM_FORMAT_G723_24_1B         45
#define SNDRV_PCM_FORMAT_G723_40            46
#define SNDRV_PCM_FORMAT_G723_40_1B         47
#define SNDRV_PCM_FORMAT_DSD_U8             48
#define SNDRV_PCM_FORMAT_DSD_U16_LE         49
#define SNDRV_PCM_FORMAT_DSD_U32_LE         50
#define SNDRV_PCM_FORMAT_DSD_U16_BE         51
#define SNDRV_PCM_FORMAT_DSD_U32_BE         52
#define SNDRV_PCM_FORMAT_LAST               SNDRV_PCM_FORMAT_DSD_U32_BE

#define SNDRV_PCM_SUBFORMAT_STD        0
#define SNDRV_PCM_SUBFORMAT_MSBITS_MAX 1
#define SNDRV_PCM_SUBFORMAT_MSBITS_20  2
#define SNDRV_PCM_SUBFORMAT_MSBITS_24  3
#define SNDRV_PCM_SUBFORMAT_LAST       SNDRV_PCM_SUBFORMAT_MSBITS_24

#define SNDRV_PCM_INFO_MMAP             0x00000001
#define SNDRV_PCM_INFO_MMAP_VALID       0x00000002
#define SNDRV_PCM_INFO_DOUBLE           0x00000004
#define SNDRV_PCM_INFO_BATCH            0x00000010
#define SNDRV_PCM_INFO_SYNC_APPLPTR     0x00000020
#define SNDRV_PCM_INFO_PERFECT_DRAIN    0x00000040
#define SNDRV_PCM_INFO_INTERLEAVED      0x00000100
#define SNDRV_PCM_INFO_NONINTERLEAVED   0x00000200
#define SNDRV_PCM_INFO_COMPLEX          0x00000400
#define SNDRV_PCM_INFO_BLOCK_TRANSFER   0x00010000
#define SNDRV_PCM_INFO_OVERRANGE        0x00020000
#define SNDRV_PCM_INFO_RESUME           0x00040000
#define SNDRV_PCM_INFO_PAUSE            0x00080000
#define SNDRV_PCM_INFO_HALF_DUPLEX      0x00100000
#define SNDRV_PCM_INFO_JOINT_DUPLEX     0x00200000
#define SNDRV_PCM_INFO_SYNC_START       0x00400000
#define SNDRV_PCM_INFO_NO_PERIOD_WAKEUP 0x00800000
#define SNDRV_PCM_INFO_EXPLICIT_SYNC    0x10000000
#define SNDRV_PCM_INFO_NO_REWINDS       0x20000000
#define SNDRV_PCM_INFO_DRAIN_TRIGGER    0x40000000
#define SNDRV_PCM_INFO_FIFO_IN_FRAMES   0x80000000

#define SNDRV_PCM_STATE_OPEN         0
#define SNDRV_PCM_STATE_SETUP        1
#define SNDRV_PCM_STATE_PREPARED     2
#define SNDRV_PCM_STATE_RUNNING      3
#define SNDRV_PCM_STATE_XRUN         4
#define SNDRV_PCM_STATE_DRAINING     5
#define SNDRV_PCM_STATE_PAUSED       6
#define SNDRV_PCM_STATE_SUSPENDED    7
#define SNDRV_PCM_STATE_DISCONNECTED 8
#define SNDRV_PCM_STATE_LAST         SNDRV_PCM_STATE_DISCONNECTED

#define SNDRV_PCM_MMAP_OFFSET_DATA        0x00000000UL
#define SNDRV_PCM_MMAP_OFFSET_STATUS_OLD  0x80000000UL
#define SNDRV_PCM_MMAP_OFFSET_CONTROL_OLD 0x81000000UL
#define SNDRV_PCM_MMAP_OFFSET_STATUS_NEW  0x82000000UL
#define SNDRV_PCM_MMAP_OFFSET_CONTROL_NEW 0x83000000UL

#define SNDRV_PCM_HW_PARAM_ACCESS            0
#define SNDRV_PCM_HW_PARAM_FORMAT            1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT         2
#define SNDRV_PCM_HW_PARAM_FIRST_MASK        SNDRV_PCM_HW_PARAM_ACCESS
#define SNDRV_PCM_HW_PARAM_LAST_MASK         SNDRV_PCM_HW_PARAM_SUBFORMAT
#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS       8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS        9
#define SNDRV_PCM_HW_PARAM_CHANNELS          10
#define SNDRV_PCM_HW_PARAM_RATE              11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME       12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE       13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES      14
#define SNDRV_PCM_HW_PARAM_PERIODS           15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME       16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE       17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES      18
#define SNDRV_PCM_HW_PARAM_TICK_TIME         19
#define SNDRV_PCM_HW_PARAM_FIRST_INTERVAL    SNDRV_PCM_HW_PARAM_SAMPLE_BITS
#define SNDRV_PCM_HW_PARAM_LAST_INTERVAL     SNDRV_PCM_HW_PARAM_TICK_TIME
#define SNDRV_PCM_HW_PARAMS_NORESAMPLE       (1 << 0)
#define SNDRV_PCM_HW_PARAMS_EXPORT_BUFFER    (1 << 1)
#define SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP (1 << 2)
#define SNDRV_PCM_HW_PARAMS_NO_DRAIN_SILENCE (1 << 3)

#define SNDRV_PCM_TSTAMP_NONE               0
#define SNDRV_PCM_TSTAMP_ENABLE             1
#define SNDRV_PCM_TSTAMP_LAST               SNDRV_PCM_TSTAMP_ENABLE
#define SNDRV_PCM_TSTAMP_TYPE_GETTIMEOFDAY  0
#define SNDRV_PCM_TSTAMP_TYPE_MONOTONIC     1
#define SNDRV_PCM_TSTAMP_TYPE_MONOTONIC_RAW 2
#define SNDRV_PCM_TSTAMP_TYPE_LAST          SNDRV_PCM_TSTAMP_TYPE_MONOTONIC_RAW

#define SNDRV_PCM_SYNC_PTR_HWSYNC    (1 << 0)
#define SNDRV_PCM_SYNC_PTR_APPL      (1 << 1)
#define SNDRV_PCM_SYNC_PTR_AVAIL_MIN (1 << 2)

#define SNDRV_MASK_MAX 256

/* Linux-compatible 64-bit timespec as user space lays it out. */
struct snd_timespec {
        int64_t tv_sec;
        int64_t tv_nsec;
};

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
        unsigned char pad1[16];
        unsigned char reserved[64];
};

struct snd_interval {
        unsigned int min, max;
        unsigned int openmin : 1, openmax : 1, integer : 1, empty : 1;
};

struct snd_mask {
        uint32_t bits[(SNDRV_MASK_MAX + 31) / 32];
};

struct snd_pcm_hw_params {
        unsigned int        flags;
        struct snd_mask     masks[SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK + 1];
        struct snd_mask     mres[5];
        struct snd_interval intervals[SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1];
        struct snd_interval ires[9];
        unsigned int        rmask;
        unsigned int        cmask;
        unsigned int        info;
        unsigned int        msbits;
        unsigned int        rate_num;
        unsigned int        rate_den;
        snd_pcm_uframes_t   fifo_size;
        unsigned char       sync[16];
        unsigned char       reserved[48];
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

struct snd_pcm_channel_info {
        unsigned int channel;
        long         offset;
        unsigned int first;
        unsigned int step;
};

struct snd_pcm_status {
        snd_pcm_state_t     state;
        int                 pad1;
        struct snd_timespec trigger_tstamp;
        struct snd_timespec tstamp;
        snd_pcm_uframes_t   appl_ptr;
        snd_pcm_uframes_t   hw_ptr;
        snd_pcm_sframes_t   delay;
        snd_pcm_uframes_t   avail;
        snd_pcm_uframes_t   avail_max;
        snd_pcm_uframes_t   overrange;
        snd_pcm_state_t     suspended_state;
        uint32_t            audio_tstamp_data;
        struct snd_timespec audio_tstamp;
        struct snd_timespec driver_tstamp;
        uint32_t            audio_tstamp_accuracy;
        unsigned char       reserved[52 - 2 * sizeof(struct snd_timespec)];
};

/* The record a user maps at SNDRV_PCM_MMAP_OFFSET_STATUS, also embedded in SYNC_PTR. */
struct snd_pcm_mmap_status {
        snd_pcm_state_t     state;
        int                 pad1;
        snd_pcm_uframes_t   hw_ptr;
        struct snd_timespec tstamp;
        snd_pcm_state_t     suspended_state;
        struct snd_timespec audio_tstamp;
};

/* The record a user maps at SNDRV_PCM_MMAP_OFFSET_CONTROL, also embedded in SYNC_PTR. */
struct snd_pcm_mmap_control {
        snd_pcm_uframes_t appl_ptr;
        snd_pcm_uframes_t avail_min;
};

struct snd_pcm_sync_ptr {
        unsigned int flags;
        union {
                struct snd_pcm_mmap_status status;
                unsigned char              reserved[64];
        } s;
        union {
                struct snd_pcm_mmap_control control;
                unsigned char               reserved[64];
        } c;
};

struct snd_xferi {
        snd_pcm_sframes_t result;
        void             *buf;
        snd_pcm_uframes_t frames;
};

struct snd_xfern {
        snd_pcm_sframes_t result;
        void            **bufs;
        snd_pcm_uframes_t frames;
};

#define SNDRV_PCM_IOCTL_PVERSION      _IOR('A', 0x00, int)
#define SNDRV_PCM_IOCTL_INFO          _IOR('A', 0x01, struct snd_pcm_info)
#define SNDRV_PCM_IOCTL_TSTAMP        _IOW('A', 0x02, int)
#define SNDRV_PCM_IOCTL_TTSTAMP       _IOW('A', 0x03, int)
#define SNDRV_PCM_IOCTL_USER_PVERSION _IOW('A', 0x04, int)
#define SNDRV_PCM_IOCTL_HW_REFINE     _IOWR('A', 0x10, struct snd_pcm_hw_params)
#define SNDRV_PCM_IOCTL_HW_PARAMS     _IOWR('A', 0x11, struct snd_pcm_hw_params)
#define SNDRV_PCM_IOCTL_HW_FREE       _IO('A', 0x12)
#define SNDRV_PCM_IOCTL_SW_PARAMS     _IOWR('A', 0x13, struct snd_pcm_sw_params)
#define SNDRV_PCM_IOCTL_STATUS        _IOR('A', 0x20, struct snd_pcm_status)
#define SNDRV_PCM_IOCTL_DELAY         _IOR('A', 0x21, snd_pcm_sframes_t)
#define SNDRV_PCM_IOCTL_HWSYNC        _IO('A', 0x22)
#define SNDRV_PCM_IOCTL_SYNC_PTR      _IOWR('A', 0x23, struct snd_pcm_sync_ptr)
#define SNDRV_PCM_IOCTL_STATUS_EXT    _IOWR('A', 0x24, struct snd_pcm_status)
#define SNDRV_PCM_IOCTL_CHANNEL_INFO  _IOR('A', 0x32, struct snd_pcm_channel_info)
#define SNDRV_PCM_IOCTL_PREPARE       _IO('A', 0x40)
#define SNDRV_PCM_IOCTL_RESET         _IO('A', 0x41)
#define SNDRV_PCM_IOCTL_START         _IO('A', 0x42)
#define SNDRV_PCM_IOCTL_DROP          _IO('A', 0x43)
#define SNDRV_PCM_IOCTL_DRAIN         _IO('A', 0x44)
#define SNDRV_PCM_IOCTL_PAUSE         _IOW('A', 0x45, int)
#define SNDRV_PCM_IOCTL_REWIND        _IOW('A', 0x46, snd_pcm_uframes_t)
#define SNDRV_PCM_IOCTL_RESUME        _IO('A', 0x47)
#define SNDRV_PCM_IOCTL_XRUN          _IO('A', 0x48)
#define SNDRV_PCM_IOCTL_FORWARD       _IOW('A', 0x49, snd_pcm_uframes_t)
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES _IOW('A', 0x50, struct snd_xferi)
#define SNDRV_PCM_IOCTL_READI_FRAMES  _IOR('A', 0x51, struct snd_xferi)
#define SNDRV_PCM_IOCTL_WRITEN_FRAMES _IOW('A', 0x52, struct snd_xfern)
#define SNDRV_PCM_IOCTL_READN_FRAMES  _IOR('A', 0x53, struct snd_xfern)
#define SNDRV_PCM_IOCTL_LINK          _IOW('A', 0x60, int)
#define SNDRV_PCM_IOCTL_UNLINK        _IO('A', 0x61)

/* channel positions used by the PCM channel-map controls */
#define SNDRV_CHMAP_UNKNOWN 0
#define SNDRV_CHMAP_NA      1
#define SNDRV_CHMAP_MONO    2
#define SNDRV_CHMAP_FL      3
#define SNDRV_CHMAP_FR      4
#define SNDRV_CHMAP_RL      5
#define SNDRV_CHMAP_RR      6
#define SNDRV_CHMAP_FC      7
#define SNDRV_CHMAP_LFE     8
#define SNDRV_CHMAP_SL      9
#define SNDRV_CHMAP_SR      10

/* -------------------------------------------------------------- timer */

#define SNDRV_TIMER_VERSION SNDRV_PROTOCOL_VERSION(2, 0, 8)

#define SNDRV_TIMER_CLASS_NONE   (-1)
#define SNDRV_TIMER_CLASS_SLAVE  0
#define SNDRV_TIMER_CLASS_GLOBAL 1
#define SNDRV_TIMER_CLASS_CARD   2
#define SNDRV_TIMER_CLASS_PCM    3
#define SNDRV_TIMER_CLASS_LAST   SNDRV_TIMER_CLASS_PCM

#define SNDRV_TIMER_SCLASS_NONE          0
#define SNDRV_TIMER_SCLASS_APPLICATION   1
#define SNDRV_TIMER_SCLASS_SEQUENCER     2
#define SNDRV_TIMER_SCLASS_OSS_SEQUENCER 3
#define SNDRV_TIMER_SCLASS_LAST          SNDRV_TIMER_SCLASS_OSS_SEQUENCER

#define SNDRV_TIMER_GLOBAL_SYSTEM  0
#define SNDRV_TIMER_GLOBAL_RTC     1
#define SNDRV_TIMER_GLOBAL_HPET    2
#define SNDRV_TIMER_GLOBAL_HRTIMER 3

#define SNDRV_TIMER_FLG_SLAVE (1 << 0)

#define SNDRV_TIMER_PSFLG_AUTO        (1 << 0)
#define SNDRV_TIMER_PSFLG_EXCLUSIVE   (1 << 1)
#define SNDRV_TIMER_PSFLG_EARLY_EVENT (1 << 2)

#define SNDRV_TIMER_EVENT_RESOLUTION 0
#define SNDRV_TIMER_EVENT_TICK       1
#define SNDRV_TIMER_EVENT_START      2
#define SNDRV_TIMER_EVENT_STOP       3
#define SNDRV_TIMER_EVENT_CONTINUE   4
#define SNDRV_TIMER_EVENT_PAUSE      5
#define SNDRV_TIMER_EVENT_EARLY      6
#define SNDRV_TIMER_EVENT_SUSPEND    7
#define SNDRV_TIMER_EVENT_RESUME     8
#define SNDRV_TIMER_EVENT_MSTART     (SNDRV_TIMER_EVENT_START + 10)
#define SNDRV_TIMER_EVENT_MSTOP      (SNDRV_TIMER_EVENT_STOP + 10)
#define SNDRV_TIMER_EVENT_MCONTINUE  (SNDRV_TIMER_EVENT_CONTINUE + 10)
#define SNDRV_TIMER_EVENT_MPAUSE     (SNDRV_TIMER_EVENT_PAUSE + 10)
#define SNDRV_TIMER_EVENT_MSUSPEND   (SNDRV_TIMER_EVENT_SUSPEND + 10)
#define SNDRV_TIMER_EVENT_MRESUME    (SNDRV_TIMER_EVENT_RESUME + 10)

struct snd_timer_id {
        int dev_class;
        int dev_sclass;
        int card;
        int device;
        int subdevice;
};

struct snd_timer_ginfo {
        struct snd_timer_id tid;
        unsigned int        flags;
        int                 card;
        unsigned char       id[64];
        unsigned char       name[80];
        unsigned long       reserved0;
        unsigned long       resolution;
        unsigned long       resolution_min;
        unsigned long       resolution_max;
        unsigned int        clients;
        unsigned char       reserved[32];
};

struct snd_timer_gparams {
        struct snd_timer_id tid;
        unsigned long       period_num;
        unsigned long       period_den;
        unsigned char       reserved[32];
};

struct snd_timer_gstatus {
        struct snd_timer_id tid;
        unsigned long       resolution;
        unsigned long       resolution_num;
        unsigned long       resolution_den;
        unsigned char       reserved[32];
};

struct snd_timer_select {
        struct snd_timer_id id;
        unsigned char       reserved[32];
};

struct snd_timer_info {
        unsigned int  flags;
        int           card;
        unsigned char id[64];
        unsigned char name[80];
        unsigned long reserved0;
        unsigned long resolution;
        unsigned char reserved[64];
};

struct snd_timer_params {
        unsigned int  flags;
        unsigned int  ticks;
        unsigned int  queue_size;
        unsigned int  reserved0;
        unsigned int  filter;
        unsigned char reserved[60];
};

struct snd_timer_status {
        struct snd_timespec tstamp;
        unsigned int        resolution;
        unsigned int        lost;
        unsigned int        overrun;
        unsigned int        queue;
        unsigned char       reserved[64];
};

struct snd_timer_uinfo {
        uint64_t      resolution;
        int           fd;
        unsigned int  id;
        unsigned char reserved[16];
};

struct snd_timer_read {
        unsigned int resolution;
        unsigned int ticks;
};

struct snd_timer_tread {
        int                 event;
        int                 pad1;
        struct snd_timespec tstamp;
        unsigned int        val;
        int                 pad2;
};

#define SNDRV_TIMER_IOCTL_PVERSION    _IOR('T', 0x00, int)
#define SNDRV_TIMER_IOCTL_NEXT_DEVICE _IOWR('T', 0x01, struct snd_timer_id)
#define SNDRV_TIMER_IOCTL_TREAD_OLD   _IOW('T', 0x02, int)
#define SNDRV_TIMER_IOCTL_GINFO       _IOWR('T', 0x03, struct snd_timer_ginfo)
#define SNDRV_TIMER_IOCTL_GPARAMS     _IOW('T', 0x04, struct snd_timer_gparams)
#define SNDRV_TIMER_IOCTL_GSTATUS     _IOWR('T', 0x05, struct snd_timer_gstatus)
#define SNDRV_TIMER_IOCTL_SELECT      _IOW('T', 0x10, struct snd_timer_select)
#define SNDRV_TIMER_IOCTL_INFO        _IOR('T', 0x11, struct snd_timer_info)
#define SNDRV_TIMER_IOCTL_PARAMS      _IOW('T', 0x12, struct snd_timer_params)
#define SNDRV_TIMER_IOCTL_STATUS      _IOR('T', 0x14, struct snd_timer_status)
#define SNDRV_TIMER_IOCTL_START       _IO('T', 0xa0)
#define SNDRV_TIMER_IOCTL_STOP        _IO('T', 0xa1)
#define SNDRV_TIMER_IOCTL_CONTINUE    _IO('T', 0xa2)
#define SNDRV_TIMER_IOCTL_PAUSE       _IO('T', 0xa3)
#define SNDRV_TIMER_IOCTL_TREAD64     _IOW('T', 0xa4, int)
#define SNDRV_TIMER_IOCTL_CREATE      _IOWR('T', 0xa5, struct snd_timer_uinfo)
#define SNDRV_TIMER_IOCTL_TRIGGER     _IO('T', 0xa6)

/* ------------------------------------------------------------ control */

#define SNDRV_CTL_VERSION SNDRV_PROTOCOL_VERSION(2, 0, 10)

struct snd_ctl_card_info {
        int           card;
        int           pad;
        unsigned char id[16];
        unsigned char driver[16];
        unsigned char name[32];
        unsigned char longname[80];
        unsigned char reserved_[16];
        unsigned char mixername[80];
        unsigned char components[128];
};

#define SND_CTL_CARD_BTYPE_COMPONENTS 1

struct snd_ctl_card_bytes {
        uint32_t type;
        uint32_t data_allocated;
        uint32_t data_len;
        uint32_t reserved;
        uint64_t data;
};

typedef int snd_ctl_elem_type_t;
typedef int snd_ctl_elem_iface_t;

#define SNDRV_CTL_ELEM_TYPE_NONE       0
#define SNDRV_CTL_ELEM_TYPE_BOOLEAN    1
#define SNDRV_CTL_ELEM_TYPE_INTEGER    2
#define SNDRV_CTL_ELEM_TYPE_ENUMERATED 3
#define SNDRV_CTL_ELEM_TYPE_BYTES      4
#define SNDRV_CTL_ELEM_TYPE_IEC958     5
#define SNDRV_CTL_ELEM_TYPE_INTEGER64  6
#define SNDRV_CTL_ELEM_TYPE_LAST       SNDRV_CTL_ELEM_TYPE_INTEGER64

#define SNDRV_CTL_ELEM_IFACE_CARD      0
#define SNDRV_CTL_ELEM_IFACE_HWDEP     1
#define SNDRV_CTL_ELEM_IFACE_MIXER     2
#define SNDRV_CTL_ELEM_IFACE_PCM       3
#define SNDRV_CTL_ELEM_IFACE_RAWMIDI   4
#define SNDRV_CTL_ELEM_IFACE_TIMER     5
#define SNDRV_CTL_ELEM_IFACE_SEQUENCER 6
#define SNDRV_CTL_ELEM_IFACE_LAST      SNDRV_CTL_ELEM_IFACE_SEQUENCER

#define SNDRV_CTL_ELEM_ACCESS_READ          (1 << 0)
#define SNDRV_CTL_ELEM_ACCESS_WRITE         (1 << 1)
#define SNDRV_CTL_ELEM_ACCESS_READWRITE     (SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_WRITE)
#define SNDRV_CTL_ELEM_ACCESS_VOLATILE      (1 << 2)
#define SNDRV_CTL_ELEM_ACCESS_TLV_READ      (1 << 4)
#define SNDRV_CTL_ELEM_ACCESS_TLV_WRITE     (1 << 5)
#define SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE (SNDRV_CTL_ELEM_ACCESS_TLV_READ | SNDRV_CTL_ELEM_ACCESS_TLV_WRITE)
#define SNDRV_CTL_ELEM_ACCESS_TLV_COMMAND   (1 << 6)
#define SNDRV_CTL_ELEM_ACCESS_INACTIVE      (1 << 8)
#define SNDRV_CTL_ELEM_ACCESS_LOCK          (1 << 9)
#define SNDRV_CTL_ELEM_ACCESS_OWNER         (1 << 10)
#define SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK  (1 << 28)
#define SNDRV_CTL_ELEM_ACCESS_USER          (1 << 29)

#define SNDRV_CTL_POWER_D0 0x0000

#define SNDRV_CTL_ELEM_ID_NAME_MAXLEN 44

struct snd_ctl_elem_id {
        unsigned int         numid;
        snd_ctl_elem_iface_t iface;
        unsigned int         device;
        unsigned int         subdevice;
        unsigned char        name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
        unsigned int         index;
};

struct snd_ctl_elem_list {
        unsigned int            offset;
        unsigned int            space;
        unsigned int            used;
        unsigned int            count;
        struct snd_ctl_elem_id *pids;
        unsigned char           reserved[50];
};

struct snd_ctl_elem_info {
        struct snd_ctl_elem_id id;
        snd_ctl_elem_type_t    type;
        unsigned int           access;
        unsigned int           count;
        int                    owner;
        union {
                struct {
                        long min;
                        long max;
                        long step;
                } integer;
                struct {
                        long long min;
                        long long max;
                        long long step;
                } integer64;
                struct {
                        unsigned int items;
                        unsigned int item;
                        char         name[64];
                        uint64_t     names_ptr;
                        unsigned int names_length;
                } enumerated;
                unsigned char reserved[128];
        } value;
        unsigned char reserved[64];
};

struct snd_ctl_elem_value {
        struct snd_ctl_elem_id id;
        unsigned int           indirect : 1;
        union {
                union {
                        long  value[128];
                        long *value_ptr;
                } integer;
                union {
                        long long  value[64];
                        long long *value_ptr;
                } integer64;
                union {
                        unsigned int  item[128];
                        unsigned int *item_ptr;
                } enumerated;
                union {
                        unsigned char  data[512];
                        unsigned char *data_ptr;
                } bytes;
                unsigned char iec958[400];
        } value;
        unsigned char reserved[128];
};

struct snd_ctl_tlv {
        unsigned int numid;
        unsigned int length;
        unsigned int tlv[];
};

#define SNDRV_CTL_IOCTL_PVERSION                 _IOR('U', 0x00, int)
#define SNDRV_CTL_IOCTL_CARD_INFO                _IOR('U', 0x01, struct snd_ctl_card_info)
#define SNDRV_CTL_IOCTL_CARD_BYTES               _IOWR('U', 0x02, struct snd_ctl_card_bytes)
#define SNDRV_CTL_IOCTL_ELEM_LIST                _IOWR('U', 0x10, struct snd_ctl_elem_list)
#define SNDRV_CTL_IOCTL_ELEM_INFO                _IOWR('U', 0x11, struct snd_ctl_elem_info)
#define SNDRV_CTL_IOCTL_ELEM_READ                _IOWR('U', 0x12, struct snd_ctl_elem_value)
#define SNDRV_CTL_IOCTL_ELEM_WRITE               _IOWR('U', 0x13, struct snd_ctl_elem_value)
#define SNDRV_CTL_IOCTL_ELEM_LOCK                _IOW('U', 0x14, struct snd_ctl_elem_id)
#define SNDRV_CTL_IOCTL_ELEM_UNLOCK              _IOW('U', 0x15, struct snd_ctl_elem_id)
#define SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS         _IOWR('U', 0x16, int)
#define SNDRV_CTL_IOCTL_ELEM_ADD                 _IOWR('U', 0x17, struct snd_ctl_elem_info)
#define SNDRV_CTL_IOCTL_ELEM_REPLACE             _IOWR('U', 0x18, struct snd_ctl_elem_info)
#define SNDRV_CTL_IOCTL_ELEM_REMOVE              _IOWR('U', 0x19, struct snd_ctl_elem_id)
#define SNDRV_CTL_IOCTL_TLV_READ                 _IOWR('U', 0x1a, struct snd_ctl_tlv)
#define SNDRV_CTL_IOCTL_TLV_WRITE                _IOWR('U', 0x1b, struct snd_ctl_tlv)
#define SNDRV_CTL_IOCTL_TLV_COMMAND              _IOWR('U', 0x1c, struct snd_ctl_tlv)
#define SNDRV_CTL_IOCTL_HWDEP_NEXT_DEVICE        _IOWR('U', 0x20, int)
#define SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE          _IOR('U', 0x30, int)
#define SNDRV_CTL_IOCTL_PCM_INFO                 _IOWR('U', 0x31, struct snd_pcm_info)
#define SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE     _IOW('U', 0x32, int)
#define SNDRV_CTL_IOCTL_RAWMIDI_NEXT_DEVICE      _IOWR('U', 0x40, int)
#define SNDRV_CTL_IOCTL_RAWMIDI_PREFER_SUBDEVICE _IOW('U', 0x42, int)
#define SNDRV_CTL_IOCTL_UMP_NEXT_DEVICE          _IOWR('U', 0x43, int)
#define SNDRV_CTL_IOCTL_POWER                    _IOWR('U', 0xd0, int)
#define SNDRV_CTL_IOCTL_POWER_STATE              _IOR('U', 0xd1, int)

#define SNDRV_CTL_EVENT_ELEM        0
#define SNDRV_CTL_EVENT_MASK_VALUE  (1 << 0)
#define SNDRV_CTL_EVENT_MASK_INFO   (1 << 1)
#define SNDRV_CTL_EVENT_MASK_ADD    (1 << 2)
#define SNDRV_CTL_EVENT_MASK_TLV    (1 << 3)
#define SNDRV_CTL_EVENT_MASK_REMOVE (~0U)

struct snd_ctl_event {
        int type;
        union {
                struct {
                        unsigned int           mask;
                        struct snd_ctl_elem_id id;
                } elem;
                unsigned char data8[60];
        } data;
};

/* TLV (type-length-value) metadata attached to controls. */
#define SNDRV_CTL_TLVT_CONTAINER      0
#define SNDRV_CTL_TLVT_DB_SCALE       1
#define SNDRV_CTL_TLVT_DB_LINEAR      2
#define SNDRV_CTL_TLVT_DB_RANGE       3
#define SNDRV_CTL_TLVT_DB_MINMAX      4
#define SNDRV_CTL_TLVT_DB_MINMAX_MUTE 5
#define SNDRV_CTL_TLVT_CHMAP_FIXED    0x101
#define SNDRV_CTL_TLVT_CHMAP_VAR      0x102
#define SNDRV_CTL_TLVT_CHMAP_PAIRED   0x103
#define SNDRV_CTL_TLVD_DB_SCALE_MASK  0xffff
#define SNDRV_CTL_TLVD_DB_SCALE_MUTE  0x10000

#define SNDRV_CTL_TLV_OP_READ  0
#define SNDRV_CTL_TLV_OP_WRITE 1
#define SNDRV_CTL_TLV_OP_CMD   (-1)

/* --------------------------------------------------------- ABI checks */

_Static_assert(sizeof(struct snd_pcm_info) == 288, "snd_pcm_info ABI");
_Static_assert(sizeof(struct snd_pcm_hw_params) == 608, "snd_pcm_hw_params ABI");
_Static_assert(offsetof(struct snd_pcm_hw_params, intervals) == 260, "snd_pcm_hw_params ABI");
_Static_assert(offsetof(struct snd_pcm_hw_params, rmask) == 512, "snd_pcm_hw_params ABI");
_Static_assert(offsetof(struct snd_pcm_hw_params, fifo_size) == 536, "snd_pcm_hw_params ABI");
_Static_assert(sizeof(struct snd_pcm_sw_params) == 136, "snd_pcm_sw_params ABI");
_Static_assert(sizeof(struct snd_pcm_status) == 152, "snd_pcm_status ABI");
_Static_assert(offsetof(struct snd_pcm_status, appl_ptr) == 40, "snd_pcm_status ABI");
_Static_assert(offsetof(struct snd_pcm_status, suspended_state) == 88, "snd_pcm_status ABI");
_Static_assert(offsetof(struct snd_pcm_status, audio_tstamp) == 96, "snd_pcm_status ABI");
_Static_assert(offsetof(struct snd_pcm_status, audio_tstamp_accuracy) == 128, "snd_pcm_status ABI");
_Static_assert(sizeof(struct snd_pcm_mmap_status) == 56, "snd_pcm_mmap_status ABI");
_Static_assert(offsetof(struct snd_pcm_mmap_status, audio_tstamp) == 40, "snd_pcm_mmap_status ABI");
_Static_assert(sizeof(struct snd_pcm_mmap_control) == 16, "snd_pcm_mmap_control ABI");
_Static_assert(sizeof(struct snd_pcm_sync_ptr) == 136, "snd_pcm_sync_ptr ABI");
_Static_assert(offsetof(struct snd_pcm_sync_ptr, c) == 72, "snd_pcm_sync_ptr ABI");
_Static_assert(sizeof(struct snd_xferi) == 24, "snd_xferi ABI");
_Static_assert(sizeof(struct snd_pcm_channel_info) == 24, "snd_pcm_channel_info ABI");
_Static_assert(sizeof(struct snd_ctl_card_info) == 376, "snd_ctl_card_info ABI");
_Static_assert(sizeof(struct snd_ctl_elem_id) == 64, "snd_ctl_elem_id ABI");
_Static_assert(sizeof(struct snd_ctl_elem_list) == 80, "snd_ctl_elem_list ABI");
_Static_assert(sizeof(struct snd_ctl_elem_info) == 272, "snd_ctl_elem_info ABI");
_Static_assert(offsetof(struct snd_ctl_elem_info, value) == 80, "snd_ctl_elem_info ABI");
_Static_assert(sizeof(struct snd_ctl_elem_value) == 1224, "snd_ctl_elem_value ABI");
_Static_assert(offsetof(struct snd_ctl_elem_value, value) == 72, "snd_ctl_elem_value ABI");
_Static_assert(sizeof(struct snd_ctl_event) == 72, "snd_ctl_event ABI");
_Static_assert(sizeof(struct snd_ctl_card_bytes) == 24, "snd_ctl_card_bytes ABI");
_Static_assert(sizeof(struct snd_timer_id) == 20, "snd_timer_id ABI");
_Static_assert(sizeof(struct snd_timer_ginfo) == 248, "snd_timer_ginfo ABI");
_Static_assert(sizeof(struct snd_timer_gparams) == 72, "snd_timer_gparams ABI");
_Static_assert(sizeof(struct snd_timer_gstatus) == 80, "snd_timer_gstatus ABI");
_Static_assert(sizeof(struct snd_timer_select) == 52, "snd_timer_select ABI");
_Static_assert(sizeof(struct snd_timer_info) == 232, "snd_timer_info ABI");
_Static_assert(sizeof(struct snd_timer_params) == 80, "snd_timer_params ABI");
_Static_assert(sizeof(struct snd_timer_status) == 96, "snd_timer_status ABI");
_Static_assert(sizeof(struct snd_timer_tread) == 32, "snd_timer_tread ABI");
_Static_assert(sizeof(struct snd_timer_uinfo) == 32, "snd_timer_uinfo ABI");

_Static_assert(SNDRV_PCM_IOCTL_INFO == 0x81204101UL, "PCM INFO ioctl");
_Static_assert(SNDRV_PCM_IOCTL_HW_REFINE == 0xc2604110UL, "PCM HW_REFINE ioctl");
_Static_assert(SNDRV_PCM_IOCTL_HW_PARAMS == 0xc2604111UL, "PCM HW_PARAMS ioctl");
_Static_assert(SNDRV_PCM_IOCTL_SW_PARAMS == 0xc0884113UL, "PCM SW_PARAMS ioctl");
_Static_assert(SNDRV_PCM_IOCTL_STATUS == 0x80984120UL, "PCM STATUS ioctl");
_Static_assert(SNDRV_PCM_IOCTL_SYNC_PTR == 0xc0884123UL, "PCM SYNC_PTR ioctl");
_Static_assert(SNDRV_PCM_IOCTL_STATUS_EXT == 0xc0984124UL, "PCM STATUS_EXT ioctl");
_Static_assert(SNDRV_PCM_IOCTL_CHANNEL_INFO == 0x80184132UL, "PCM CHANNEL_INFO ioctl");
_Static_assert(SNDRV_PCM_IOCTL_WRITEI_FRAMES == 0x40184150UL, "PCM WRITEI ioctl");
_Static_assert(SNDRV_PCM_IOCTL_READI_FRAMES == 0x80184151UL, "PCM READI ioctl");
_Static_assert(SNDRV_PCM_IOCTL_REWIND == 0x40084146UL, "PCM REWIND ioctl");
_Static_assert(SNDRV_CTL_IOCTL_CARD_INFO == 0x81785501UL, "CTL CARD_INFO ioctl");
_Static_assert(SNDRV_CTL_IOCTL_ELEM_LIST == 0xc0505510UL, "CTL ELEM_LIST ioctl");
_Static_assert(SNDRV_CTL_IOCTL_ELEM_INFO == 0xc1105511UL, "CTL ELEM_INFO ioctl");
_Static_assert(SNDRV_CTL_IOCTL_ELEM_READ == 0xc4c85512UL, "CTL ELEM_READ ioctl");
_Static_assert(SNDRV_CTL_IOCTL_PCM_INFO == 0xc1205531UL, "CTL PCM_INFO ioctl");
_Static_assert(SNDRV_CTL_IOCTL_TLV_READ == 0xc008551aUL, "CTL TLV_READ ioctl");
_Static_assert(SNDRV_TIMER_IOCTL_NEXT_DEVICE == 0xc0145401UL, "TIMER NEXT_DEVICE ioctl");
_Static_assert(SNDRV_TIMER_IOCTL_GINFO == 0xc0f85403UL, "TIMER GINFO ioctl");
_Static_assert(SNDRV_TIMER_IOCTL_STATUS == 0x80605414UL, "TIMER STATUS ioctl");

#endif // INCLUDE_DRIVERS_AUDIO_ULSA_ASOUND_H_
