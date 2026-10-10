/*
 *
 *      cpumask.c
 *      CPU-list parsing and formatting
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpu/cpumask.h>
#include <kernel/errno.h>
#include <kernel/vsprintf.h>

/* Parse an index without overflow or reading beyond the supplied buffer. */
static int parse_index(const char *buf, size_t size, size_t *at, uint32_t limit, uint32_t *index)
{
    uint32_t value = 0;
    size_t   start = *at;
    while (*at < size && buf[*at] >= '0' && buf[*at] <= '9') {
        uint32_t digit = (uint32_t)(buf[(*at)++] - '0');
        if (value > (UINT32_MAX - digit) / 10) return -ERANGE;
        value = (value * 10) + digit;
    }
    if (*at == start || value >= limit) return -EINVAL;
    *index = value;
    return 0;
}

/* A failure leaves the caller's mask untouched. */
int cpumask_parse_list(const char *buf, size_t size, uint32_t limit, cpumask_t *mask)
{
    if (!buf || !mask || !limit || limit > CONFIG_SCHED_MAX_CPUS) return -EINVAL;
    while (size && (buf[size - 1] == '\n' || buf[size - 1] == ' ' || buf[size - 1] == '\t')) size--;
    size_t at = 0;
    while (at < size && (buf[at] == ' ' || buf[at] == '\t')) at++;
    cpumask_t parsed = {0};
    while (at < size) {
        uint32_t first, last;
        int      rc = parse_index(buf, size, &at, limit, &first);
        if (rc) return rc;
        last = first;
        if (at < size && buf[at] == '-') {
            at++;
            rc = parse_index(buf, size, &at, limit, &last);
            if (rc || last < first) return -EINVAL;
        }
        for (uint32_t cpu = first; cpu <= last; cpu++) cpumask_set_cpu(cpu, &parsed);
        if (at == size) break;
        if (buf[at] != ',') return -EINVAL;
        at++;
        if (at == size) return -EINVAL;
    }
    *mask = parsed;
    return 0;
}

/* snprintf's would-have-written count must never become a buffer offset. */
int cpumask_format_list(char *buf, size_t size, const cpumask_t *mask)
{
    if (!buf || !size || !mask) return -EINVAL;
    size_t at = 0;
    for (uint32_t cpu = cpumask_next(0, mask); cpu < CONFIG_SCHED_MAX_CPUS; cpu = cpumask_next(cpu + 1, mask)) {
        uint32_t last = cpu;
        while (last + 1 < CONFIG_SCHED_MAX_CPUS && cpumask_test_cpu(last + 1, mask)) last++;
        int n = last == cpu ? snprintf(buf + at, size - at, "%s%u", at ? "," : "", cpu) : snprintf(buf + at, size - at, "%s%u-%u", at ? "," : "", cpu, last);
        if (n < 0 || (size_t)n >= size - at) return -ENOSPC;
        at += (size_t)n;
        cpu = last;
    }
    if (size - at < 2) return -ENOSPC;
    buf[at++] = '\n';
    buf[at]   = '\0';
    return (int)at;
}
