#include "net_epoll_mocks.h"
#include <ipc/epoll.h>
#include <stdio.h>

static int failures;
_Static_assert(sizeof(epoll_event_t) == 12, "epoll_event size");
_Static_assert(__builtin_offsetof(epoll_event_t, data) == 4, "epoll_event offset");
#define CHECK(x, m) do { if (!(x)) { printf("FAIL %s: %s\n", __func__, m); failures++; goto out; } } while (0)
static epoll_event_t ev(uint32_t bits, uint64_t data) { epoll_event_t e = {.events = bits}; e.data.u64 = data; return e; }

static void test_initial_and_wake(void)
{
    mock_epoll_reset(); mock_target_t *t = mock_target_open(EPOLLIN); int fd = mock_target_fd(t); int ep = sys_epoll_create1(0); epoll_event_t add = ev(EPOLLIN, 1), got;
    CHECK(t && fd >= 0 && ep >= 0, "setup"); CHECK(sys_epoll_ctl(ep, EPOLL_CTL_ADD, fd, &add) == 0, "add"); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 1 && got.data.u64 == 1, "initial readiness");
out: mock_epoll_reset();
}
static mock_target_t *wake_target;
static void wake_hook(void) { mock_target_set_ready(wake_target, EPOLLIN, 1); }
static void test_subscription_wake(void)
{
    mock_epoll_reset(); wake_target = mock_target_open(0); int fd = mock_target_fd(wake_target); int ep = sys_epoll_create1(0); epoll_event_t add = ev(EPOLLIN, 2), got;
    CHECK(wake_target && fd >= 0 && ep >= 0, "setup"); CHECK(sys_epoll_ctl(ep, EPOLL_CTL_ADD, fd, &add) == 0, "add"); mock_set_sleep_hook(wake_hook); CHECK(sys_epoll_wait(ep, &got, 1, -1) == 1 && got.events == EPOLLIN, "subscription wake"); CHECK(mock_sleep_calls() == 1 && mock_sched_ticks() == 0, "tick polling");
out: mock_epoll_reset();
}
static void test_ofd_close_reuse(void)
{
    mock_epoll_reset(); mock_target_t *t = mock_target_open(0); int fd = mock_target_fd(t); int ep = sys_epoll_create1(0); int dup = mock_fd_dup(fd); epoll_event_t add = ev(EPOLLIN, 3), got;
    CHECK(t && fd >= 0 && ep >= 0 && dup >= 0, "setup"); CHECK(sys_epoll_ctl(ep, EPOLL_CTL_ADD, fd, &add) == 0, "add"); CHECK(mock_fd_close(fd) == 0, "close"); mock_target_t *replacement = mock_target_open(0); CHECK(replacement && mock_target_fd(replacement) == fd, "fd reuse"); mock_target_set_ready(t, EPOLLIN, 1); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 1 && got.data.u64 == 3, "retained OFD");
out: mock_epoll_reset();
}
static void test_del_unsubscribes(void)
{
    mock_epoll_reset(); mock_target_t *t = mock_target_open(0); int fd = mock_target_fd(t); int ep = sys_epoll_create1(0); epoll_event_t add = ev(EPOLLIN, 4), got;
    CHECK(t && fd >= 0 && ep >= 0, "setup"); CHECK(sys_epoll_ctl(ep, EPOLL_CTL_ADD, fd, &add) == 0, "add"); CHECK(mock_target_subscribers(t) == 1, "subscribe"); CHECK(sys_epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL) == 0, "del"); CHECK(mock_target_subscribers(t) == 0, "unsubscribe"); mock_target_set_ready(t, EPOLLIN, 1); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 0, "DEL event");
out: mock_epoll_reset();
}
static void test_target_close_hup(void)
{
    mock_epoll_reset(); mock_target_t *t = mock_target_open(0); int fd = mock_target_fd(t); int ep = sys_epoll_create1(0); epoll_event_t add = ev(EPOLLIN, 5), got;
    CHECK(t && fd >= 0 && ep >= 0, "setup"); CHECK(sys_epoll_ctl(ep, EPOLL_CTL_ADD, fd, &add) == 0, "add"); CHECK(mock_fd_close(fd) == 0, "target close"); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 1 && (got.events & EPOLLHUP), "close HUP");
out: mock_epoll_reset();
}
static void test_level(void)
{
    mock_epoll_reset(); mock_target_t *t = mock_target_open(EPOLLIN); int fd = mock_target_fd(t); int ep = sys_epoll_create1(0); epoll_event_t add = ev(EPOLLIN, 6), got;
    CHECK(t && fd >= 0 && ep >= 0, "setup"); CHECK(sys_epoll_ctl(ep, EPOLL_CTL_ADD, fd, &add) == 0, "add"); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 1, "first level event"); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 1, "level repeat");
out: mock_epoll_reset();
}
static void test_edge(void)
{
    mock_epoll_reset(); mock_target_t *t = mock_target_open(EPOLLIN); int fd = mock_target_fd(t); int ep = sys_epoll_create1(0); epoll_event_t add = ev(EPOLLIN | EPOLLET, 7), got;
    CHECK(t && fd >= 0 && ep >= 0, "setup"); CHECK(sys_epoll_ctl(ep, EPOLL_CTL_ADD, fd, &add) == 0, "add"); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 1, "initial edge"); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 0, "edge repeat"); mock_target_set_ready(t, EPOLLIN, 0); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 0, "falling edge"); mock_target_set_ready(t, EPOLLIN, 1); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 1, "new edge");
out: mock_epoll_reset();
}
static void test_oneshot(void)
{
    mock_epoll_reset(); mock_target_t *t = mock_target_open(EPOLLIN); int fd = mock_target_fd(t); int ep = sys_epoll_create1(0); epoll_event_t add = ev(EPOLLIN | EPOLLONESHOT, 8), got;
    CHECK(t && fd >= 0 && ep >= 0, "setup"); CHECK(sys_epoll_ctl(ep, EPOLL_CTL_ADD, fd, &add) == 0, "add"); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 1, "first event"); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 0, "disabled repeat"); add.data.u64 = 9; CHECK(sys_epoll_ctl(ep, EPOLL_CTL_MOD, fd, &add) == 0, "rearm"); CHECK(sys_epoll_wait(ep, &got, 1, 0) == 1 && got.data.u64 == 9, "rearmed event");
out: mock_epoll_reset();
}
static void test_absolute_timeout(void)
{
    mock_epoll_reset(); int ep = sys_epoll_create1(0); epoll_event_t got; mock_set_sched_ticks(1000); CHECK(ep >= 0, "setup"); CHECK(sys_epoll_wait(ep, &got, 1, 25) == 0, "timeout"); CHECK(mock_timed_wait_calls() == 1 && mock_last_deadline() == 1003 && mock_sched_ticks() == 1003, "single absolute expiry");
out: mock_epoll_reset();
}
int main(void)
{
    mock_epoll_init(); test_initial_and_wake(); test_subscription_wake(); test_ofd_close_reuse(); test_del_unsubscribes(); test_target_close_hup(); test_level(); test_edge(); test_oneshot(); test_absolute_timeout();
    if (failures) { printf("%d production epoll test(s) failed\n", failures); return 1; } printf("PASS production epoll subscription host tests\n"); return 0;
}
