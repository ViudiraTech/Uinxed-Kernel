/*
 *
 *      inet.h
 *      Linux x86_64 Internet socket ABI and kernel backend adapter.
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NET_ABI_INET_H_
#define INCLUDE_NET_ABI_INET_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#ifndef AF_UNSPEC
#    define AF_UNSPEC 0
#endif
#ifndef AF_INET
#    define AF_INET 2
#endif
#ifndef AF_INET6
#    define AF_INET6 10
#endif

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41
#define IPPROTO_RAW  255

#define SOL_IP   0
#define SOL_TCP  6
#define SOL_IPV6 41

#define IPV6_ADDRFORM        1
#define IPV6_2292PKTINFO     2
#define IPV6_2292HOPOPTS     3
#define IPV6_2292DSTOPTS     4
#define IPV6_2292RTHDR       5
#define IPV6_UNICAST_HOPS    16
#define IPV6_MULTICAST_IF    17
#define IPV6_MULTICAST_HOPS  18
#define IPV6_MULTICAST_LOOP  19
#define IPV6_ADD_MEMBERSHIP  20
#define IPV6_DROP_MEMBERSHIP 21
#define IPV6_MTU_DISCOVER    23
#define IPV6_MTU             24
#define IPV6_RECVERR         25
#define IPV6_V6ONLY          26
#define IPV6_RECVPKTINFO     49
#define IPV6_PKTINFO         50
#define IPV6_RECVHOPLIMIT    51
#define IPV6_HOPLIMIT        52
#define IPV6_RECVTCLASS      66
#define IPV6_TCLASS          67
#define IPV6_TRANSPARENT     75
#define IPV6_FREEBIND        78

#define IP_TOS             1
#define IP_TTL             2
#define IP_HDRINCL         3
#define IP_OPTIONS         4
#define IP_PKTINFO         8
#define IP_MTU_DISCOVER    10
#define IP_RECVERR         11
#define IP_RECVTTL         12
#define IP_MTU             14
#define IP_FREEBIND        15
#define IP_TRANSPARENT     19
#define IP_MULTICAST_IF    32
#define IP_MULTICAST_TTL   33
#define IP_MULTICAST_LOOP  34
#define IP_ADD_MEMBERSHIP  35
#define IP_DROP_MEMBERSHIP 36

#define TCP_NODELAY      1
#define TCP_MAXSEG       2
#define TCP_CORK         3
#define TCP_KEEPIDLE     4
#define TCP_KEEPINTVL    5
#define TCP_KEEPCNT      6
#define TCP_SYNCNT       7
#define TCP_LINGER2      8
#define TCP_DEFER_ACCEPT 9
#define TCP_INFO         11
#define TCP_QUICKACK     12
#define TCP_CONGESTION   13

typedef struct socket_timeval {
        int64_t tv_sec;
        int64_t tv_usec;
} socket_timeval_t;

typedef uint16_t sa_family_t;
typedef uint32_t in_addr_t;

typedef struct sockaddr {
        sa_family_t sa_family;
        char        sa_data[14];
} sockaddr_t;

typedef struct in_addr {
        in_addr_t s_addr;
} in_addr_t_struct;

typedef struct sockaddr_in {
        sa_family_t    sin_family;
        uint16_t       sin_port;
        struct in_addr sin_addr;
        uint8_t        sin_zero[8];
} sockaddr_in_t;

typedef struct in6_addr {
        union {
                uint8_t  u6_addr8[16];
                uint16_t u6_addr16[8];
                uint32_t u6_addr32[4];
        } in6_u;
} in6_addr_t;

#define IN6ADDR_ANY_INIT                                       \
    {                                                          \
        {                                                      \
            {                                                  \
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 \
            }                                                  \
        }                                                      \
    }
#define IN6ADDR_LOOPBACK_INIT                                  \
    {                                                          \
        {                                                      \
            {                                                  \
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 \
            }                                                  \
        }                                                      \
    }

#define s6_addr   in6_u.u6_addr8
#define s6_addr16 in6_u.u6_addr16
#define s6_addr32 in6_u.u6_addr32

typedef struct sockaddr_in6 {
        sa_family_t     sin6_family;
        uint16_t        sin6_port;
        uint32_t        sin6_flowinfo;
        struct in6_addr sin6_addr;
        uint32_t        sin6_scope_id;
} sockaddr_in6_t;

typedef struct ipv6_mreq {
        struct in6_addr ipv6mr_multiaddr;
        uint32_t        ipv6mr_interface;
} ipv6_mreq_t;

#define IPV6_JOIN_GROUP  IPV6_ADD_MEMBERSHIP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP

typedef struct sockaddr_storage {
        sa_family_t ss_family;
        uint8_t     __data[118];
        uint64_t    __align;
} sockaddr_storage_t;

#define IFNAMSIZ 16

typedef struct ifmap {
        uint64_t mem_start;
        uint64_t mem_end;
        uint16_t base_addr;
        uint8_t  irq;
        uint8_t  dma;
        uint8_t  port;
} ifmap_t;

typedef struct ifreq {
        char ifr_name[IFNAMSIZ];
        union {
                struct sockaddr ifru_addr;
                struct sockaddr ifru_dstaddr;
                struct sockaddr ifru_broadaddr;
                struct sockaddr ifru_netmask;
                struct sockaddr ifru_hwaddr;
                int16_t         ifru_flags;
                int32_t         ifru_ivalue;
                int32_t         ifru_mtu;
                struct ifmap    ifru_map;
                char            ifru_slave[IFNAMSIZ];
                char            ifru_newname[IFNAMSIZ];
                void           *ifru_data;
        } ifr_ifru;
} ifreq_t;

#define ifr_addr      ifr_ifru.ifru_addr
#define ifr_dstaddr   ifr_ifru.ifru_dstaddr
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_netmask   ifr_ifru.ifru_netmask
#define ifr_hwaddr    ifr_ifru.ifru_hwaddr
#define ifr_flags     ifr_ifru.ifru_flags
#define ifr_ifindex   ifr_ifru.ifru_ivalue
#define ifr_mtu       ifr_ifru.ifru_mtu

#define SIOCGIFNAME    0x8910
#define SIOCSIFLINK    0x8911
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFDSTADDR 0x8917
#define SIOCSIFDSTADDR 0x8918
#define SIOCGIFBRDADDR 0x8919
#define SIOCSIFBRDADDR 0x891a
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFMETRIC  0x891d
#define SIOCSIFMETRIC  0x891e
#define SIOCGIFMTU     0x8921
#define SIOCSIFMTU     0x8922
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933

#define IFF_UP        0x0001
#define IFF_BROADCAST 0x0002
#define IFF_RUNNING   0x0040
#define IFF_MULTICAST 0x1000

enum inet_proc_file {
    INET_PROC_DEV,
    INET_PROC_ARP,
    INET_PROC_ROUTE,
    INET_PROC_TCP,
    INET_PROC_UDP,
};

struct inet_backend_ops {
        int (*create)(int family, int type, int protocol, uint32_t flags, void **context);
        void (*close)(void *context);
        int (*bind)(void *context, const struct sockaddr *addr, uint32_t addrlen);
        int (*connect)(void *context, const struct sockaddr *addr, uint32_t addrlen, uint32_t flags);
        int (*listen)(void *context, int backlog);
        int (*accept)(void *context, void **accepted_context, struct sockaddr *addr, uint32_t *addrlen, uint32_t flags);
        int (*sendto)(void *context, const void *buf, size_t len, int flags, const struct sockaddr *addr, uint32_t addrlen);
        int (*recvfrom)(void *context, void *buf, size_t len, int flags, struct sockaddr *addr, uint32_t *addrlen);
        int (*shutdown)(void *context, int how);
        int (*getsockname)(void *context, struct sockaddr *addr, uint32_t *addrlen);
        int (*getpeername)(void *context, struct sockaddr *addr, uint32_t *addrlen);
        int (*setsockopt)(void *context, int level, int optname, const void *value, uint32_t length);
        int (*getsockopt)(void *context, int level, int optname, void *value, uint32_t *length);
        int (*poll)(void *context, size_t events);
        void (*set_event_callback)(void *context, void (*callback)(void *argument, uint32_t events), void *argument);
        int (*ioctl)(void *context, size_t request, struct ifreq *ifr);
        size_t (*proc_read)(enum inet_proc_file file, char *buf, size_t capacity);
};

int                            inet_backend_register(const struct inet_backend_ops *ops);
const struct inet_backend_ops *inet_backend_get(void);
size_t                         inet_backend_proc_read(enum inet_proc_file file, char *buf, size_t capacity);
int                            inet_builtin_backend_register(void);

_Static_assert(sizeof(struct sockaddr_in) == 16, "Linux sockaddr_in ABI");
_Static_assert(sizeof(struct in6_addr) == 16, "Linux in6_addr ABI");
_Static_assert(sizeof(struct sockaddr_in6) == 28, "Linux sockaddr_in6 ABI");
_Static_assert(sizeof(struct ipv6_mreq) == 20, "Linux ipv6_mreq ABI");
_Static_assert(sizeof(struct sockaddr_storage) == 128, "Linux sockaddr_storage ABI");
_Static_assert(_Alignof(struct sockaddr_storage) == 8, "Linux sockaddr_storage alignment");
_Static_assert(sizeof(struct ifreq) == 40, "Linux x86_64 ifreq ABI");

#endif
