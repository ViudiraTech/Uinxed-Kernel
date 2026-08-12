/*
 *
 *      dhcp.c
 *      DHCP client implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <net/core/endian.h>
#include <net/ipv4/dhcp.h>
#include <net/transport/udp.h>

#define DHCP_SERVER_PORT      67U
#define DHCP_CLIENT_PORT      68U
#define DHCP_FIXED_LENGTH     240U
#define DHCP_PACKET_CAPACITY  576U
#define DHCP_TICKS_PER_SECOND TIMER_HZ
#define DHCP_INITIAL_RETRY    DHCP_TICKS_PER_SECOND
#define DHCP_MAX_RETRY        (16U * DHCP_TICKS_PER_SECOND)
#define DHCP_RETRY_LIMIT      5U
#define DHCP_DEFAULT_LEASE    3600U
#define DHCP_MAGIC_COOKIE     0x63825363U

/*
 * DHCP client: sends DISCOVER/REQUEST over UDP, parses OFFER/ACK options,
 * and applies the obtained address, netmask, router and DNS server to the
 * interface, with lease-renewal retry logic.
 */

#define DHCP_OPT_PAD            0U
#define DHCP_OPT_NETMASK        1U
#define DHCP_OPT_ROUTER         3U
#define DHCP_OPT_DNS            6U
#define DHCP_OPT_REQUESTED_IP   50U
#define DHCP_OPT_LEASE          51U
#define DHCP_OPT_MESSAGE_TYPE   53U
#define DHCP_OPT_SERVER_ID      54U
#define DHCP_OPT_PARAMETER_LIST 55U
#define DHCP_OPT_MAX_MESSAGE    57U
#define DHCP_OPT_RENEWAL        58U
#define DHCP_OPT_REBINDING      59U
#define DHCP_OPT_CLIENT_ID      61U
#define DHCP_OPT_END            255U

#define DHCP_DISCOVER 1U
#define DHCP_OFFER    2U
#define DHCP_REQUEST  3U
#define DHCP_ACK      5U
#define DHCP_NAK      6U

typedef enum dhcp_state {
    DHCP_STATE_WAIT_LINK,
    DHCP_STATE_SELECTING,
    DHCP_STATE_REQUESTING,
    DHCP_STATE_BOUND,
    DHCP_STATE_RENEWING,
    DHCP_STATE_REBINDING,
    DHCP_STATE_RESTART,
    DHCP_STATE_DORMANT,
} dhcp_state_t;

typedef struct dhcp_client {
        net_device_t *device;
        dhcp_state_t  state;
        uint32_t      xid;
        uint32_t      offered_address;
        uint32_t      server_identifier;
        uint8_t       retries;
        uint8_t       link_running;
        uint64_t      next_action;
        uint64_t      lease_expiry;
        uint64_t      renewal_at;
        uint64_t      rebinding_at;
} dhcp_client_t;

static dhcp_client_t   clients[NETDEV_MAX];
static udp_endpoint_t *dhcp_endpoint;
static spinlock_t      dhcp_lock;
static uint32_t        xid_sequence = 0x55495844U;

/* Convert a lease duration in seconds to scheduler ticks. */
static uint64_t dhcp_seconds_to_ticks(uint32_t seconds)
{
    return (uint64_t)seconds * DHCP_TICKS_PER_SECOND;
}

/* Compute a deadline tick count from now + delay, saturating on overflow. */
static uint64_t dhcp_deadline(uint64_t now, uint64_t delay)
{
    return delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
}

/* Exponential backoff delay for a retry count, capped at the maximum. */
static uint64_t dhcp_retry_delay(uint8_t retries)
{
    uint64_t delay = DHCP_INITIAL_RETRY << (retries > 4 ? 4 : retries);
    return delay > (uint64_t)DHCP_MAX_RETRY ? (uint64_t)DHCP_MAX_RETRY : delay;
}

/* Bump the retry counter, capped at the configured limit. */
static void dhcp_count_retry(dhcp_client_t *client)
{
    if (client->retries < DHCP_RETRY_LIMIT) client->retries++;
}

/* Infer a classful default netmask when the server sends none. */
static uint32_t dhcp_default_netmask(uint32_t address)
{
    uint8_t first = (uint8_t)(address >> 24);
    if (first < 128U) return 0xff000000U;
    if (first < 192U) return 0xffff0000U;
    return 0xffffff00U;
}

/* Generate a transaction ID mixed from the clock, device, and prior value. */
static uint32_t dhcp_new_xid(dhcp_client_t *client, uint64_t now)
{
    xid_sequence = xid_sequence * 1664525U + 1013904223U + (uint32_t)now + client->device->ifindex;
    for (unsigned i = 0; i < 6; i++) xid_sequence = (xid_sequence << 5) ^ (xid_sequence >> 2) ^ client->device->address[i];
    return xid_sequence ? xid_sequence : ++xid_sequence;
}

/* Parse DHCP options into the reply struct, rejecting malformed input. */
static int dhcp_parse_options(const uint8_t *options, size_t length, dhcp_reply_t *reply)
{
    size_t offset = 0;
    int    ended  = 0;
    while (offset < length) {
        uint8_t code = options[offset++];
        if (code == DHCP_OPT_PAD) continue;
        if (code == DHCP_OPT_END) {
            ended = 1;
            break;
        }
        if (offset >= length) return -EBADMSG;
        size_t option_length = options[offset++];
        if (option_length > length - offset) return -EBADMSG;
        const uint8_t *value = options + offset;
        switch (code) {
            case DHCP_OPT_MESSAGE_TYPE :
                if (option_length != 1) return -EBADMSG;
                reply->message_type = value[0];
                break;
            case DHCP_OPT_SERVER_ID :
                if (option_length != 4) return -EBADMSG;
                reply->server_identifier = net_read_be32(value);
                break;
            case DHCP_OPT_NETMASK :
                if (option_length != 4) return -EBADMSG;
                reply->netmask     = net_read_be32(value);
                reply->has_netmask = 1;
                break;
            case DHCP_OPT_ROUTER :
                if (option_length < 4 || (option_length & 3U)) return -EBADMSG;
                reply->gateway     = net_read_be32(value);
                reply->has_gateway = 1;
                break;
            case DHCP_OPT_DNS :
                if (!option_length || (option_length & 3U)) return -EBADMSG;
                reply->dns_count = (uint8_t)(option_length / 4U > NETDEV_DNS_MAX ? NETDEV_DNS_MAX : option_length / 4U);
                for (uint8_t i = 0; i < reply->dns_count; i++) reply->dns[i] = net_read_be32(value + (size_t)i * 4U);
                reply->has_dns = 1;
                break;
            case DHCP_OPT_LEASE :
                if (option_length != 4) return -EBADMSG;
                reply->lease_seconds = net_read_be32(value);
                reply->has_lease     = 1;
                break;
            case DHCP_OPT_RENEWAL :
                if (option_length != 4) return -EBADMSG;
                reply->renewal_seconds = net_read_be32(value);
                reply->has_renewal     = 1;
                break;
            case DHCP_OPT_REBINDING :
                if (option_length != 4) return -EBADMSG;
                reply->rebinding_seconds = net_read_be32(value);
                reply->has_rebinding     = 1;
                break;
            default :
                break;
        }
        offset += option_length;
    }
    return ended && reply->message_type ? 0 : -EBADMSG;
}

/* Validate a DHCP reply packet against the expected transaction and client. */
int dhcp_parse_reply(const void *data, size_t length, uint32_t expected_xid, const uint8_t hardware_address[6], dhcp_reply_t *reply)
{
    if (!data || !hardware_address || !reply || length < DHCP_FIXED_LENGTH) return -EBADMSG;
    const uint8_t *packet = data;
    if (packet[0] != 2 || packet[1] != 1 || packet[2] != 6 || net_read_be32(packet + 4) != expected_xid
        || memcmp(packet + 28, hardware_address, 6) != 0 || net_read_be32(packet + 236) != DHCP_MAGIC_COOKIE)
        return -EBADMSG;
    memset(reply, 0, sizeof(*reply));
    reply->offered_address = net_read_be32(packet + 16);
    return dhcp_parse_options(packet + DHCP_FIXED_LENGTH, length - DHCP_FIXED_LENGTH, reply);
}

/* Append one TLV option to the packet, returning the new offset. */
static size_t dhcp_add_option(uint8_t *packet, size_t offset, uint8_t code, const void *value, uint8_t length)
{
    packet[offset++] = code;
    packet[offset++] = length;
    memcpy(packet + offset, value, length);
    return offset + length;
}

/* Build and transmit a DHCP message, broadcast or unicast to the server. */
static int dhcp_send(dhcp_client_t *client, uint8_t message_type, int broadcast)
{
    uint8_t packet[DHCP_PACKET_CAPACITY];
    memset(packet, 0, sizeof(packet));
    packet[0] = 1;
    packet[1] = 1;
    packet[2] = 6;
    net_write_be32(packet + 4, client->xid);
    if (broadcast) net_write_be16(packet + 10, 0x8000U);
    if (client->state == DHCP_STATE_RENEWING || client->state == DHCP_STATE_REBINDING) net_write_be32(packet + 12, client->device->ipv4_address);
    memcpy(packet + 28, client->device->address, 6);
    net_write_be32(packet + 236, DHCP_MAGIC_COOKIE);

    size_t offset                = DHCP_FIXED_LENGTH;
    offset                       = dhcp_add_option(packet, offset, DHCP_OPT_MESSAGE_TYPE, &message_type, 1);
    uint8_t client_identifier[7] = {1};
    memcpy(client_identifier + 1, client->device->address, 6);
    offset = dhcp_add_option(packet, offset, DHCP_OPT_CLIENT_ID, client_identifier, sizeof(client_identifier));
    if (client->state == DHCP_STATE_REQUESTING) {
        uint8_t address[4];
        net_write_be32(address, client->offered_address);
        offset = dhcp_add_option(packet, offset, DHCP_OPT_REQUESTED_IP, address, sizeof(address));
        net_write_be32(address, client->server_identifier);
        offset = dhcp_add_option(packet, offset, DHCP_OPT_SERVER_ID, address, sizeof(address));
    }
    static const uint8_t parameters[]
        = {DHCP_OPT_NETMASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS, DHCP_OPT_LEASE, DHCP_OPT_SERVER_ID, DHCP_OPT_RENEWAL, DHCP_OPT_REBINDING};
    offset = dhcp_add_option(packet, offset, DHCP_OPT_PARAMETER_LIST, parameters, sizeof(parameters));
    uint8_t maximum[2];
    net_write_be16(maximum, DHCP_PACKET_CAPACITY);
    offset           = dhcp_add_option(packet, offset, DHCP_OPT_MAX_MESSAGE, maximum, sizeof(maximum));
    packet[offset++] = DHCP_OPT_END;
    if (offset < 300U) offset = 300U;

    if (broadcast) {
        int status = netdev_udp_broadcast(client->device, client->device->ipv4_address, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, packet, offset);
        if (status < 0) plogk("dhcp: %s: Broadcast failed (%d)\n", client->device->name, status);
        return status < 0 ? status : 0;
    }
    int status = udp_send(dhcp_endpoint, packet, offset, client->server_identifier, DHCP_SERVER_PORT);
    if (status < 0) plogk("dhcp: %s: Send to server failed (%d)\n", client->device->name, status);
    return status < 0 ? status : 0;
}

/* Drop the address/DNS configuration obtained from the server. */
static void dhcp_clear_configuration(dhcp_client_t *client)
{
    netdev_configure_ipv4(client->device, 0, 0, 0);
    netdev_configure_dns(client->device, NULL, 0);
    client->lease_expiry = client->renewal_at = client->rebinding_at = 0;
}

/* Enter the SELECTING state and broadcast a DISCOVER. */
static void dhcp_begin_discovery(dhcp_client_t *client, uint64_t now)
{
    client->state             = DHCP_STATE_SELECTING;
    client->xid               = dhcp_new_xid(client, now);
    client->offered_address   = 0;
    client->server_identifier = 0;
    client->retries           = 0;
    dhcp_send(client, DHCP_DISCOVER, 1);
    client->next_action = dhcp_deadline(now, dhcp_retry_delay(client->retries));
    dhcp_count_retry(client);
}

/* Clear the lease and restart discovery after the given delay. */
static void dhcp_schedule_restart(dhcp_client_t *client, uint64_t now, uint64_t delay)
{
    dhcp_clear_configuration(client);
    client->state       = DHCP_STATE_RESTART;
    client->next_action = dhcp_deadline(now, delay);
    client->retries     = 0;
}

/* Configure the interface from an ACK and enter the BOUND state. */
static int dhcp_apply_lease(dhcp_client_t *client, const dhcp_reply_t *reply, uint64_t now)
{
    uint32_t address = reply->offered_address ? reply->offered_address : client->device->ipv4_address;
    uint32_t netmask = reply->has_netmask ? reply->netmask : client->device->ipv4_netmask;
    uint32_t gateway = reply->has_gateway ? reply->gateway : client->device->ipv4_gateway;
    if (!netmask) netmask = dhcp_default_netmask(address);
    if (!address || netdev_configure_ipv4(client->device, address, netmask, gateway)) {
        plogk("dhcp: %s: Lease apply failed (address=%u.%u.%u.%u)\n", client->device->name, (unsigned)(address >> 24) & 0xff,
              (unsigned)(address >> 16) & 0xff, (unsigned)(address >> 8) & 0xff, (unsigned)address & 0xff);
        return -EINVAL;
    }
    if (reply->has_dns) netdev_configure_dns(client->device, reply->dns, reply->dns_count);

    uint32_t lease = reply->has_lease && reply->lease_seconds ? reply->lease_seconds : DHCP_DEFAULT_LEASE;
    uint32_t t1    = reply->has_renewal ? reply->renewal_seconds : lease / 2U;
    uint32_t t2    = reply->has_rebinding ? reply->rebinding_seconds : lease - lease / 8U;
    if (!t1 || t1 >= lease) t1 = lease / 2U;
    if (t2 <= t1 || t2 >= lease) t2 = lease - lease / 8U;
    if (!t1) t1 = 1;
    if (t2 <= t1) t2 = t1 + 1U < lease ? t1 + 1U : t1;

    client->server_identifier = reply->server_identifier ? reply->server_identifier : client->server_identifier;
    client->lease_expiry      = dhcp_deadline(now, dhcp_seconds_to_ticks(lease));
    client->renewal_at        = dhcp_deadline(now, dhcp_seconds_to_ticks(t1));
    client->rebinding_at      = dhcp_deadline(now, dhcp_seconds_to_ticks(t2));
    client->state             = DHCP_STATE_BOUND;
    client->retries           = 0;
    return 0;
}

/* Locate a client by transaction ID and hardware address. */
static dhcp_client_t *dhcp_find_client(uint32_t xid, const uint8_t hardware_address[6])
{
    for (unsigned i = 0; i < NETDEV_MAX; i++)
        if (clients[i].device && clients[i].xid == xid && !memcmp(clients[i].device->address, hardware_address, 6)) return &clients[i];
    return NULL;
}

/* Drain UDP datagrams and drive OFFER/ACK/NAK state transitions. */
static void dhcp_receive_replies(uint64_t now)
{
    uint8_t        packet[DHCP_PACKET_CAPACITY];
    udp_datagram_t datagram;
    int            length;
    while ((length = udp_receive(dhcp_endpoint, packet, sizeof(packet), &datagram, 0)) >= 0) {
        if (datagram.length > sizeof(packet) || datagram.source_port != DHCP_SERVER_PORT || length < (int)DHCP_FIXED_LENGTH) continue;
        uint32_t xid = net_read_be32(packet + 4);
        spin_lock(&dhcp_lock);
        dhcp_client_t *client = dhcp_find_client(xid, packet + 28);
        dhcp_reply_t   reply;
        if (!client || !client->link_running || dhcp_parse_reply(packet, (size_t)length, xid, client->device->address, &reply)) {
            spin_unlock(&dhcp_lock);
            continue;
        }
        if (reply.message_type == DHCP_OFFER && client->state == DHCP_STATE_SELECTING && reply.offered_address && reply.server_identifier
            && (!datagram.source_address || datagram.source_address == reply.server_identifier)) {
            client->offered_address   = reply.offered_address;
            client->server_identifier = reply.server_identifier;
            client->state             = DHCP_STATE_REQUESTING;
            client->retries           = 0;
            dhcp_send(client, DHCP_REQUEST, 1);
            client->next_action = dhcp_deadline(now, dhcp_retry_delay(client->retries));
            dhcp_count_retry(client);
        } else if (reply.message_type == DHCP_ACK
                   && (client->state == DHCP_STATE_REQUESTING || client->state == DHCP_STATE_RENEWING || client->state == DHCP_STATE_REBINDING)
                   && (client->state != DHCP_STATE_REQUESTING || reply.server_identifier == client->server_identifier)
                   && (client->state != DHCP_STATE_REQUESTING || reply.offered_address == client->offered_address)) {
            dhcp_apply_lease(client, &reply, now);
        } else if (reply.message_type == DHCP_NAK
                   && (client->state == DHCP_STATE_REQUESTING || client->state == DHCP_STATE_RENEWING || client->state == DHCP_STATE_REBINDING)
                   && (client->state != DHCP_STATE_REQUESTING || !reply.server_identifier
                       || reply.server_identifier == client->server_identifier)) {
            plogk("dhcp: %s: Server NAK, restarting.\n", client->device->name);
            dhcp_schedule_restart(client, now, DHCP_INITIAL_RETRY);
        }
        spin_unlock(&dhcp_lock);
    }
}

/* Attach a client record to each broadcast-capable device, once. */
static void dhcp_track_device(net_device_t *device, void *context)
{
    (void)context;
    if (!(device->flags & NETDEV_F_BROADCAST)) return;
    spin_lock(&dhcp_lock);
    for (unsigned i = 0; i < NETDEV_MAX; i++) {
        if (clients[i].device == device) {
            spin_unlock(&dhcp_lock);
            return;
        }
    }
    for (unsigned i = 0; i < NETDEV_MAX; i++) {
        if (!clients[i].device) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].device = device;
            clients[i].state  = DHCP_STATE_WAIT_LINK;
            netdev_get(device);
            break;
        }
    }
    spin_unlock(&dhcp_lock);
}

/* Advance the client state machine: link, discovery, renewal, and expiry. */
static void dhcp_advance(dhcp_client_t *client, uint64_t now)
{
    int running = (client->device->flags & (NETDEV_F_UP | NETDEV_F_RUNNING)) == (NETDEV_F_UP | NETDEV_F_RUNNING);
    if (!running) {
        if (client->link_running) dhcp_clear_configuration(client);
        client->link_running = 0;
        client->state        = DHCP_STATE_WAIT_LINK;
        return;
    }
    if (!client->link_running) {
        client->link_running = 1;
        dhcp_begin_discovery(client, now);
        return;
    }
    if (client->state == DHCP_STATE_BOUND && now >= client->renewal_at) {
        client->state   = DHCP_STATE_RENEWING;
        client->xid     = dhcp_new_xid(client, now);
        client->retries = 0;
        dhcp_send(client, DHCP_REQUEST, 0);
        client->next_action = dhcp_deadline(now, dhcp_retry_delay(client->retries));
        dhcp_count_retry(client);
        return;
    }
    if (client->state == DHCP_STATE_RENEWING && now >= client->rebinding_at) {
        client->state   = DHCP_STATE_REBINDING;
        client->xid     = dhcp_new_xid(client, now);
        client->retries = 0;
        dhcp_send(client, DHCP_REQUEST, 1);
        client->next_action = dhcp_deadline(now, dhcp_retry_delay(client->retries));
        dhcp_count_retry(client);
        return;
    }
    if ((client->state == DHCP_STATE_RENEWING || client->state == DHCP_STATE_REBINDING) && now >= client->lease_expiry) {
        plogk("dhcp: %s: Lease expired, restarting.\n", client->device->name);
        dhcp_schedule_restart(client, now, 0);
        return;
    }
    if (client->state == DHCP_STATE_RESTART && now >= client->next_action) {
        dhcp_begin_discovery(client, now);
        return;
    }
    if ((client->state == DHCP_STATE_SELECTING || client->state == DHCP_STATE_REQUESTING) && now >= client->next_action) {
        if (client->retries >= DHCP_RETRY_LIMIT) {
            plogk("dhcp: %s: No reply after %u attempts; suspended until the link changes.\n", client->device->name, (unsigned)DHCP_RETRY_LIMIT);
            dhcp_clear_configuration(client);
            client->state       = DHCP_STATE_DORMANT;
            client->next_action = 0;
            client->retries     = 0;
            return;
        }
        dhcp_send(client, client->state == DHCP_STATE_SELECTING ? DHCP_DISCOVER : DHCP_REQUEST, 1);
        client->next_action = dhcp_deadline(now, dhcp_retry_delay(client->retries));
        dhcp_count_retry(client);
    } else if ((client->state == DHCP_STATE_RENEWING || client->state == DHCP_STATE_REBINDING) && now >= client->next_action) {
        dhcp_send(client, DHCP_REQUEST, client->state == DHCP_STATE_REBINDING);
        client->next_action = dhcp_deadline(now, dhcp_retry_delay(client->retries));
        dhcp_count_retry(client);
    }
}

void dhcp_init(void)
{
    if (dhcp_endpoint) return;
    dhcp_endpoint = udp_open();
    if (!dhcp_endpoint) {
        plogk("dhcp: Endpoint open failed.\n");
        return;
    }
    if (udp_bind(dhcp_endpoint, 0, DHCP_CLIENT_PORT)) {
        plogk("dhcp: Bind to port %u failed.\n", (unsigned)DHCP_CLIENT_PORT);
        udp_close(dhcp_endpoint);
        dhcp_endpoint = NULL;
    }
}

/* Periodic tick: track devices, process replies, and advance each client. */
void dhcp_timer(uint64_t now_ticks)
{
    if (!dhcp_endpoint) return;
    netdev_iterate(dhcp_track_device, NULL);
    dhcp_receive_replies(now_ticks);
    spin_lock(&dhcp_lock);
    for (unsigned i = 0; i < NETDEV_MAX; i++)
        if (clients[i].device) dhcp_advance(&clients[i], now_ticks);
    spin_unlock(&dhcp_lock);
}

/* Release the client record and its device reference on removal. */
void dhcp_device_removed(net_device_t *device)
{
    if (!device) return;
    net_device_t *release = NULL;
    spin_lock(&dhcp_lock);
    for (unsigned i = 0; i < NETDEV_MAX; i++) {
        if (clients[i].device == device) {
            release = clients[i].device;
            memset(&clients[i], 0, sizeof(clients[i]));
            break;
        }
    }
    spin_unlock(&dhcp_lock);
    if (release) netdev_put(release);
}
