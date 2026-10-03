/*
 *
 *      kdev_t.h
 *      Device number layout with major/minor accessors
 *
 *      2026/9/13 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_KDEV_T_H_
#define INCLUDE_KDEV_T_H_

#include <libs/std/stdint.h>

/* Linux x86-64 device number: 12-bit major, 20-bit minor. */

#define MINORBITS 20
#define MINORMASK ((1U << MINORBITS) - 1)

#define MAJOR(dev)    ((uint32_t)((dev) >> MINORBITS))
#define MINOR(dev)    ((uint32_t)((dev) & MINORMASK))
#define MKDEV(ma, mi) ((((uint32_t)(ma) & 0xFFFU) << MINORBITS) | ((uint32_t)(mi) & MINORMASK))

typedef uint32_t dev_t;

_Static_assert(sizeof(dev_t) == 4, "Linux dev_t is 32-bit");

/* User-visible layout (glibc makedev), as stat and TIOCGDEV report it. */
uint32_t dev_encode_uapi(dev_t dev);

/* Convert a userspace (uapi) device number into the kernel layout. */
dev_t dev_decode_uapi(uint64_t dev);

#endif // INCLUDE_KDEV_T_H_
