/*
 *
 *      rock.c
 *      Rock Ridge extension parser
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

#define SIG(A, B) ((A) | ((B) << 8))

static int rock_continue(struct rock_state *rs)
{
    int min_de_size = 4; /* offsetof(struct rock_ridge, u) */

    free(rs->buffer);
    rs->buffer = NULL;

    if (!rs->cont_extent) return 1;

    if ((unsigned)rs->cont_offset > (unsigned)(int)(rs->block_size - min_de_size) || (unsigned)rs->cont_size > rs->block_size
        || (unsigned)(rs->cont_offset + rs->cont_size) > rs->block_size) {
        plogk("rock: corrupted CE entry, extent=%d offset=%d size=%d\n", rs->cont_extent, rs->cont_offset, rs->cont_size);
        return -5; /* -EIO */
    }

    if (++rs->cont_loops >= ISOFS_RR_MAX_CE) return -5;

    rs->buffer = malloc(rs->cont_size);
    if (!rs->buffer) return -12; /* -ENOMEM */

    if (rs->read_block(rs->io_ctx, (uint32_t)rs->cont_extent, rs->buffer, (uint32_t)rs->cont_size) != 0) {
        plogk("rock: unable to read CE block %d\n", rs->cont_extent);
        free(rs->buffer);
        rs->buffer = NULL;
        return -5;
    }

    rs->chr         = rs->buffer;
    rs->len         = rs->cont_size;
    rs->cont_extent = 0;
    rs->cont_size   = 0;
    rs->cont_offset = 0;
    return 0;
}

static int check_sp(struct rock_ridge *rr, struct rock_state *rs)
{
    if (rr->u.SP.magic[0] != 0xbe || rr->u.SP.magic[1] != 0xef) return -1;
    rs->rock_offset = (int)rr->u.SP.skip;
    return 0;
}

static void setup_rock_ridge(struct iso_directory_record *de, struct rock_state *rs)
{
    rs->len = sizeof(struct iso_directory_record) + de->name_len[0];
    if (rs->len & 1) rs->len++;
    rs->chr = (uint8_t *)de + rs->len;
    rs->len = (int)de->length - rs->len;
    if (rs->len < 0) rs->len = 0;

    if (rs->rock_offset != -1) {
        rs->len -= rs->rock_offset;
        rs->chr += rs->rock_offset;
        if (rs->len < 0) rs->len = 0;
    }
}

int get_rock_ridge_filename(void *raw_de, char *out, int bufsize, isofs_mount_t *mount)
{
    struct iso_directory_record *de = raw_de;
    struct rock_state            rs;
    struct rock_ridge           *rr;
    int                          sig;
    int                          retnamlen = 0;
    int                          truncate  = 0;
    int                          ret       = 0;

    if (!mount || !mount->rock_ridge) return 0;
    *out = '\0';

    memset(&rs, 0, sizeof(rs));
    rs.rock_offset = mount->rock_offset;
    rs.block_size  = mount->block_size;
    rs.read_block  = mount->rr_read_block;
    rs.io_ctx      = mount->rr_read_ctx;
    setup_rock_ridge(de, &rs);

    for (;;) {
        while (rs.len > 2) {
            rr = (struct rock_ridge *)rs.chr;
            if (rr->len < 3) goto out;
            sig = isonum_721(rr->signature);
            rs.chr += rr->len;
            rs.len -= rr->len;
            if (rs.len < 0) goto out;

            switch (sig) {
                case SIG('R', 'R') :
                    if ((rr->u.RR.flags[0] & RR_NM) == 0) goto out;
                    break;
                case SIG('S', 'P') :
                    if (check_sp(rr, &rs)) goto out;
                    break;
                case SIG('C', 'E') :
                    rs.cont_extent = isonum_733(rr->u.CE.extent);
                    rs.cont_offset = isonum_733(rr->u.CE.offset);
                    rs.cont_size   = isonum_733(rr->u.CE.size);
                    break;
                case SIG('N', 'M') :
                    if (truncate) break;
                    if (rr->len < 5) break;
                    if (rr->u.NM.flags & 6) break;
                    if (rr->u.NM.flags & ~1) break;
                    {
                        int len = rr->len - 5;
                        if (retnamlen + len >= bufsize) {
                            truncate = 1;
                            break;
                        }
                        memcpy(out + retnamlen, rr->u.NM.name, len);
                        retnamlen += len;
                        out[retnamlen] = '\0';
                    }
                    break;
                case SIG('R', 'E') :
                    free(rs.buffer);
                    return -1;
                default :
                    break;
            }
        }
        ret = rock_continue(&rs);
        if (ret == 0) continue;
        break;
    }

out:
    free(rs.buffer);
    if (ret == 1) return retnamlen;
    return ret;
}

void parse_rock_ridge_inode(void *raw_de, isofs_handle_t *handle, isofs_mount_t *mount)
{
    struct iso_directory_record *de = raw_de;
    struct rock_state            rs;
    struct rock_ridge           *rr;
    int                          sig;
    int                          symlink_len = 0;
    int                          ret         = 0;

    if (!mount || !mount->rock_ridge || !handle) return;

    memset(&rs, 0, sizeof(rs));
    rs.rock_offset = mount->rock_offset;
    rs.block_size  = mount->block_size;
    rs.read_block  = mount->rr_read_block;
    rs.io_ctx      = mount->rr_read_ctx;
    setup_rock_ridge(de, &rs);

    for (;;) {
        while (rs.len > 2) {
            rr = (struct rock_ridge *)rs.chr;
            if (rr->len < 3) goto out;
            sig = isonum_721(rr->signature);
            rs.chr += rr->len;
            rs.len -= rr->len;
            if (rs.len < 0) goto out;

            switch (sig) {
                case SIG('R', 'R') :
                    if ((rr->u.RR.flags[0] & (RR_PX | RR_TF | RR_SL | RR_CL)) == 0) goto out;
                    break;
                case SIG('S', 'P') :
                    if (check_sp(rr, &rs)) goto out;
                    break;
                case SIG('C', 'E') :
                    rs.cont_extent = isonum_733(rr->u.CE.extent);
                    rs.cont_offset = isonum_733(rr->u.CE.offset);
                    rs.cont_size   = isonum_733(rr->u.CE.size);
                    break;
                case SIG('S', 'L') : {
                    int                  slen = rr->len - 5;
                    struct SL_component *slp  = &rr->u.SL.link;
                    symlink_len               = (int)handle->size;
                    while (slen > 1) {
                        if ((int)(slp->len + 2) > slen) goto out;
                        switch (slp->flags & ~1) {
                            case 0 :
                                symlink_len += slp->len;
                                break;
                            case 2 :
                                symlink_len += 1;
                                break;
                            case 4 :
                                symlink_len += 2;
                                break;
                            case 8 :
                                symlink_len += 1;
                                break;
                        }
                        slen -= slp->len + 2;
                        slp = (struct SL_component *)((char *)slp + slp->len + 2);
                        if (slen < 2) {
                            if ((rr->u.SL.flags & 1) && !(slp[-1].flags & 1)) symlink_len += 1;
                            break;
                        }
                        if (!(slp[-1].flags & 1)) symlink_len += 1;
                    }
                } break;
                case SIG('C', 'L') :
                    mount->rock_ridge = 0;
                    goto out;
                default :
                    break;
            }
        }
        ret = rock_continue(&rs);
        if (ret == 0) continue;
        break;
    }

out:
    free(rs.buffer);
    if (symlink_len > 0) {
        handle->is_symlink = 1;
        handle->size       = (uint64_t)symlink_len;
    }
}

int get_rock_ridge_symlink(void *raw_de, isofs_mount_t *mount, char *buf, int bufsize)
{
    struct iso_directory_record *de = raw_de;
    struct rock_state            rs;
    struct rock_ridge           *rr;
    int                          sig;
    char                        *rpnt = buf;
    char                        *end  = buf + bufsize - 1;
    int                          ret  = 0;

    if (!mount || !mount->rock_ridge) return -EIO;

    memset(&rs, 0, sizeof(rs));
    rs.rock_offset = mount->rock_offset;
    rs.block_size  = mount->block_size;
    rs.read_block  = mount->rr_read_block;
    rs.io_ctx      = mount->rr_read_ctx;

    rs.len = sizeof(struct iso_directory_record) + de->name_len[0];
    if (rs.len & 1) rs.len++;
    rs.chr = (uint8_t *)de + rs.len;
    rs.len = (int)de->length - rs.len;
    if (rs.len < 0) rs.len = 0;
    if (rs.rock_offset != -1) {
        rs.len -= rs.rock_offset;
        rs.chr += rs.rock_offset;
        if (rs.len < 0) rs.len = 0;
    }

    for (;;) {
        while (rs.len > 2) {
            rr = (struct rock_ridge *)rs.chr;
            if (rr->len < 3) goto out;
            sig = isonum_721(rr->signature);
            rs.chr += rr->len;
            rs.len -= rr->len;
            if (rs.len < 0) goto out;

            switch (sig) {
                case SIG('R', 'R') :
                    if ((rr->u.RR.flags[0] & RR_SL) == 0) goto out;
                    break;
                case SIG('S', 'P') :
                    if (check_sp(rr, &rs)) goto out;
                    break;
                case SIG('S', 'L') : {
                    int                  slen = rr->len - 5;
                    struct SL_component *slp  = &rr->u.SL.link;
                    while (slen > 1) {
                        if ((int)(slp->len + 2) > slen) goto out;
                        switch (slp->flags & ~1) {
                            case 0 :
                                if (slp->len > end - rpnt) goto out;
                                memcpy(rpnt, slp->text, slp->len);
                                rpnt += slp->len;
                                break;
                            case 2 :
                                if (rpnt >= end) goto out;
                                *rpnt++ = '.';
                                break;
                            case 4 :
                                if (2 > end - rpnt) goto out;
                                *rpnt++ = '.';
                                *rpnt++ = '.';
                                break;
                            case 8 :
                                if (rpnt >= end) goto out;
                                *rpnt++ = '/';
                                break;
                        }
                        slen -= slp->len + 2;
                        slp = (struct SL_component *)((char *)slp + slp->len + 2);
                        if (slen < 2) {
                            if ((rr->u.SL.flags & 1) && !(((struct SL_component *)((char *)slp - slp->len - 2))->flags & 1)) {
                                if (rpnt >= end) goto out;
                                *rpnt++ = '/';
                            }
                            break;
                        }
                        if (!(((struct SL_component *)((char *)slp - slp->len - 2))->flags & 1)) {
                            if (rpnt >= end) goto out;
                            *rpnt++ = '/';
                        }
                    }
                } break;
                case SIG('C', 'E') :
                    rs.cont_extent = isonum_733(rr->u.CE.extent);
                    rs.cont_offset = isonum_733(rr->u.CE.offset);
                    rs.cont_size   = isonum_733(rr->u.CE.size);
                    break;
            }
        }
        ret = rock_continue(&rs);
        if (ret == 0) continue;
        break;
    }

out:
    free(rs.buffer);
    if (rpnt == buf) return -EIO;
    *rpnt = '\0';
    return (int)(rpnt - buf);
}

/*
 * Translate an ISO 9660 "8.3 with version" filename to a readable form.
 */
int isofs_name_translate(void *raw_de, char *out, int bufsize)
{
    struct iso_directory_record *de  = raw_de;
    char                        *old = de->name;
    int                          len = (int)de->name_len[0];
    int                          i;

    for (i = 0; i < len && i < bufsize - 1; i++) {
        unsigned char c = (unsigned char)old[i];
        if (!c) break;

        if (c >= 'A' && c <= 'Z') c |= 0x20;

        if (c == '.' && i == len - 3 && old[i + 1] == ';' && old[i + 2] == '1') break;

        if (c == ';' && i == len - 2 && old[i + 1] == '1') break;

        if (c == ';' || c == '/') c = '.';

        out[i] = (char)c;
    }
    out[i] = '\0';
    return i;
}
