/*
 *
 *      rock.c
 *      Rock Ridge (SUSP/RRIP) extension parser
 *
 *      2026/7/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/isofs/isofs.h>
#include <fs/isofs/rock.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>

/* SUSP entries are tagged with a two-byte little-endian signature. */
#define RR_SIG_BYTE(a, b) ((a) | ((b) << 8))

/* A visitor reports RR_SCAN_ABORT to end the walk with a zero status. */
#define RR_SCAN_ABORT (-1000)

/* Position the scan cursor at the SUSP area that trails a directory record. */
static void rr_seek_susp(isofs_rr_state_t *state, const struct iso_directory_record *de)
{
    state->len = (int)sizeof(struct iso_directory_record) + de->name_len[0];
    if (state->len & 1) state->len++;
    state->chr = (uint8_t *)de + state->len;
    state->len = (int)de->length - state->len;
    if (state->len < 0) state->len = 0;
    if (state->rock_offset != -1) {
        state->len -= state->rock_offset;
        state->chr += state->rock_offset;
        if (state->len < 0) state->len = 0;
    }
}

/*
 * Follow a CE (continuation) entry to the block holding further SUSP
 * records. Returns 0 on success, 1 when no continuation exists, or a
 * negative error code.
 */
static int rr_fetch_continuation(isofs_rr_state_t *state)
{
    const int entry_head = 4;

    free(state->buffer);
    state->buffer = NULL;

    if (!state->cont_extent) return 1;
    if ((unsigned)state->cont_offset > (unsigned)(state->block_size - entry_head) || (unsigned)state->cont_size > state->block_size
        || (unsigned)(state->cont_offset + state->cont_size) > state->block_size) {
        plogk("isofs-rock: CE entry out of bounds (extent=%d offset=%d size=%d)\n", state->cont_extent, state->cont_offset, state->cont_size);
        return -EIO;
    }
    if (++state->cont_loops >= ISOFS_RR_MAX_CE) return -EIO;

    state->buffer = malloc(state->cont_size);
    if (!state->buffer) return -ENOMEM;
    if (state->read_block(state->io_ctx, (uint32_t)state->cont_extent, state->buffer, (uint32_t)state->cont_size) != 0) {
        plogk("isofs-rock: Cannot read CE block %d\n", state->cont_extent);
        free(state->buffer);
        state->buffer = NULL;
        return -EIO;
    }

    state->chr         = state->buffer;
    state->len         = state->cont_size;
    state->cont_extent = 0;
    state->cont_size   = 0;
    state->cont_offset = 0;
    return 0;
}

/* Validate an SP (suspension point) entry and record its skip offset. */
static int rr_check_sp(const struct rock_ridge *entry, isofs_rr_state_t *state)
{
    if (entry->u.SP.magic[0] != 0xbe || entry->u.SP.magic[1] != 0xef) return -1;
    state->rock_offset = (int)entry->u.SP.skip;
    return 0;
}

/*
 * Walk every SUSP record of one directory record, handing each RRIP
 * entry to `visit`.  The visitor returns 0 to keep scanning, a negative
 * code to abort with that error, or RR_SCAN_ABORT to stop cleanly.
 */
static int rr_scan(isofs_rr_state_t *state, isofs_mount_t *mount, const struct iso_directory_record *de, int (*visit)(const struct rock_ridge *, isofs_rr_state_t *, void *), void *opaque)
{
    int status = 0;

    memset(state, 0, sizeof(*state));
    state->rock_offset = mount->rock_offset;
    state->block_size  = mount->block_size;
    state->read_block  = mount->rr_read_block;
    state->io_ctx      = mount->rr_read_ctx;
    rr_seek_susp(state, de);

    for (;;) {
        while (state->len > 2) {
            const struct rock_ridge *entry = (const struct rock_ridge *)state->chr;

            if (entry->len < 3) goto out;
            state->chr += entry->len;
            state->len -= entry->len;
            if (state->len < 0) goto out;

            status = visit(entry, state, opaque);
            if (status == RR_SCAN_ABORT) {
                status = 0;
                goto out;
            }
            if (status != 0) goto out;
        }
        status = rr_fetch_continuation(state);
        if (status == 0) continue;
        break;
    }
out:
    free(state->buffer);
    state->buffer = NULL;
    return status;
}

/* NM (alternate name) record collection */

typedef struct {
        char *out;
        int   bufsize;
        int   used;
        int   truncated;
} rr_name_ctx_t;

static int rr_visit_name(const struct rock_ridge *entry, isofs_rr_state_t *state, void *opaque)
{
    rr_name_ctx_t *ctx = opaque;
    int            sig = isonum_721(entry->signature);

    switch (sig) {
        case RR_SIG_BYTE('R', 'R') :
            if ((entry->u.RR.flags[0] & RR_NM) == 0) return RR_SCAN_ABORT;
            return 0;
        case RR_SIG_BYTE('S', 'P') :
            if (rr_check_sp(entry, state)) return RR_SCAN_ABORT;
            return 0;
        case RR_SIG_BYTE('C', 'E') :
            state->cont_extent = isonum_733(entry->u.CE.extent);
            state->cont_offset = isonum_733(entry->u.CE.offset);
            state->cont_size   = isonum_733(entry->u.CE.size);
            return 0;
        case RR_SIG_BYTE('N', 'M') : {
            int namelen;

            if (ctx->truncated) return 0;
            if (entry->len < 5) return 0;
            if (entry->u.NM.flags & 6) return 0;
            if (entry->u.NM.flags & ~1) return 0;

            namelen = entry->len - 5;
            if (ctx->used + namelen >= ctx->bufsize) {
                ctx->truncated = 1;
                return 0;
            }
            memcpy(ctx->out + ctx->used, entry->u.NM.name, (size_t)namelen);
            ctx->used += namelen;
            ctx->out[ctx->used] = '\0';
            return 0;
        }
        case RR_SIG_BYTE('R', 'E') :
            return -1;
        default :
            return 0;
    }
}

/* Collect the Rock Ridge alternate (NM) name of a record. */
int isofs_rr_filename(void *raw_de, char *out, int bufsize, isofs_mount_t *mount)
{
    isofs_rr_state_t             state;
    rr_name_ctx_t                ctx;
    struct iso_directory_record *de = raw_de;
    int                          status;

    if (!mount || !mount->rock_ridge) return 0;
    *out = '\0';

    memset(&ctx, 0, sizeof(ctx));
    ctx.out     = out;
    ctx.bufsize = bufsize;

    status = rr_scan(&state, mount, de, rr_visit_name, &ctx);
    return status == 1 ? ctx.used : status;
}

/* SL (symlink) chain handling */

typedef struct {
        isofs_handle_t *handle;
        int             symlink_len;
        int             bad;
} rr_inode_ctx_t;

/* Measure the reconstructed size of one SL chain. */
static void rr_measure_sl(const struct rock_ridge *entry, rr_inode_ctx_t *ctx)
{
    int                        slen = entry->len - 5;
    const struct SL_component *slp  = &entry->u.SL.link;

    ctx->symlink_len = (int)ctx->handle->size;

    while (slen > 1) {
        if ((int)(slp->len + 2) > slen) {
            ctx->bad = 1;
            return;
        }
        switch (slp->flags & ~1) {
            case 0 :
                ctx->symlink_len += slp->len;
                break;
            case 2 :
                ctx->symlink_len += 1;
                break;
            case 4 :
                ctx->symlink_len += 2;
                break;
            case 8 :
                ctx->symlink_len += 1;
                break;
            default :
                break;
        }
        slen -= slp->len + 2;
        slp = (const struct SL_component *)((const char *)slp + slp->len + 2);
        if (slen < 2) {
            if ((entry->u.SL.flags & 1) && !(slp[-1].flags & 1)) ctx->symlink_len += 1;
            break;
        }
        if (!(slp[-1].flags & 1)) ctx->symlink_len += 1;
    }
}

/* RRIP visitor collecting symlink size and child-link metadata. */
static int rr_visit_inode(const struct rock_ridge *entry, isofs_rr_state_t *state, void *opaque)
{
    rr_inode_ctx_t *ctx = opaque;
    int             sig = isonum_721(entry->signature);

    switch (sig) {
        case RR_SIG_BYTE('R', 'R') :
            if ((entry->u.RR.flags[0] & (RR_PX | RR_TF | RR_SL | RR_CL)) == 0) return RR_SCAN_ABORT;
            return 0;
        case RR_SIG_BYTE('S', 'P') :
            if (rr_check_sp(entry, state)) return RR_SCAN_ABORT;
            return 0;
        case RR_SIG_BYTE('C', 'E') :
            state->cont_extent = isonum_733(entry->u.CE.extent);
            state->cont_offset = isonum_733(entry->u.CE.offset);
            state->cont_size   = isonum_733(entry->u.CE.size);
            return 0;
        case RR_SIG_BYTE('S', 'L') :
            rr_measure_sl(entry, ctx);
            return ctx->bad ? RR_SCAN_ABORT : 0;
        case RR_SIG_BYTE('C', 'L') :
            ctx->handle->mount->rock_ridge = 0;
            return RR_SCAN_ABORT;
        default :
            return 0;
    }
}

/* Apply Rock Ridge metadata (size, symlink, relocation) to a handle. */
void isofs_rr_parse_inode(void *raw_de, isofs_handle_t *handle, isofs_mount_t *mount)
{
    isofs_rr_state_t             state;
    rr_inode_ctx_t               ctx;
    struct iso_directory_record *de = raw_de;

    if (!mount || !mount->rock_ridge || !handle) return;

    memset(&ctx, 0, sizeof(ctx));
    ctx.handle = handle;

    (void)rr_scan(&state, mount, de, rr_visit_inode, &ctx);

    if (ctx.symlink_len > 0) {
        handle->is_symlink = 1;
        handle->size       = (uint64_t)ctx.symlink_len;
    }
}

/* Reconstruct the symlink target from an SL chain into `buf`. */
typedef struct {
        char *rpnt;
        char *end;
} rr_symlink_ctx_t;

/* RRIP visitor reconstructing the symlink target into the output buffer. */
static int rr_visit_symlink(const struct rock_ridge *entry, isofs_rr_state_t *state, void *opaque)
{
    rr_symlink_ctx_t *ctx = opaque;
    int               sig = isonum_721(entry->signature);

    switch (sig) {
        case RR_SIG_BYTE('R', 'R') :
            if ((entry->u.RR.flags[0] & RR_SL) == 0) return RR_SCAN_ABORT;
            return 0;
        case RR_SIG_BYTE('S', 'P') :
            if (rr_check_sp(entry, state)) return RR_SCAN_ABORT;
            return 0;
        case RR_SIG_BYTE('C', 'E') :
            state->cont_extent = isonum_733(entry->u.CE.extent);
            state->cont_offset = isonum_733(entry->u.CE.offset);
            state->cont_size   = isonum_733(entry->u.CE.size);
            return 0;
        case RR_SIG_BYTE('S', 'L') : {
            int                        slen = entry->len - 5;
            const struct SL_component *slp  = &entry->u.SL.link;

            while (slen > 1) {
                if ((int)(slp->len + 2) > slen) return RR_SCAN_ABORT;
                switch (slp->flags & ~1) {
                    case 0 :
                        if (slp->len > ctx->end - ctx->rpnt) return RR_SCAN_ABORT;
                        memcpy(ctx->rpnt, slp->text, slp->len);
                        ctx->rpnt += slp->len;
                        break;
                    case 2 :
                        if (ctx->rpnt >= ctx->end) return RR_SCAN_ABORT;
                        *ctx->rpnt++ = '.';
                        break;
                    case 4 :
                        if (2 > ctx->end - ctx->rpnt) return RR_SCAN_ABORT;
                        *ctx->rpnt++ = '.';
                        *ctx->rpnt++ = '.';
                        break;
                    case 8 :
                        if (ctx->rpnt >= ctx->end) return RR_SCAN_ABORT;
                        *ctx->rpnt++ = '/';
                        break;
                    default :
                        break;
                }
                slen -= slp->len + 2;
                slp = (const struct SL_component *)((const char *)slp + slp->len + 2);
                if (slen < 2) {
                    if ((entry->u.SL.flags & 1) && !(((const struct SL_component *)((const char *)slp - slp->len - 2))->flags & 1)) {
                        if (ctx->rpnt >= ctx->end) return RR_SCAN_ABORT;
                        *ctx->rpnt++ = '/';
                    }
                    break;
                }
                if (!(((const struct SL_component *)((const char *)slp - slp->len - 2))->flags & 1)) {
                    if (ctx->rpnt >= ctx->end) return RR_SCAN_ABORT;
                    *ctx->rpnt++ = '/';
                }
            }
            return 0;
        }
        default :
            return 0;
    }
}

/* Reconstruct a symlink target from an SL chain into a buffer. */
int isofs_rr_symlink(void *raw_de, isofs_mount_t *mount, char *buf, int bufsize)
{
    isofs_rr_state_t             state;
    rr_symlink_ctx_t             ctx;
    struct iso_directory_record *de = raw_de;

    if (!mount || !mount->rock_ridge) return -EIO;

    ctx.rpnt = buf;
    ctx.end  = buf + bufsize - 1;

    (void)rr_scan(&state, mount, de, rr_visit_symlink, &ctx);

    if (ctx.rpnt == buf) return -EIO;
    *ctx.rpnt = '\0';
    return (int)(ctx.rpnt - buf);
}

/* Translate an ISO 9660 "8.3 with version" filename to a readable form. */
int isofs_rr_translate_name(void *raw_de, char *out, int bufsize)
{
    const struct iso_directory_record *de  = raw_de;
    const char                        *raw = de->name;
    int                                len = (int)de->name_len[0];
    int                                i;

    for (i = 0; i < len && i < bufsize - 1; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (!c) break;
        if (c >= 'A' && c <= 'Z') c |= 0x20;
        if (c == '.' && i == len - 3 && raw[i + 1] == ';' && raw[i + 2] == '1') break;
        if (c == ';' && i == len - 2 && raw[i + 1] == '1') break;
        if (c == ';' || c == '/') c = '.';
        out[i] = (char)c;
    }
    out[i] = '\0';
    return i;
}
