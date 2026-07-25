/*
 *
 *      termios.h
 *      POSIX termios structures, flags, and TTY/PTY ioctl definitions
 *
 *      2026/7/24 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *      Full Linux x86_64 compatible termios definitions.
 *      Covers all standard TTY ioctls and Unix98 PTY ioctls.
 *
 */

#ifndef INCLUDE_KERNEL_TERMIOS_H_
#define INCLUDE_KERNEL_TERMIOS_H_

#include <libs/std/stdint.h>

/* ===================================================================
 * NCCS — number of control characters in c_cc array
 * =================================================================== */

#define NCCS 19
#define NCC  8

/* ===================================================================
 * c_cc indices (control character positions in termios.c_cc)
 * =================================================================== */

#define VINTR    0  /* interrupt (^C) */
#define VQUIT    1  /* quit (^\) */
#define VERASE   2  /* erase (^H or ^?) */
#define VKILL    3  /* kill line (^U) */
#define VEOF     4  /* end-of-file (^D) */
#define VTIME    5  /* timer for raw mode (0.1s units) */
#define VMIN     6  /* minimum chars for raw mode read */
#define VSWTC    7  /* switch (obsolete) */
#define VSTART   8  /* start output (^Q) */
#define VSTOP    9  /* stop output (^S) */
#define VSUSP    10 /* suspend (^Z) */
#define VEOL     11 /* end-of-line char */
#define VREPRINT 12 /* reprint line (^R) */
#define VDISCARD 13 /* discard output (^O) */
#define VWERASE  14 /* word erase (^W) */
#define VLNEXT   15 /* literal next (^V) */
#define VEOL2    16 /* alternate end-of-line */

/* POSIX control character names (aliases) */
#define _VINTR  VINTR
#define _VQUIT  VQUIT
#define _VERASE VERASE
#define _VKILL  VKILL
#define _VEOF   VEOF
#define _VTIME  VTIME
#define _VMIN   VMIN
#define _VSTART VSTART
#define _VSTOP  VSTOP
#define _VSUSP  VSUSP

/* ===================================================================
 * c_iflag — input mode flags
 * =================================================================== */

#define IGNBRK  0x0001 /* Ignore break condition */
#define BRKINT  0x0002 /* Send SIGINT on break */
#define IGNPAR  0x0004 /* Ignore framing/parity errors */
#define PARMRK  0x0008 /* Mark parity errors */
#define INPCK   0x0010 /* Enable input parity check */
#define ISTRIP  0x0020 /* Strip 8th bit */
#define INLCR   0x0040 /* Map NL → CR on input */
#define IGNCR   0x0080 /* Ignore CR */
#define ICRNL   0x0100 /* Map CR → NL on input */
#define IUCLC   0x0200 /* Map uppercase → lowercase on input */
#define IXON    0x0400 /* Enable XON/XOFF flow control on output */
#define IXANY   0x0800 /* Any character restarts output */
#define IXOFF   0x1000 /* Enable XON/XOFF flow control on input */
#define IMAXBEL 0x2000 /* Ring bell when input queue is full */
#define IUTF8   0x4000 /* Input is UTF-8 */

/* ===================================================================
 * c_oflag — output mode flags
 * =================================================================== */

#define OPOST  0x0001 /* Enable output processing */
#define OLCUC  0x0002 /* Map lowercase → uppercase on output */
#define ONLCR  0x0004 /* Map NL → CR-NL on output */
#define OCRNL  0x0008 /* Map CR → NL on output */
#define ONOCR  0x0010 /* No CR output at column 0 */
#define ONLRET 0x0020 /* NL performs CR function */
#define OFILL  0x0040 /* Use fill characters for delay */
#define OFDEL  0x0080 /* Fill character is DEL (0x7F), else NUL */
#define NLDLY  0x0100 /* NL delay mask */
#define NL0    0x0000
#define NL1    0x0100
#define CRDLY  0x0600 /* CR delay mask */
#define CR0    0x0000
#define CR1    0x0200
#define CR2    0x0400
#define CR3    0x0600
#define TABDLY 0x1800 /* TAB delay mask */
#define TAB0   0x0000
#define TAB1   0x0800
#define TAB2   0x1000
#define TAB3   0x1800 /* Expand tabs to spaces */
#define BSDLY  0x2000 /* BS delay mask */
#define BS0    0x0000
#define BS1    0x2000
#define VTDLY  0x4000 /* VT delay mask */
#define VT0    0x0000
#define VT1    0x4000
#define FFDLY  0x8000 /* FF delay mask */
#define FF0    0x0000
#define FF1    0x8000
#define XTABS  TAB3 /* Alias for TAB3 (tab expansion) */

/* ===================================================================
 * c_cflag — control mode flags
 * =================================================================== */

#define CBAUD  0x0000100f /* Baud rate mask (legacy) */
#define B0     0x00000000 /* Hang up */
#define B50    0x00000001
#define B75    0x00000002
#define B110   0x00000003
#define B134   0x00000004
#define B150   0x00000005
#define B200   0x00000006
#define B300   0x00000007
#define B600   0x00000008
#define B1200  0x00000009
#define B1800  0x0000000a
#define B2400  0x0000000b
#define B4800  0x0000000c
#define B9600  0x0000000d
#define B19200 0x0000000e
#define B38400 0x0000000f
#define EXTA   B19200
#define EXTB   B38400

#define CSIZE  0x00000030 /* Character size mask */
#define CS5    0x00000000
#define CS6    0x00000010
#define CS7    0x00000020
#define CS8    0x00000030
#define CSTOPB 0x00000040 /* Two stop bits (else one) */
#define CREAD  0x00000080 /* Enable receiver */
#define PARENB 0x00000100 /* Enable parity generation/detection */
#define PARODD 0x00000200 /* Odd parity (else even) */
#define HUPCL  0x00000400 /* Hang up on last close */
#define CLOCAL 0x00000800 /* Ignore modem control lines */

/* Extended baud rates */
#define CBAUDEX    0x00001000
#define B57600     0x00001001
#define B115200    0x00001002
#define B230400    0x00001003
#define B460800    0x00001004
#define B500000    0x00001005
#define B576000    0x00001006
#define B921600    0x00001007
#define B1000000   0x00001008
#define B1152000   0x00001009
#define B1500000   0x0000100a
#define B2000000   0x0000100b
#define B2500000   0x0000100c
#define B3000000   0x0000100d
#define B3500000   0x0000100e
#define B4000000   0x0000100f
#define __MAX_BAUD B4000000

#define CIBAUD  0x100f0000 /* Input baud rate mask (separate) */
#define CMSPAR  0x40000000 /* Mark/space parity */
#define CRTSCTS 0x80000000 /* RTS/CTS (hardware) flow control */

/* ===================================================================
 * c_lflag — local mode flags
 * =================================================================== */

#define ISIG    0x00000001 /* Enable signal-generating chars */
#define ICANON  0x00000002 /* Canonical input (line editing) */
#define XCASE   0x00000004 /* Canonical case conversion (obsolete) */
#define ECHO    0x00000008 /* Echo input characters */
#define ECHOE   0x00000010 /* Echo ERASE as BS-SP-BS */
#define ECHOK   0x00000020 /* Echo KILL by erasing the line */
#define ECHONL  0x00000040 /* Echo NL even if !ECHO */
#define NOFLSH  0x00000080 /* Disable flushing buffers on signals */
#define TOSTOP  0x00000100 /* Send SIGTTOU for background writes */
#define ECHOCTL 0x00000200 /* Echo control chars as ^X */
#define ECHOPRT 0x00000400 /* Echo erase as chars are erased */
#define ECHOKE  0x00000800 /* Echo KILL by erasing each char */
#define FLUSHO  0x00001000 /* Output is being flushed */
#define PENDIN  0x00004000 /* Reprint pending input at next read */
#define IEXTEN  0x00008000 /* Enable implementation-defined input processing */
#define EXTPROC 0x00010000 /* External processing */

/* ===================================================================
 * termios structure (x86_64 ABI, compatible with glibc)
 * =================================================================== */

typedef uint32_t tcflag_t;
typedef uint8_t  cc_t;
typedef uint32_t speed_t;

struct termios {
        tcflag_t c_iflag;    /* input mode flags */
        tcflag_t c_oflag;    /* output mode flags */
        tcflag_t c_cflag;    /* control mode flags */
        tcflag_t c_lflag;    /* local mode flags */
        cc_t     c_line;     /* line discipline */
        cc_t     c_cc[NCCS]; /* control characters */
};

/* ===================================================================
 * termios2 — extended termios with separate input/output baud
 * =================================================================== */

struct termios2 {
        tcflag_t c_iflag;
        tcflag_t c_oflag;
        tcflag_t c_cflag;
        tcflag_t c_lflag;
        cc_t     c_line;
        cc_t     c_cc[NCCS];
        speed_t  c_ispeed; /* input baud rate */
        speed_t  c_ospeed; /* output baud rate */
};

/* ===================================================================
 * winsize — terminal window size
 * =================================================================== */

struct winsize {
        uint16_t ws_row;    /* rows, in characters */
        uint16_t ws_col;    /* columns, in characters */
        uint16_t ws_xpixel; /* horizontal size, pixels */
        uint16_t ws_ypixel; /* vertical size, pixels */
};

/* ===================================================================
 * _IOC macros — construct ioctl command numbers
 * =================================================================== */

#define _IOC_NRBITS   8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS  2

#define _IOC_NRMASK   ((1U << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK ((1U << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK ((1U << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK  ((1U << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U

#define _IOC(dir, type, nr, size)                                                                                                \
    (((unsigned long)(dir) << _IOC_DIRSHIFT) | ((unsigned long)(type) << _IOC_TYPESHIFT) | ((unsigned long)(nr) << _IOC_NRSHIFT) \
     | ((unsigned long)(size) << _IOC_SIZESHIFT))

#define _IO(type, nr)       _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, sz)  _IOC(_IOC_READ, (type), (nr), sizeof(sz))
#define _IOW(type, nr, sz)  _IOC(_IOC_WRITE, (type), (nr), sizeof(sz))
#define _IOWR(type, nr, sz) _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(sz))

/* Decode an ioctl command number */
#define _IOC_DIR(cmd)  (((cmd) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(cmd) (((cmd) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(cmd)   (((cmd) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(cmd) (((cmd) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

/* ===================================================================
 * TTY ioctl magic number ('T' = 0x54)
 * =================================================================== */

#define TTY_IOCTL_MAGIC 0x54

/* ===================================================================
 * Standard TTY ioctls
 * =================================================================== */

/* termios get/set */
#define TCGETS  0x5401
#define TCSETS  0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404

/* Legacy termio (compatibility) */
#define TCGETA  0x5405
#define TCSETA  0x5406
#define TCSETAW 0x5407
#define TCSETAF 0x5408

/* Line control */
#define TCSBRK 0x5409
#define TCXONC 0x540A
#define TCFLSH 0x540B

/* Exclusive mode */
#define TIOCEXCL 0x540C
#define TIOCNXCL 0x540D

/* Controlling terminal */
#define TIOCSCTTY 0x540E

/* Process group */
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410

/* Queue status */
#define TIOCOUTQ 0x5411
#define TIOCSTI  0x5412

/* Window size */
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

/* Modem control lines (stubs for PTY, meaningful for serial) */
#define TIOCMGET 0x5415
#define TIOCMBIS 0x5416
#define TIOCMBIC 0x5417
#define TIOCMSET 0x5418

/* Soft carrier detect */
#define TIOCGSOFTCAR 0x5419
#define TIOCSSOFTCAR 0x541A

/* Bytes available to read (TIOCINQ = synonym for FIONREAD) */
#define FIONREAD 0x541B
#define TIOCINQ  FIONREAD

/* Linux-specific */
#define TIOCLINUX 0x541C
#define TIOCCONS  0x541D

/* Serial port info (stubbed) */
#define TIOCGSERIAL 0x541E
#define TIOCSSERIAL 0x541F

/* Packet mode (PTY) */
#define TIOCPKT 0x5420

/* Non-blocking I/O */
#define FIONBIO 0x5421

/* Detach controlling terminal */
#define TIOCNOTTY 0x5422

/* Line discipline */
#define TIOCSETD 0x5423
#define TIOCGETD 0x5424

/* POSIX break */
#define TCSBRKP 0x5425

/* BSD-style break */
#define TIOCTTYGSTRUCT 0x5426
#define TIOCSBRK       0x5427
#define TIOCCBRK       0x5428

/* Session ID */
#define TIOCGSID 0x5429

/* RS-485 mode (stubbed) */
#define TIOCGRS485 0x542E
#define TIOCSRS485 0x542F

/* termios2 (arbitrary baud rates) */
#define TCGETS2  _IOR(TTY_IOCTL_MAGIC, 0x2A, struct termios2)
#define TCSETS2  _IOW(TTY_IOCTL_MAGIC, 0x2B, struct termios2)
#define TCSETSW2 _IOW(TTY_IOCTL_MAGIC, 0x2C, struct termios2)
#define TCSETSF2 _IOW(TTY_IOCTL_MAGIC, 0x2D, struct termios2)

/* ===================================================================
 * Unix98 PTY-specific ioctls
 * =================================================================== */

#define TIOCGPTN    _IOR(TTY_IOCTL_MAGIC, 48, unsigned int) /* Get PTY number */
#define TIOCSPTLCK  _IOW(TTY_IOCTL_MAGIC, 49, int)          /* Lock/unlock PTY */
#define TIOCGDEV    _IOR(TTY_IOCTL_MAGIC, 50, unsigned int) /* Get PTY slave device number */
#define TCGETX      0x5432
#define TCSETX      0x5433
#define TCSETXF     0x5434
#define TCSETXW     0x5435
#define TIOCSIG     _IOW(TTY_IOCTL_MAGIC, 0x36, int) /* Send signal to slave */
#define TIOCVHANGUP 0x5437                           /* Virtual hangup */
#define TIOCGPKT    _IOR(TTY_IOCTL_MAGIC, 0x38, int) /* Get packet mode */
#define TIOCGPTLCK  _IOR(TTY_IOCTL_MAGIC, 0x39, int) /* Get PTY lock status */
#define TIOCGEXCL   _IOR(TTY_IOCTL_MAGIC, 0x40, int) /* Get exclusive mode */
#define TIOCGPTPEER _IO(TTY_IOCTL_MAGIC, 0x41)

/* ===================================================================
 * Modem control line bitmask (for TIOCMGET, TIOCMSET, etc.)
 * =================================================================== */

#define TIOCM_LE  0x0001 /* Line enable */
#define TIOCM_DTR 0x0002 /* Data terminal ready */
#define TIOCM_RTS 0x0004 /* Request to send */
#define TIOCM_ST  0x0008 /* Secondary transmit */
#define TIOCM_SR  0x0010 /* Secondary receive */
#define TIOCM_CTS 0x0020 /* Clear to send */
#define TIOCM_CAR 0x0040 /* Carrier detect */
#define TIOCM_RNG 0x0080 /* Ring indicator */
#define TIOCM_DSR 0x0100 /* Data set ready */
#define TIOCM_CD  TIOCM_CAR
#define TIOCM_RI  TIOCM_RNG

/* ===================================================================
 * TIOCPKT packet mode control byte bits (PTY master reads)
 * =================================================================== */

#define TIOCPKT_DATA       0x00 /* Normal data follows */
#define TIOCPKT_FLUSHREAD  0x01 /* Slave read queue flushed */
#define TIOCPKT_FLUSHWRITE 0x02 /* Slave write queue flushed */
#define TIOCPKT_STOP       0x04 /* Slave output stopped */
#define TIOCPKT_START      0x08 /* Slave output started */
#define TIOCPKT_NOSTOP     0x10 /* software flow control disabled */
#define TIOCPKT_DOSTOP     0x20 /* software flow control enabled */
#define TIOCPKT_IOCTL      0x40 /* Slave called an ioctl */

/* ===================================================================
 * Argument values for TCSETS / termios actions
 * =================================================================== */

#define TCSANOW   0 /* Change attributes immediately */
#define TCSADRAIN 1 /* Change after all output has drained */
#define TCSAFLUSH 2 /* Change after drain, flush pending input */

/* ===================================================================
 * Argument values for TCXONC
 * =================================================================== */

#define TCOOFF 0 /* Suspend output */
#define TCOON  1 /* Resume output */
#define TCIOFF 2 /* Send STOP character */
#define TCION  3 /* Send START character */

/* ===================================================================
 * Argument values for TCFLSH
 * =================================================================== */

#define TCIFLUSH  0 /* Flush input queue */
#define TCOFLUSH  1 /* Flush output queue */
#define TCIOFLUSH 2 /* Flush both queues */

/* ===================================================================
 * Line discipline numbers
 * =================================================================== */

#define N_TTY          0  /* Terminal line discipline */
#define N_SLIP         1  /* Serial Line IP */
#define N_MOUSE        2  /* Mouse */
#define N_PPP          3  /* PPP */
#define N_STRIP        4  /* Strip */
#define N_AX25         5  /* AX.25 */
#define N_X25          6  /* X.25 */
#define N_6PACK        7  /* 6pack */
#define N_MASC         8  /* Mobitex async */
#define N_R3964        9  /* Siemens R3964 */
#define N_PROFIBUS_FDL 10 /* Profibus */
#define N_IRDA         11 /* IrDA */
#define N_SMSBLOCK     12 /* SMS block mode */
#define N_HDLC         13 /* HDLC */
#define N_SYNC_PPP     14 /* Synchronous PPP */
#define N_HCI          15 /* Bluetooth HCI */

/* ===================================================================
 * Linux serial_struct (for TIOCGSERIAL/TIOCSSERIAL compatibility)
 * =================================================================== */

struct serial_struct {
        int            type;
        int            line;
        unsigned int   port;
        int            irq;
        int            flags;
        int            xmit_fifo_size;
        int            custom_divisor;
        int            baud_base;
        unsigned short close_delay;
        char           io_type;
        char           reserved_char[1];
        int            hub6;
        unsigned short closing_wait;
        unsigned short closing_wait2;
        unsigned char *iomem_base;
        unsigned short iomem_reg_shift;
        unsigned int   port_high;
        unsigned long  iomap_base;
};

_Static_assert(TCGETS == 0x5401 && TCSETS == 0x5402 && TCSETSW == 0x5403 && TCSETSF == 0x5404, "Linux termios ioctl ABI");
_Static_assert(TCGETA == 0x5405 && TCSETA == 0x5406 && TCSETAW == 0x5407 && TCSETAF == 0x5408, "Linux termio ioctl ABI");
_Static_assert(TCSBRK == 0x5409 && TCXONC == 0x540a && TCFLSH == 0x540b, "Linux line-control ioctl ABI");
_Static_assert(TIOCEXCL == 0x540c && TIOCNXCL == 0x540d && TIOCSCTTY == 0x540e, "Linux tty ownership ioctl ABI");
_Static_assert(TIOCGPGRP == 0x540f && TIOCSPGRP == 0x5410 && TIOCOUTQ == 0x5411 && TIOCSTI == 0x5412, "Linux tty process ioctl ABI");
_Static_assert(TIOCGWINSZ == 0x5413 && TIOCSWINSZ == 0x5414, "Linux winsize ioctl ABI");
_Static_assert(TIOCMGET == 0x5415 && TIOCMBIS == 0x5416 && TIOCMBIC == 0x5417 && TIOCMSET == 0x5418, "Linux modem ioctl ABI");
_Static_assert(TIOCGSOFTCAR == 0x5419 && TIOCSSOFTCAR == 0x541a && FIONREAD == 0x541b, "Linux tty queue ioctl ABI");
_Static_assert(TIOCLINUX == 0x541c && TIOCCONS == 0x541d && TIOCGSERIAL == 0x541e && TIOCSSERIAL == 0x541f, "Linux tty extension ioctl ABI");
_Static_assert(TIOCPKT == 0x5420 && FIONBIO == 0x5421 && TIOCNOTTY == 0x5422, "Linux tty mode ioctl ABI");
_Static_assert(TIOCSETD == 0x5423 && TIOCGETD == 0x5424 && TCSBRKP == 0x5425 && TIOCTTYGSTRUCT == 0x5426, "Linux line-discipline ioctl ABI");
_Static_assert(TIOCSBRK == 0x5427 && TIOCCBRK == 0x5428 && TIOCGSID == 0x5429, "Linux break/session ioctl ABI");
_Static_assert(TIOCGPTN == 0x80045430UL, "Linux TIOCGPTN ABI");
_Static_assert(TIOCSPTLCK == 0x40045431UL, "Linux TIOCSPTLCK ABI");
_Static_assert(TIOCGDEV == 0x80045432UL, "Linux TIOCGDEV ABI");
_Static_assert(TCGETS2 == 0x802c542aUL, "Linux TCGETS2 ABI");
_Static_assert(TCSETS2 == 0x402c542bUL, "Linux TCSETS2 ABI");
_Static_assert(TCSETSW2 == 0x402c542cUL, "Linux TCSETSW2 ABI");
_Static_assert(TCSETSF2 == 0x402c542dUL, "Linux TCSETSF2 ABI");
_Static_assert(TIOCGRS485 == 0x542e, "Linux TIOCGRS485 ABI");
_Static_assert(TIOCSRS485 == 0x542f, "Linux TIOCSRS485 ABI");
_Static_assert(TCGETX == 0x5432 && TCSETX == 0x5433 && TCSETXF == 0x5434 && TCSETXW == 0x5435, "Linux TCGETX/TCSETX ABI");
_Static_assert(TIOCSIG == 0x40045436UL, "Linux TIOCSIG ABI");
_Static_assert(TIOCVHANGUP == 0x5437, "Linux TIOCVHANGUP ABI");
_Static_assert(TIOCGPKT == 0x80045438UL, "Linux TIOCGPKT ABI");
_Static_assert(TIOCGPTLCK == 0x80045439UL, "Linux TIOCGPTLCK ABI");
_Static_assert(TIOCGEXCL == 0x80045440UL, "Linux TIOCGEXCL ABI");
_Static_assert(TIOCGPTPEER == 0x5441, "Linux TIOCGPTPEER ABI");
_Static_assert(sizeof(struct termios) == 36, "Linux x86_64 termios size");
_Static_assert(sizeof(struct termios2) == 44, "Linux x86_64 termios2 size");
_Static_assert(sizeof(struct winsize) == 8, "Linux winsize size");

#endif /* INCLUDE_KERNEL_TERMIOS_H_ */
