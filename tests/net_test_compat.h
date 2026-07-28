#ifndef TESTS_NET_TEST_COMPAT_H_
#define TESTS_NET_TEST_COMPAT_H_

#include <net/tcp.h>

/* Keep host tests buildable while tcp_emit's production forward declaration lands. */
static int tcp_emit(tcp_endpoint_t *endpoint, uint32_t sequence, uint32_t acknowledgment, uint8_t flags, const void *data,
                    size_t length, int track);

#endif
