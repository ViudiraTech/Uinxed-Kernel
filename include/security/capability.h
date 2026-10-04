/*
 *
 *      capability.h
 *      Per-thread capability state and credential transitions
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CAPABILITY_H_
#define INCLUDE_CAPABILITY_H_

#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>

#define CAP_LAST_SUPPORTED 40
#define CAP_SETPCAP        8
#define CAP_NET_ADMIN      12
#define CAP_SYS_PTRACE     19
#define CAP_SYS_ADMIN      21
#define CAP_AUDIT_WRITE    29
#define CAP_SUPPORTED_MASK ((1ULL << (CAP_LAST_SUPPORTED + 1)) - 1)

struct task;

typedef struct {
        uint64_t effective, permitted, inheritable, bounding, ambient;
} capability_status_t;

/* Real, effective and saved set-user-ID of one credential state. */
typedef struct {
        uint32_t real, effective, saved;
} uid_set_t;

void    capability_get(struct task *task, uint64_t *effective, uint64_t *permitted, uint64_t *inheritable);
void    capability_get_status(struct task *task, capability_status_t *status);
int     capability_set(struct task *task, uint64_t effective, uint64_t permitted, uint64_t inheritable);
bool    capability_has(struct task *task, unsigned capability);
void    capability_inherit(struct task *child, struct task *parent);
void    capability_uid_change(struct task *task, const uid_set_t *old_ids, const uid_set_t *new_ids);
void    capability_exec(struct task *task);
int64_t capability_prctl(struct task *task, uint64_t option, uint64_t operation, uint64_t capability, uint64_t arg4, uint64_t arg5);

#endif // INCLUDE_CAPABILITY_H_
