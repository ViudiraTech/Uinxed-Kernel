#ifndef INCLUDE_NET_ICMP_H_
#define INCLUDE_NET_ICMP_H_

#include <net/ipv4.h>

#define ICMP_DEST_UNREACHABLE 3U
#define ICMP_ECHO_REQUEST     8U
#define ICMP_ECHO_REPLY       0U
#define ICMP_TIME_EXCEEDED    11U

int icmp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet);
int icmp_error(net_device_t *device, uint32_t destination, uint8_t type, uint8_t code, const void *original, size_t original_length);

#endif
