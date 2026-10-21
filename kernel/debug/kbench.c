/*
 *
 *      kbench.c
 *      In-kernel micro-benchmarks for the scheduler, wait queues, pipes and the heap
 *
 *      2026/10/21 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/smp.h>
#include <ipc/pipe.h>
#include <kernel/cmdline/cmdline.h>
#include <kernel/debug/kbench.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <process/kthread.h>
#include <process/sched.h>

#define KBENCH_MAX_THREADS 64U

/* A monotonically increasing sequence that tasks can block on. */
typedef struct {
        wait_queue_t      wq;
        volatile uint64_t seq;
} kb_event_t;

typedef struct kb_run kb_run_t;

/* Per-worker state handed to a benchmark body. */
typedef struct {
        kb_run_t *run;
        uint32_t  index;
        uint32_t  cpu;
        uint32_t  end_cpu;
        uint32_t  migrations;
        uint64_t  start_ns;
        uint64_t  end_ns;
        void     *priv;
} kb_worker_t;

typedef void (*kb_body_t)(kb_worker_t *worker);

/* One benchmark invocation: a set of workers released together. */
struct kb_run {
        kb_body_t   body;
        uint64_t    iters;
        uint64_t    size;
        kb_event_t  start;
        kb_event_t  done;
        kb_event_t  events[KBENCH_MAX_THREADS];
        void       *pipes[KBENCH_MAX_THREADS];
        kb_worker_t workers[KBENCH_MAX_THREADS];
};

static void kb_event_init(kb_event_t *event)
{
    wait_queue_init(&event->wq);
    event->seq = 0;
}

/* Advance the sequence and wake one waiter. */
static void kb_event_post(kb_event_t *event)
{
    spin_lock(&event->wq.lock);
    event->seq++;
    spin_unlock(&event->wq.lock);
    wait_queue_wake_one(&event->wq);
}

/*
 * Advance the sequence and wake one waiter while still holding the lock, so a
 * waiter that observes the new value can reuse the event immediately.
 */
static void kb_event_complete(kb_event_t *event)
{
    spin_lock(&event->wq.lock);
    event->seq++;
    wait_queue_wake_one(&event->wq);
    spin_unlock(&event->wq.lock);
}

/* Advance the sequence and wake every waiter. */
static void kb_event_post_all(kb_event_t *event)
{
    spin_lock(&event->wq.lock);
    event->seq++;
    spin_unlock(&event->wq.lock);
    wait_queue_wake_all(&event->wq);
}

/* Block until the sequence reaches target. */
static void kb_event_wait(kb_event_t *event, uint64_t target)
{
    for (;;) {
        spin_lock(&event->wq.lock);
        if (event->seq >= target) {
            spin_unlock(&event->wq.lock);
            return;
        }
        wait_queue_prepare(&event->wq);
        spin_unlock(&event->wq.lock);
        wait_queue_sleep();
    }
}

/* Worker trampoline: wait for the start signal, time the body, report completion. */
static int kb_worker_main(void *arg)
{
    kb_worker_t *worker = arg;
    kb_run_t    *run    = worker->run;

    kb_event_wait(&run->start, 1);
    uint32_t migrations = current_task()->migration_count;
    worker->start_ns    = timer_monotonic_ns();
    run->body(worker);
    worker->end_ns     = timer_monotonic_ns();
    worker->end_cpu    = get_current_cpu_id();
    worker->migrations = current_task()->migration_count - migrations;
    kb_event_complete(&run->done);
    return 0;
}

/* Scheduler counters summed over every runqueue. */
typedef struct {
        uint64_t steals;
        uint64_t migrations;
        uint64_t ipis;
        uint64_t switches;
        uint64_t wakeups;
} kb_counters_t;

static kb_counters_t kb_counters_before;

static void kb_counters_read(kb_counters_t *counters)
{
    memset(counters, 0, sizeof(*counters));
    for (uint32_t cpu = 0; cpu < sched_cpu_count(); cpu++) {
        eevdf_rq_t *rq = &cpu_rqs[cpu];
        counters->steals += __atomic_load_n(&rq->nr_steals, __ATOMIC_RELAXED);
        counters->migrations += __atomic_load_n(&rq->nr_migrations, __ATOMIC_RELAXED);
        counters->ipis += __atomic_load_n(&rq->reschedule_ipis, __ATOMIC_RELAXED);
        counters->switches += __atomic_load_n(&rq->context_switches, __ATOMIC_RELAXED);
        counters->wakeups += __atomic_load_n(&rq->nr_wakeups, __ATOMIC_RELAXED);
    }
}

/* Print value/divisor with one decimal digit. */
static void kb_report(const char *name, uint64_t value, uint64_t divisor, const char *unit, kb_run_t *run, uint32_t nr)
{
    uint64_t tenths = divisor ? value * 10ULL / divisor : 0;
    char     cpus[96];
    size_t   off = 0;

    cpus[0] = '\0';
    for (uint32_t i = 0; i < nr && off + 12 < sizeof(cpus); i++) {
        kb_worker_t *worker = &run->workers[i];
        off += (size_t)snprintf(cpus + off, sizeof(cpus) - off, "%s%u>%u", i ? "," : "", worker->cpu, worker->end_cpu);
        if (worker->migrations && off + 8 < sizeof(cpus)) off += (size_t)snprintf(cpus + off, sizeof(cpus) - off, "m%u", worker->migrations);
    }
    kb_counters_t after;
    kb_counters_read(&after);
    plogk("kbench: %-24s %8llu.%llu %-6s [%s] steal=%llu migr=%llu ipi=%llu cs=%llu wake=%llu\n", name, tenths / 10ULL, tenths % 10ULL, unit, cpus, after.steals - kb_counters_before.steals,
          (after.migrations - kb_counters_before.migrations) / 2, after.ipis - kb_counters_before.ipis, after.switches - kb_counters_before.switches, after.wakeups - kb_counters_before.wakeups);
}

/*
 * Run body on nr workers placed on the given CPUs and return the wall time
 * from the earliest start to the latest finish.
 */
static uint64_t kb_run_workers(kb_run_t *run, kb_body_t body, uint32_t nr, const uint32_t *cpus)
{
    uint32_t ncpu = sched_cpu_count();

    run->body = body;
    kb_event_init(&run->start);
    kb_event_init(&run->done);
    for (uint32_t i = 0; i < nr; i++) {
        kb_worker_t *worker = &run->workers[i];
        worker->run         = run;
        worker->index       = i;
        worker->cpu         = cpus[i] % ncpu;
        worker->end_cpu     = UINT32_MAX;
        worker->migrations  = 0;
        if (!kthread_run_on_cpu("kbench-worker", kb_worker_main, worker, worker->cpu)) {
            plogk("kbench: worker creation failed\n");
            return 0;
        }
    }

    /* Let every worker reach the start barrier before releasing them. */
    task_sleep_ticks(20);
    kb_counters_read(&kb_counters_before);
    kb_event_post_all(&run->start);
    kb_event_wait(&run->done, nr);

    uint64_t first = UINT64_MAX;
    uint64_t last  = 0;
    for (uint32_t i = 0; i < nr; i++) {
        if (run->workers[i].start_ns < first) first = run->workers[i].start_ns;
        if (run->workers[i].end_ns > last) last = run->workers[i].end_ns;
    }
    return last - first;
}

/* sched_yield() in a loop. */
static void kb_body_yield(kb_worker_t *worker)
{
    for (uint64_t i = 0; i < worker->run->iters; i++) sched_yield();
}

/* Wait-queue ping-pong: even workers serve, odd workers return the token. */
static void kb_body_wq_pingpong(kb_worker_t *worker)
{
    kb_run_t   *run   = worker->run;
    uint32_t    pair  = worker->index & ~1U;
    kb_event_t *ping  = &run->events[pair];
    kb_event_t *pong  = &run->events[pair + 1];
    bool        first = !(worker->index & 1U);

    for (uint64_t i = 1; i <= run->iters; i++) {
        if (first) {
            kb_event_post(ping);
            kb_event_wait(pong, i);
        } else {
            kb_event_wait(ping, i);
            kb_event_post(pong);
        }
    }
}

/* Pipe ping-pong of run->size bytes over two pipes per pair. */
static void kb_body_pipe_pingpong(kb_worker_t *worker)
{
    kb_run_t *run   = worker->run;
    uint32_t  pair  = worker->index & ~1U;
    void     *ab    = run->pipes[pair];
    void     *ba    = run->pipes[pair + 1];
    bool      first = !(worker->index & 1U);
    uint8_t   buf[64];

    memset(buf, 0x5a, sizeof(buf));
    for (uint64_t i = 0; i < run->iters; i++) {
        if (first) {
            pipe_kernel_write(ab, buf, run->size);
            pipe_kernel_read(ba, buf, run->size);
        } else {
            pipe_kernel_read(ab, buf, run->size);
            pipe_kernel_write(ba, buf, run->size);
        }
    }
}

/* Pipe streaming: even workers write run->iters bytes in run->size chunks, odd workers drain them. */
static void kb_body_pipe_stream(kb_worker_t *worker)
{
    kb_run_t *run  = worker->run;
    void     *pipe = run->pipes[worker->index & ~1U];
    uint8_t  *buf  = worker->priv;
    uint64_t  left = run->iters;

    if (!(worker->index & 1U)) {
        while (left) {
            size_t  chunk = left < run->size ? left : run->size;
            int64_t ret   = pipe_kernel_write(pipe, buf, chunk);
            if (ret <= 0) break;
            left -= (uint64_t)ret;
        }
    } else {
        while (left) {
            int64_t ret = pipe_kernel_read(pipe, buf, run->size);
            if (ret <= 0) break;
            left -= (uint64_t)ret;
        }
    }
}

/* malloc()+free() of run->size bytes. */
static void kb_body_malloc(kb_worker_t *worker)
{
    kb_run_t *run = worker->run;
    for (uint64_t i = 0; i < run->iters; i++) {
        void *ptr = malloc(run->size);
        if (!ptr) break;
        *(volatile uint8_t *)ptr = 1;
        free(ptr);
    }
}

/* malloc() a batch of objects, then free them, to exercise slab list churn. */
static void kb_body_malloc_batch(kb_worker_t *worker)
{
    kb_run_t *run   = worker->run;
    void    **slots = worker->priv;
    for (uint64_t round = 0; round < run->iters; round++) {
        for (uint32_t i = 0; i < 256; i++) slots[i] = malloc(run->size);
        for (uint32_t i = 0; i < 256; i++) free(slots[i]);
    }
}

static int kb_nop_thread(void *arg)
{
    kb_event_complete(arg);
    return 0;
}

/* Create and run a trivial kernel thread, waiting for it each time. */
static void kb_body_kthread(kb_worker_t *worker)
{
    kb_event_t *event = &worker->run->events[0];
    kb_event_init(event);
    for (uint64_t i = 1; i <= worker->run->iters; i++) {
        if (!kthread_run("kbench-nop", kb_nop_thread, event)) break;
        kb_event_wait(event, i);
    }
}

/* task_sleep_ticks(1) latency: the extra time past one tick shows sleep-queue/timer cost. */
static void kb_body_sleep(kb_worker_t *worker)
{
    for (uint64_t i = 0; i < worker->run->iters; i++) task_sleep_ticks(1);
}

/* Fill cpus[] with the pair layout (2i, 2i+1), skipping CPU 0 when possible. */
static uint32_t kb_pair_cpus(uint32_t *cpus, uint32_t pairs, bool same_cpu)
{
    uint32_t ncpu = sched_cpu_count();
    uint32_t base = ncpu > 2 ? 1 : 0;
    for (uint32_t p = 0; p < pairs; p++) {
        uint32_t a      = same_cpu ? base + p : base + (2 * p);
        cpus[2 * p]     = a % ncpu;
        cpus[2 * p + 1] = (same_cpu ? a : a + 1) % ncpu;
    }
    return pairs * 2;
}

static void kb_suite(kb_run_t *run)
{
    uint32_t ncpu = sched_cpu_count();
    uint32_t cpus[KBENCH_MAX_THREADS];
    uint32_t pairs = (ncpu > 2 ? ncpu - 1 : ncpu) / 2;
    uint64_t ns;
    uint32_t nr;

    if (!pairs) pairs = 1;
    if (pairs * 2 > KBENCH_MAX_THREADS) pairs = KBENCH_MAX_THREADS / 2;

    /* Scheduler */
    run->iters = 200000;
    cpus[0]    = 1;
    ns         = kb_run_workers(run, kb_body_yield, 1, cpus);
    kb_report("yield/solo", ns, run->iters, "ns/op", run, 1);

    cpus[1] = 1;
    ns      = kb_run_workers(run, kb_body_yield, 2, cpus);
    kb_report("yield/pair-same-cpu", ns, run->iters * 2, "ns/sw", run, 2);

    run->iters = 1000;
    cpus[0]    = 1;
    ns         = kb_run_workers(run, kb_body_sleep, 1, cpus);
    kb_report("sleep/1tick", ns, run->iters, "ns/op", run, 1);

    /* Wait queues */
    run->iters = 100000;
    for (uint32_t i = 0; i < 2; i++) kb_event_init(&run->events[i]);
    nr = kb_pair_cpus(cpus, 1, true);
    ns = kb_run_workers(run, kb_body_wq_pingpong, nr, cpus);
    kb_report("wq-pingpong/same-cpu", ns, run->iters, "ns/rt", run, nr);

    for (uint32_t i = 0; i < 2; i++) kb_event_init(&run->events[i]);
    nr = kb_pair_cpus(cpus, 1, false);
    ns = kb_run_workers(run, kb_body_wq_pingpong, nr, cpus);
    kb_report("wq-pingpong/cross-cpu", ns, run->iters, "ns/rt", run, nr);

    for (uint32_t i = 0; i < pairs * 2; i++) kb_event_init(&run->events[i]);
    nr = kb_pair_cpus(cpus, pairs, false);
    ns = kb_run_workers(run, kb_body_wq_pingpong, nr, cpus);
    kb_report("wq-pingpong/parallel", ns, run->iters, "ns/rt", run, nr);

    /* Pipes */
    for (uint32_t i = 0; i < pairs * 2; i++) {
        run->pipes[i] = pipe_kernel_create();
        if (!run->pipes[i]) {
            plogk("kbench: pipe allocation failed\n");
            return;
        }
    }

    run->iters = 100000;
    run->size  = 1;
    nr         = kb_pair_cpus(cpus, 1, true);
    ns         = kb_run_workers(run, kb_body_pipe_pingpong, nr, cpus);
    kb_report("pipe-pingpong/same-cpu", ns, run->iters, "ns/rt", run, nr);

    nr = kb_pair_cpus(cpus, 1, false);
    ns = kb_run_workers(run, kb_body_pipe_pingpong, nr, cpus);
    kb_report("pipe-pingpong/cross-cpu", ns, run->iters, "ns/rt", run, nr);

    static const uint32_t chunks[] = {4096, 65536};
    for (uint32_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        for (uint32_t i = 0; i < pairs * 2; i++) run->workers[i].priv = malloc(chunks[c]);
        run->size  = chunks[c];
        run->iters = 512ULL << 20;

        for (uint32_t same = 0; same < 2; same++) {
            char name[40];
            nr = kb_pair_cpus(cpus, 1, same);
            ns = kb_run_workers(run, kb_body_pipe_stream, nr, cpus);
            (void)snprintf(name, sizeof(name), "pipe-stream/%uk/%s", chunks[c] / 1024, same ? "same-cpu" : "cross-cpu");
            kb_report(name, run->iters * 1000ULL / (1ULL << 20), ns / 1000000ULL, "MiB/s", run, nr);
        }

        char name[40];
        nr = kb_pair_cpus(cpus, pairs, false);
        ns = kb_run_workers(run, kb_body_pipe_stream, nr, cpus);
        (void)snprintf(name, sizeof(name), "pipe-stream/%uk/parallel", chunks[c] / 1024);
        kb_report(name, run->iters * pairs * 1000ULL / (1ULL << 20), ns / 1000000ULL, "MiB/s", run, nr);
        for (uint32_t i = 0; i < pairs * 2; i++) free(run->workers[i].priv);
    }
    for (uint32_t i = 0; i < pairs * 2; i++) pipe_kernel_destroy(run->pipes[i]);

    /* Heap */
    static const uint32_t sizes[] = {64, 4096, 65536};
    for (uint32_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        char name[40];
        run->size  = sizes[s];
        run->iters = sizes[s] > 8192 ? 20000 : 500000;
        cpus[0]    = 1;
        ns         = kb_run_workers(run, kb_body_malloc, 1, cpus);
        (void)snprintf(name, sizeof(name), "malloc/%u/solo", sizes[s]);
        kb_report(name, ns, run->iters, "ns/op", run, 1);

        nr = ncpu > KBENCH_MAX_THREADS ? KBENCH_MAX_THREADS : ncpu;
        for (uint32_t i = 0; i < nr; i++) cpus[i] = i;
        ns = kb_run_workers(run, kb_body_malloc, nr, cpus);
        (void)snprintf(name, sizeof(name), "malloc/%u/all-cpus", sizes[s]);
        kb_report(name, ns, run->iters, "ns/op", run, nr);
    }

    run->size            = 64;
    run->iters           = 2000;
    cpus[0]              = 1;
    run->workers[0].priv = malloc(256 * sizeof(void *));
    ns                   = kb_run_workers(run, kb_body_malloc_batch, 1, cpus);
    kb_report("malloc/64/batch256", ns, run->iters * 256, "ns/op", run, 1);
    free(run->workers[0].priv);

    /* Thread lifecycle */
    run->iters = 2000;
    cpus[0]    = 1;
    ns         = kb_run_workers(run, kb_body_kthread, 1, cpus);
    kb_report("kthread/create-exit", ns, run->iters, "ns/op", run, 1);
}

static int kbench_main(void *arg)
{
    uint32_t rounds = (uint32_t)(uintptr_t)arg;

    /* Let boot-time workers and init settle first. */
    task_sleep_ticks(3000);
    kb_run_t *run = calloc(1, sizeof(*run));
    if (!run) return -1;

    plogk("kbench: start, %u CPU(s), %u round(s)\n", sched_cpu_count(), rounds);
    for (uint32_t r = 0; r < rounds; r++) {
        plogk("kbench: round %u\n", r + 1);
        kb_suite(run);
    }
    plogk("kbench: done\n");
    free(run);
    return 0;
}

/* Register the benchmark worker when the command line carries kbench[=rounds]. */
void kbench_init(void)
{
    char        buf[16];
    const char *value = cmdline_get_option("kbench", buf, sizeof(buf));
    if (!value) return;

    int rounds = atoi(value);
    if (rounds <= 0) rounds = 1;
    kernel_worker_register("kbench", kbench_main, (void *)(uintptr_t)rounds, NULL);
}
