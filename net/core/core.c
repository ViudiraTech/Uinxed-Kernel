/*
 *
 *      core.c
 *      Network core functionality
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpuid.h>
#include <arch/fpu.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <net/core/endian.h>
#include <net/core/netdev.h>
#include <net/core/packet.h>
#include <net/ipv4/arp.h>
#include <net/ipv4/dhcp.h>
#include <net/ipv6/ndp.h>
#include <net/transport/tcp.h>

#if CONFIG_NET

/*
 * SSE2 fast path: eight 16-bit words per 16-byte vector, byte-swapped by a
 * pair of 16-bit lane shifts (pure SSE2, no SSSE3 required).  Partial sums
 * live in four 32-bit lanes and are folded periodically so no lane can
 * overflow.  Runs inside a kernel_fpu_begin()/end() section.
 */
typedef unsigned short csum_v8hu __attribute__((__vector_size__(16)));
typedef unsigned int   csum_v4su __attribute__((__vector_size__(16)));

/*
 * RFC 1071 checksum accumulation over 16-bit big-endian words.  The running
 * sum is folded below 2^16 as words are added so the 32-bit accumulator can
 * never wrap: a wrap would corrupt the result because 2^32 == 1 (mod 2^16-1).
 */
static uint32_t net_checksum_add_words(uint32_t sum, const uint8_t *bytes, size_t length)
{
    while (length >= 2) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2;
        if (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    }
    if (length) sum += (uint16_t)bytes[0] << 8;
    return sum;
}

/* SSE2 accumulation path, used by net_checksum_add for large buffers. */
__attribute__((target("sse2"))) static uint32_t net_checksum_add_sse2(uint32_t sum, const uint8_t *bytes, size_t length)
{
    kernel_fpu_begin();

    size_t    bulk = length & ~(size_t)15;
    csum_v4su acc  = {0, 0, 0, 0};
    size_t    i    = 0;
    for (; i + 16 <= bulk; i += 16) {
        csum_v8hu word;
        memcpy(&word, bytes + i, 16);
        csum_v8hu swapped = (word << 8) | (word >> 8);
        csum_v4su wide;
        memcpy(&wide, &swapped, 16);
        csum_v4su lo = wide & (csum_v4su) {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
        csum_v4su hi = wide >> 16;
        acc          = acc + lo + hi;
        if (i && !(i & 0xFFF)) {
            acc = (acc & (csum_v4su) {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu}) + (acc >> 16);
            acc = (acc & (csum_v4su) {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu}) + (acc >> 16);
        }
    }
    kernel_fpu_end();

    sum += acc[0] + acc[1] + acc[2] + acc[3];
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return net_checksum_add_words(sum, bytes + bulk, length - bulk);
}

/* Accumulate a partial checksum, choosing the scalar or SSE2 path. */
uint32_t net_checksum_add(uint32_t sum, const void *data, size_t length)
{
    static uint8_t sse_checked;
    static uint8_t sse_ok;

    if (!sse_checked) {
        sse_ok      = kernel_sse_available() != 0 && cpu_support_sse2() != 0;
        sse_checked = 1;
    }

    /* FPU-section overhead only pays off for buffers of at least a few vectors */
    if (sse_ok && length >= 128) return net_checksum_add_sse2(sum, data, length);
    return net_checksum_add_words(sum, data, length);
}

/* Fold a partial sum down to a 16-bit one's-complement result. */
uint16_t net_checksum_finish(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Compute the RFC 1071 checksum of a buffer. */
uint16_t net_checksum(const void *data, size_t length)
{
    return net_checksum_finish(net_checksum_add(0, data, length));
}

/* Compute the IPv4 pseudo-header checksum for a transport segment. */
uint16_t net_checksum_ipv4_pseudo(uint32_t source, uint32_t destination, uint8_t protocol, const void *data, size_t length)
{
    uint8_t pseudo[12];
    store_be32(pseudo, source);
    store_be32(pseudo + 4, destination);
    pseudo[8] = 0;
    pseudo[9] = protocol;
    store_be16(pseudo + 10, (uint16_t)length);
    return net_checksum_finish(net_checksum_add(net_checksum_add(0, pseudo, sizeof(pseudo)), data, length));
}

/* Allocate a packet buffer with room for a protocol header in the headroom. */
net_pbuf_t *net_pbuf_alloc(size_t payload_length, size_t headroom)
{
    if (payload_length > NET_PBUF_MAX_SIZE || headroom > NET_PBUF_MAX_SIZE || payload_length > NET_PBUF_MAX_SIZE - headroom) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("net: Pbuf alloc rejected (payload=%zu headroom=%zu): size limit.\n", payload_length, headroom);
        return NULL;
    }
    net_pbuf_t *pbuf = calloc(1, sizeof(*pbuf));
    if (!pbuf) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("net: Pbuf struct alloc failed (payload=%zu headroom=%zu)\n", payload_length, headroom);
        return NULL;
    }
    size_t total  = payload_length + headroom;
    pbuf->storage = malloc(total ? total : 1);
    if (!pbuf->storage) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("net: Pbuf storage alloc failed (payload=%zu headroom=%zu)\n", payload_length, headroom);
        free(pbuf);
        return NULL;
    }
    pbuf->data     = pbuf->storage + headroom;
    pbuf->length   = payload_length;
    pbuf->capacity = total;
    pbuf->refs     = 1;
    return pbuf;
}

/* Wrap a driver-owned buffer as a packet that calls release when freed. */
int net_packet_init_external(net_pbuf_t *packet, void *data, size_t length, net_packet_release_t release, void *context)
{
    if (!packet || (!data && length)) return -EINVAL;
    memset(packet, 0, sizeof(*packet));
    packet->storage         = data;
    packet->data            = data;
    packet->length          = length;
    packet->capacity        = length;
    packet->refs            = 1;
    packet->release         = release;
    packet->release_context = context;
    packet->external        = 1;
    return 0;
}

/* Take a reference on a packet. */
void net_packet_get(net_pbuf_t *packet)
{
    net_pbuf_ref(packet);
}

/* Release a reference on a packet. */
void net_packet_put(net_pbuf_t *packet)
{
    net_pbuf_free(packet);
}

/* Payload pointer of a packet, or NULL. */
void *net_packet_data(net_pbuf_t *packet)
{
    return packet ? packet->data : NULL;
}

/* Payload length of a packet, or 0. */
size_t net_packet_length(const net_pbuf_t *packet)
{
    return packet ? packet->length : 0;
}

/* Create a packet buffer containing a copy of data with the given headroom. */
net_pbuf_t *net_pbuf_from(const void *data, size_t length, size_t headroom)
{
    if (!data && length) return NULL;
    net_pbuf_t *pbuf = net_pbuf_alloc(length, headroom);
    if (pbuf && length) memcpy(pbuf->data, data, length);
    return pbuf;
}

/* Copy a packet buffer, leaving the given headroom in front. */
net_pbuf_t *net_pbuf_clone(const net_pbuf_t *pbuf, size_t headroom)
{
    return pbuf ? net_pbuf_from(pbuf->data, pbuf->length, headroom) : NULL;
}

/* Take a reference on a packet buffer. */
void net_pbuf_ref(net_pbuf_t *pbuf)
{
    if (!pbuf) return;
    __atomic_add_fetch(&pbuf->refs, 1, __ATOMIC_SEQ_CST);
}

/* Release a reference on a packet buffer, freeing it when none remain. */
void net_pbuf_free(net_pbuf_t *pbuf)
{
    if (!pbuf) return;
    if (__atomic_sub_fetch(&pbuf->refs, 1, __ATOMIC_SEQ_CST)) return;
    if (pbuf->release) {
        pbuf->release(pbuf->release_context, pbuf->storage);
    } else if (!pbuf->external) {
        free(pbuf->storage);
    }
    free(pbuf);
}

/* Bytes available in front of the payload for prepending a header. */
size_t net_pbuf_headroom(const net_pbuf_t *pbuf)
{
    return pbuf ? (size_t)(pbuf->data - pbuf->storage) : 0;
}

/* Prepend a header by moving the data pointer back within the headroom. */
void *net_pbuf_push(net_pbuf_t *pbuf, size_t length)
{
    if (!pbuf || length > net_pbuf_headroom(pbuf)) return NULL;
    pbuf->data -= length;
    pbuf->length += length;
    return pbuf->data;
}

/* Strip a header by advancing the data pointer. */
void *net_pbuf_pull(net_pbuf_t *pbuf, size_t length)
{
    if (!pbuf || length > pbuf->length) return NULL;
    pbuf->data += length;
    pbuf->length -= length;
    return pbuf->data;
}

/* Truncate the packet to the given length. */
int net_pbuf_trim(net_pbuf_t *pbuf, size_t length)
{
    if (!pbuf || length > pbuf->length) return -EINVAL;
    pbuf->length = length;
    return 0;
}

/* Drive the per-protocol periodic timers from the system tick. */
void net_timer(uint64_t now_ticks)
{
    arp_timer(now_ticks);
    tcp_timer(now_ticks);
    dhcp_timer(now_ticks);
    ndp_timer(now_ticks);
}

#endif
