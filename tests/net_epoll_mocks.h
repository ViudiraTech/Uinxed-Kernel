#ifndef NET_EPOLL_MOCKS_H
#define NET_EPOLL_MOCKS_H
#include <libs/std/stdint.h>
typedef struct mock_target mock_target_t;
void                       mock_epoll_init(void);
void                       mock_epoll_reset(void);
mock_target_t             *mock_target_open(uint32_t);
int                        mock_target_fd(const mock_target_t *);
void                       mock_target_set_ready(mock_target_t *, uint32_t, int);
unsigned                   mock_target_subscribers(const mock_target_t *);
int                        mock_fd_dup(int);
int                        mock_fd_close(int);
void                       mock_set_sleep_hook(void (*)(void));
unsigned                   mock_sleep_calls(void);
unsigned                   mock_timed_wait_calls(void);
uint64_t                   mock_last_deadline(void);
void                       mock_set_sched_ticks(uint64_t);
uint64_t                   mock_sched_ticks(void);
#endif
