/*
 *
 *      ioctls.h
 *      Generic file/descriptor ioctl commands
 *
 *      2026/10/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_IOCTLS_H_
#define INCLUDE_IOCTLS_H_

#include <syscall/ioctl.h>

/* Generic file/descriptor ioctls. */
#define FIONREAD 0x541B
#define FIONBIO  0x5421
#define FIONCLEX 0x5450
#define FIOCLEX  0x5451
#define FIOASYNC 0x5452
#define FIOQSIZE 0x5460

#endif // INCLUDE_IOCTLS_H_
