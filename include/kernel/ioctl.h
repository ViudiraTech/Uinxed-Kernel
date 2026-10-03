/*
 *
 *      ioctl.h
 *      ioctl request-number encoding
 *
 *      2026/9/12 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_IOCTL_H_
#define INCLUDE_IOCTL_H_

/* Layout of an ioctl request number: dir(2) | size(14) | type(8) | nr(8). */
#define _IOC_NRBITS   8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS  2

#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U

#define _IOC_NRMASK   ((1U << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK ((1U << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK ((1U << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK  ((1U << _IOC_DIRBITS) - 1)

#define _IOC(dir, type, nr, size) \
    (((unsigned long)(dir) << _IOC_DIRSHIFT) | ((unsigned long)(type) << _IOC_TYPESHIFT) | ((unsigned long)(nr) << _IOC_NRSHIFT) | ((unsigned long)(size) << _IOC_SIZESHIFT))

#define _IO(type, nr)       _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, sz)  _IOC(_IOC_READ, (type), (nr), sizeof(sz))
#define _IOW(type, nr, sz)  _IOC(_IOC_WRITE, (type), (nr), sizeof(sz))
#define _IOWR(type, nr, sz) _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(sz))

#define _IOC_DIR(cmd)  (((cmd) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(cmd) (((cmd) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(cmd)   (((cmd) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(cmd) (((cmd) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

#endif // INCLUDE_IOCTL_H_
