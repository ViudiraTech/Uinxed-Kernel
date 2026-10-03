/*
 *
 *      ntfs_util.c
 *      New Technology File System - byte order, time and name helpers
 *
 *      2026/7/26 By MicroFish & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/ntfs/ntfs.h>
#include <kernel/errno.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <libs/util/byteorder.h>
#include <mem/heap.h>

#if CONFIG_NTFS_FS

/* Align a value up to 8 bytes. */
uint32_t ntfs_align8(uint32_t value)
{
    return (value + 7) & ~7U;
}

/* "$I30" index attribute name, as UTF-16 code units. */
const uint16_t ntfs_i30[4] = {'$', 'I', '3', '0'};

/* Current time as a Windows 100ns FILETIME since 1601. */
uint64_t ntfs_current_filetime(void)
{
    return ((uint64_t)timer_realtime_seconds32() + 11644473600ULL) * 10000000ULL;
}

/* Copy len little-endian UTF-16 code units out of a byte buffer. */
uint16_t *ntfs_utf16_from(const uint8_t *buf, int ofs, int len)
{
    if (len <= 0 || len > 255) return NULL;
    uint16_t *out = calloc(len + 1, sizeof(uint16_t));
    if (!out) return NULL;
    for (int i = 0; i < len; i++) out[i] = load_le16(buf + ofs + ((size_t)i * 2));
    return out;
}

/* Convert a UTF-16 string to a freshly allocated UTF-8 string. */
char *ntfs_utf8_from_utf16(const uint16_t *u, int len)
{
    if (len <= 0 || !u) return strdup("");
    int out_len = 0;
    for (int i = 0; i < len && u[i]; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            out_len++;
        } else if (cp < 0x800) {
            out_len += 2;
        } else if (cp < 0x10000) {
            out_len += 3;
        } else {
            out_len += 4;
        }
    }
    char *out = calloc(out_len + 1, 1);
    if (!out) return NULL;
    int j = 0;
    for (int i = 0; i < len && u[i]; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            out[j++] = (char)cp;
        } else if (cp < 0x800) {
            out[j++] = (char)(0xC0 | (cp >> 6));
            out[j++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[j++] = (char)(0xE0 | (cp >> 12));
            out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[j++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[j++] = (char)(0xF0 | (cp >> 18));
            out[j++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[j++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

/* Whether an attribute is the $I30 directory index. */
int ntfs_attr_name_is_i30(const attr_rec_t *attribute, uint32_t length)
{
    uint16_t offset;

    if (!attribute || attribute->name_length != 4) return 0;
    offset = load_le16((const uint8_t *)&attribute->name_offset);
    if (offset > length || sizeof(ntfs_i30) > length - offset) return 0;
    for (uint32_t i = 0; i < 4; i++)
        if (load_le16((const uint8_t *)attribute + offset + (i * sizeof(uint16_t))) != ntfs_i30[i]) return 0;
    return 1;
}

/* Uppercase one UTF-16 code unit. */
static uint16_t ntfs_upcase_char(ntfs_mount_t *mnt, uint16_t value)
{
    if (mnt && mnt->upcase && value < mnt->upcase_length) return mnt->upcase[value];
    if (value >= 'a' && value <= 'z') return value - 'a' + 'A';
    return value;
}

/* Case-insensitive comparison of two UTF-16 names using the upcase table. */
int ntfs_utf16_compare(ntfs_mount_t *mnt, const uint16_t *left, uint8_t left_length, const uint16_t *right, uint8_t right_length)
{
    uint8_t common = left_length < right_length ? left_length : right_length;

    for (uint8_t i = 0; i < common; i++) {
        uint16_t left_upcase  = ntfs_upcase_char(mnt, left[i]);
        uint16_t right_upcase = ntfs_upcase_char(mnt, right[i]);
        if (left_upcase < right_upcase) return -1;
        if (left_upcase > right_upcase) return 1;
    }
    if (left_length < right_length) return -1;
    if (left_length > right_length) return 1;
    return 0;
}

/* Convert a UTF-8 name to UTF-16, validating legal NTFS file-name characters. */
int ntfs_utf16_from_utf8(const char *source, uint16_t **name, uint8_t *name_length, int filename)
{
    uint16_t *result;
    uint32_t  count  = 0;
    uint32_t  offset = 0;

    if (!source || !name || !name_length || !source[0]) return -EINVAL;
    result = calloc(255, sizeof(uint16_t));
    if (!result) return -ENOMEM;
    while (source[offset]) {
        uint32_t codepoint;
        uint8_t  first = (uint8_t)source[offset++];
        uint32_t continuation;

        if (first < 0x80) {
            codepoint    = first;
            continuation = 0;
        } else if ((first & 0xe0) == 0xc0) {
            codepoint    = first & 0x1f;
            continuation = 1;
            if (codepoint < 2) goto invalid;
        } else if ((first & 0xf0) == 0xe0) {
            codepoint    = first & 0x0f;
            continuation = 2;
        } else if ((first & 0xf8) == 0xf0) {
            codepoint    = first & 0x07;
            continuation = 3;
        } else
            goto invalid;
        for (uint32_t i = 0; i < continuation; i++) {
            uint8_t byte = (uint8_t)source[offset++];
            if (!byte || (byte & 0xc0) != 0x80) goto invalid;
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if ((continuation == 2 && codepoint < 0x800) || (continuation == 3 && codepoint < 0x10000) || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) goto invalid;
        if (codepoint < 0x20
            || (filename
                && (codepoint == '"' || codepoint == '*' || codepoint == '/' || codepoint == ':' || codepoint == '<' || codepoint == '>' || codepoint == '?' || codepoint == '\\'
                    || codepoint == '|'))) {
            goto invalid;
        }
        if (codepoint < 0x10000) {
            if (count == 255) goto too_long;
            result[count++] = (uint16_t)codepoint;
        } else {
            if (count > 253) goto too_long;
            codepoint -= 0x10000;
            result[count++] = (uint16_t)(0xd800 | (codepoint >> 10));
            result[count++] = (uint16_t)(0xdc00 | (codepoint & 0x3ff));
        }
    }
    if (!count || (filename && (result[count - 1] == ' ' || result[count - 1] == '.'))) goto invalid;
    *name        = result;
    *name_length = (uint8_t)count;
    return 0;
too_long:
    free(result);
    return -ENAMETOOLONG;
invalid:
    free(result);
    return -EINVAL;
}

/* Convert a UTF-8 name to UTF-16 for a file name. */
int ntfs_name_from_utf8(const char *source, uint16_t **name, uint8_t *name_length)
{
    return ntfs_utf16_from_utf8(source, name, name_length, 1);
}

#endif
