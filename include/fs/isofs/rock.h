/*
 *
 *      rock.h
 *      Rock Ridge (SUSP) extension structures
 *
 *      2026/7/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef ISOFS_ROCK_H_
#define ISOFS_ROCK_H_

#include <libs/std/stdint.h>

/* ─── SUSP / Rock Ridge on‑disk structures ─── */

struct SU_SP_s {
        uint8_t magic[2];
        uint8_t skip;
} __attribute__((packed));

struct SU_CE_s {
        uint8_t extent[8];
        uint8_t offset[8];
        uint8_t size[8];
} __attribute__((packed));

struct SU_ER_s {
        uint8_t len_id;
        uint8_t len_des;
        uint8_t len_src;
        uint8_t ext_ver;
        uint8_t data[];
} __attribute__((packed));

struct RR_RR_s {
        uint8_t flags[1];
} __attribute__((packed));

struct RR_PX_s {
        uint8_t mode[8];
        uint8_t n_links[8];
        uint8_t uid[8];
        uint8_t gid[8];
} __attribute__((packed));

struct RR_PN_s {
        uint8_t dev_high[8];
        uint8_t dev_low[8];
} __attribute__((packed));

struct SL_component {
        uint8_t flags;
        uint8_t len;
        uint8_t text[];
} __attribute__((packed));

struct RR_SL_s {
        uint8_t             flags;
        struct SL_component link;
} __attribute__((packed));

struct RR_NM_s {
        uint8_t flags;
        char    name[];
} __attribute__((packed));

struct RR_CL_s {
        uint8_t location[8];
} __attribute__((packed));

struct RR_PL_s {
        uint8_t location[8];
} __attribute__((packed));

struct RR_TF_s {
        uint8_t flags;
        uint8_t data[];
} __attribute__((packed));

struct RR_ZF_s {
        uint8_t algorithm[2];
        uint8_t parms[2];
        uint8_t real_size[8];
} __attribute__((packed));

/* ─── TF flags ─── */

#define TF_CREATE     1
#define TF_MODIFY     2
#define TF_ACCESS     4
#define TF_ATTRIBUTES 8
#define TF_LONG_FORM  128

/* ─── RR flags ─── */

#define RR_PX 1
#define RR_PN 2
#define RR_SL 4
#define RR_NM 8
#define RR_CL 16
#define RR_PL 32
#define RR_RE 64
#define RR_TF 128

/* ─── Rock Ridge record header ─── */

struct rock_ridge {
        uint8_t signature[2];
        uint8_t len;
        uint8_t version;
        union {
                struct SU_SP_s SP;
                struct SU_CE_s CE;
                struct SU_ER_s ER;
                struct RR_RR_s RR;
                struct RR_PX_s PX;
                struct RR_PN_s PN;
                struct RR_SL_s SL;
                struct RR_NM_s NM;
                struct RR_CL_s CL;
                struct RR_PL_s PL;
                struct RR_TF_s TF;
                struct RR_ZF_s ZF;
        } u;
} __attribute__((packed));

/* ─── Rock Ridge parser state ─── */

struct rock_state {
        void    *buffer;
        uint8_t *chr;
        int      len;
        int      cont_extent;
        int      cont_offset;
        int      cont_size;
        int      cont_loops;
        uint8_t *disk_buf;
        uint32_t block_size;
        uint32_t block_bits;
        int      rock_offset;
        /* callbacks for block I/O */
        int (*read_block)(void *ctx, uint32_t block, void *buf, uint32_t size);
        void *io_ctx;
};

/*
 * Read the full symlink target from a directory record's SL entries.
 * `raw_de` is the iso_directory_record, `mount` is the isofs_mount_t,
 * `buf`/`bufsize` receive the reconstructed path.
 * Returns the number of bytes written (excluding NUL), or negative on error.
 */
int get_rock_ridge_symlink(void *raw_de, struct isofs_mount *mount, char *buf, int bufsize);

#endif /* ISOFS_ROCK_H_ */
