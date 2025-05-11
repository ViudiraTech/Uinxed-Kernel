/*
 *
 *      printk.c
 *      Kernel string printing
 *
 *      2024/6/27 By Rainy101112
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "printk.h"
#include "acpi.h"
#include "stdarg.h"
#include "stddef.h"
#include "stdint.h"
#include "stdlib.h"
#include "string.h"
#include "tty.h"

#define BUF_SIZE 2048 // least 2 bytes (1 byte is for '\0')

/* Kernel print string */
void printk(const char *format, ...)
{
    static char buff[BUF_SIZE];
    va_list args;
    OverflowSignal *sig = 0;

    va_start(args, format);
    while (1) {
        sig = vsprintf_s(sig, buff, BUF_SIZE, &format, args);
        tty_print_str(buff);
        if (sig == NULL) break;
    }
    va_end(args);
}

/* Kernel print string without overflow check */
void printk_unsafe(const char *format, ...)
{
    static char buff[2048];
    va_list args;

    va_start(args, format);
    vsprintf(buff, format, args); // NOLINT
    tty_print_str(buff);
    va_end(args);
}

/* Kernel print log */
void plogk(const char *format, ...)
{
    printk("[");
    printk("%5d.%06d", nano_time() / 1000000000, (nano_time() / 1000) % 1000000);
    printk("] ");

    static char buff[BUF_SIZE];
    va_list args;
    OverflowSignal *sig = NULL;

    va_start(args, format);
    while (1) {
        sig = vsprintf_s(sig, buff, BUF_SIZE, &format, args);
        tty_print_str(buff);
        if (sig == NULL) { break; }
    }
    va_end(args);
}

/* Store the formatted output in a character array */
int sprintf(char *str, const char *fmt, ...)
{
    int c = 0;
    va_list arg;
    va_start(arg, fmt);
    c = vsprintf(str, fmt, arg);
    va_end(arg);
    return c;
}

/* Format a string and output it to a character array */
/* (It's deprecated and unsafe) */
int vsprintf(char *buff, const char *format, va_list args)
{
    int64_t len, precision, field_width;
    int i, flags;
    char *str, *s;

    for (str = buff; *format; ++format) {
        if (*format != '%') {
            *str++ = *format;
            continue;
        }
        flags = 0;
repeat:
        ++format;
        switch (*format) {
            case '-' :
                flags |= LEFT;
                goto repeat;
            case '+' :
                flags |= PLUS;
                goto repeat;
            case ' ' :
                flags |= SPACE;
                goto repeat;
            case '#' :
                flags |= SPECIAL;
                goto repeat;
            case '0' :
                flags |= ZEROPAD;
                goto repeat;
            default :
                break;
        }
        field_width = -1;
        if (is_digit(*format)) {
            field_width = skip_atoi(&format);
        } else if (*format == '*') {
            field_width = va_arg(args, int);
            if (field_width < 0) {
                field_width = -field_width;
                flags |= LEFT;
            }
        }
        precision = -1;
        if (*format == '.') {
            ++format;
            if (is_digit(*format)) {
                precision = skip_atoi(&format);
            } else if (*format == '*') {
                precision = va_arg(args, int);
            }
            if (precision < 0) precision = 0;
        }
        if (*format == 'h' || *format == 'l' || *format == 'L') ++format;
        switch (*format) {
            case 'c' :
                if (!(flags & LEFT)) {
                    while (--field_width > 0) *str++ = ' ';
                }
                *str++ = (char)va_arg(args, int);
                while (--field_width > 0) *str++ = ' ';
                break;
            case 's' :
                s   = va_arg(args, char *);
                len = (int64_t)strlen(s);
                if (!(flags & LEFT)) {
                    while (len < field_width--) *str++ = ' ';
                }
                for (i = 0; i < len; ++i) *str++ = *s++;
                while (len < field_width--) *str++ = ' ';
                break;
            case 'o' :
                str = number(str, va_arg(args, size_t), 8, field_width, precision, flags);
                break;
            case 'p' :
                if (field_width == -1) {
                    field_width = 16;
                    flags |= ZEROPAD;
                }
                str = number(str, (size_t)va_arg(args, void *), 16, field_width, precision, flags);
                break;
            case 'x' :
                flags |= SMALL; // fallthrough
            case 'X' :
                str = number(str, va_arg(args, size_t), 16, field_width, precision, flags);
                break;
            case 'd' :
            case 'i' :
                flags |= SIGN; // fallthrough
            case 'u' :
                str = number(str, va_arg(args, size_t), 10, field_width, precision, flags);
                break;
            case 'b' :
                str = number(str, va_arg(args, size_t), 2, field_width, precision, flags);
                break;
            case 'n' :
                va_arg(args, int *);
                break;
            default :
                if (*format != '%') *str++ = '%';
                if (*format) {
                    *str++ = *format;
                } else {
                    --format;
                }
                break;
        }
    }
    *str = '\0';
    return (int)(str - buff);
}

/* Release the memory used by the FmtArg structure */
void free_fmtarg(FmtArg *arg)
{
    free(arg->buff);
    free(arg);
}

/* Create a new FmtArg structure and initialize it */
FmtArg *new_fmtarg(uint64_t size, char *buff, char *last_write) // NOLINT
{
    FmtArg *arg     = (FmtArg *)malloc(sizeof(FmtArg));
    arg->size       = size;
    arg->buff       = buff;
    arg->last_write = last_write;
    return arg;
}

/* Parse the format string and read the corresponding variadic parameters to generate an FmtArg structure */
FmtArg *read_fmtarg(const char **format, va_list args)
{
    FmtArg *arg         = malloc(sizeof(FmtArg));
    const char *fmt_ptr = *format;
    char *buf_ptr       = 0;
    int flags           = 0;
    size_t field_width  = 0; // Minimum width field
    size_t precision    = 0; // For float, precision field (or number of digits and with zero padding)
    char *str           = 0;
    int tmp             = 0;
    size_t str_len      = 0;
    size_t num          = 0;
    size_t buf_len      = 0;
    size_t base         = 0;
    int64_t size_cnt    = 2; // hh = 0, h = 1, (nothing) = 2, l = 3, ll = 4, z = 5
    if (*fmt_ptr != '%') {
        free(arg);
        return NULL;
    }

    /* Get size */
    while (1) {
        ++fmt_ptr;
        switch (*fmt_ptr) {
            case '-' :
                flags |= LEFT;
                continue;
            case '+' :
                flags |= PLUS;
                continue;
            case ' ' :
                flags |= SPACE;
                continue;
            case '#' :
                flags |= SPECIAL;
                continue;
            case '0' :
                flags |= ZEROPAD;
                continue;
            default :
                break;
        }

        /* Minimum width field */
        if (is_digit(*fmt_ptr)) {
            field_width = skip_atoi(&fmt_ptr);
        } else if (*fmt_ptr == '*') {
            /* by the following argument */
            fmt_ptr++;
            tmp = va_arg(args, int);
            if (tmp < 0) {
                field_width = 0;
            } else {
                field_width = tmp;
            }
        }

        /* For float, precision field */
        if (*fmt_ptr == '.') {
            ++fmt_ptr; // skip the dot
            if (is_digit(*fmt_ptr)) {
                precision = skip_atoi(&fmt_ptr);
            } else if (*fmt_ptr == '*') {
                fmt_ptr++;
                tmp = va_arg(args, int);
                if (tmp < 0) {
                    precision = 0;
                } else {
                    precision = tmp;
                }
            }
        }

        /* Size */
        switch (*fmt_ptr) {
            case 'h' :
                size_cnt--;
                if (size_cnt < 0) { size_cnt = 0; }
                continue;
            case 'L' :      // += 2
                size_cnt++; // fallthrough
            case 'l' :
                size_cnt++;
                if (size_cnt > 4) { size_cnt = 4; }
                continue;
            case 'z' :
                size_cnt = 5;
                continue;
            default :
                break;
        }

        /* Pre-processing (inc: data) */
        switch (*fmt_ptr) {
            case 'c' :
                num = va_arg(args, int);
                break;
            case 's' :
                str = va_arg(args, char *);
                break;
            case 'd' :
            case 'i' :
                switch (size_cnt) {
                    case 0 :
                        num = (size_t)(char)va_arg(args, int);
                        break;
                    case 1 :
                        num = (size_t)(short)va_arg(args, int);
                        break;
                    case 2 :
                        num = (size_t)(int)va_arg(args, int);
                        break;
                    case 3 :
                        num = (size_t)(long)va_arg(args, long);
                        break;
                    case 4 :
                        num = (size_t)(long long)va_arg(args, long long);
                        break;
                    default : // Invalid but still parse, or size_cnt = 5
                        num = va_arg(args, size_t);
                        break;
                }
                break;
            case 'o' :
            case 'x' :
            case 'X' :
            case 'b' :
            case 'u' :
                switch (size_cnt) {
                    case 0 :
                        num = (size_t)(unsigned char)va_arg(args, unsigned int);
                        break;
                    case 1 :
                        num = (size_t)(unsigned short)va_arg(args, unsigned int);
                        break;
                    case 2 :
                        num = (size_t)(unsigned int)va_arg(args, unsigned int);
                        break;
                    case 3 :
                        num = (size_t)(unsigned long)va_arg(args, unsigned long);
                        break;
                    case 4 :
                        num = (size_t)(unsigned long long)va_arg(args, unsigned long long);
                        break;
                    default : // Invalid but still parse, or size_cnt = 5
                        num = va_arg(args, size_t);
                        break;
                }
                break;
            case 'p' :
                num = (size_t)va_arg(args, void *);
                break;
            default :
                break;
        }

        /* Pre-processing (inc: size, flags of arg) */
        switch (*fmt_ptr) {
            case 'c' :
                buf_len = 1;
                if (field_width > buf_len) { buf_len = field_width; }
                break;
            case 's' :
                buf_len = str_len = strlen(*&str);
                if (field_width > buf_len) { buf_len = field_width; }
                break;
            case 'o' :
                base    = 8;
                buf_len = number_length(num, base, field_width, precision, flags);
                break;
            case 'p' :
                flags |= SMALL | SPECIAL | ZEROPAD;
                if (field_width < 16) { field_width = 16; }
                base    = 16;
                buf_len = number_length(num, base, field_width, precision, flags);
                break;
            case 'x' :
                flags |= SMALL; // fallthrough
            case 'X' :
                base    = 16;
                buf_len = number_length(num, base, field_width, precision, flags);
                break;
            case 'd' :
            case 'i' :
                flags |= SIGN; // fallthrough
            case 'u' :
                base    = 10;
                buf_len = number_length(num, base, field_width, precision, flags);
                break;
            case 'b' :
                base    = 2;
                buf_len = number_length(num, base, field_width, precision, flags);
                break;
            case 'n' :
                va_arg(args, void *);
                break;
            case '%' :
                buf_len = 1;
                break;
            default :
                --fmt_ptr; // Undo
                free(arg);
                return 0;
        }
        if (buf_len < 1) { buf_len = 1; }
        if (field_width < buf_len) field_width = buf_len;

        /* Write buffer */
        arg->buff          = malloc(buf_len + 1);
        arg->buff[buf_len] = '\0';
        buf_ptr            = arg->buff;
        switch (*fmt_ptr) {
            case 'c' :
                if (!(flags & LEFT)) {
                    while (buf_ptr < arg->buff + field_width - 1) {
                        *buf_ptr = ' ';
                        buf_ptr++;
                    }
                }
                *buf_ptr = (char)num;
                if (flags & LEFT) {
                    while (buf_ptr < arg->buff + field_width - 1) {
                        buf_ptr++;
                        *buf_ptr = ' ';
                    }
                }
                break;
            case 's' :
                if (!(flags & LEFT)) {
                    while (buf_ptr < arg->buff + field_width - str_len) {
                        *buf_ptr = ' ';
                        buf_ptr++;
                    }
                    str_len = field_width;
                }
                while (buf_ptr < arg->buff + str_len) {
                    *buf_ptr = *str;
                    buf_ptr++;
                    str++;
                }
                if (flags & LEFT) {
                    while (buf_ptr < arg->buff + buf_len) {
                        *buf_ptr = ' ';
                        buf_ptr++;
                    }
                }
                break;
            case 'o' :
            case 'p' :
            case 'x' :
            case 'X' :
            case 'd' :
            case 'i' :
            case 'u' :
            case 'b' :
                number(buf_ptr, (int)num, base, field_width, precision, flags);
                break;
            case '%' :
                *buf_ptr = '%';
                break;
            default :
                break;
        }
        arg->size       = buf_len;
        arg->last_write = arg->buff;
        break;
    }
    *format = fmt_ptr;
    return arg;
}

/* Create a new OverflowSignal structure */
OverflowSignal *new_overflow(enum OverflowKind kind, FmtArg *arg)
{
    OverflowSignal *signal = (OverflowSignal *)malloc(sizeof(OverflowSignal));
    signal->kind           = kind;
    signal->arg            = arg;
    return signal;
}

/* Format a string with size and output it to a character array */
OverflowSignal *vsprintf_s(OverflowSignal *signal, char *buff, intptr_t size, const char **format, va_list args)
{
    char *write_ptr;
    const char *fmt_ptr;
    FmtArg *fmt_arg;

    write_ptr = buff;
    if (signal != 0 && signal->kind == OFLOW_AT_FMTARG) {
        /* Write buffer of OverflowSignal to buffer */
        fmt_arg = signal->arg;
        while (fmt_arg->last_write < fmt_arg->size + fmt_arg->buff) {
            *write_ptr = *fmt_arg->last_write;
            write_ptr++;
            fmt_arg->last_write++;
            if (write_ptr >= buff + size - 1) {
                *write_ptr++ = '\0';
                return signal; // Overflow again
            }
        }

        /* Write finished */
        free_fmtarg(fmt_arg);
        free(signal);
        fmt_arg = 0;
        signal  = 0;
    }
    if (signal != 0 && signal->kind == OFLOW_AT_FMTSTR) {
        /* Clear it */
        free(signal);
        signal = 0;
    }
    fmt_ptr = *format;
    while (*fmt_ptr != '\0') {
        if (write_ptr >= buff + size - 1) {
            *write_ptr = '\0';
            *format    = fmt_ptr;                          // Move to overflow position
            signal     = new_overflow(OFLOW_AT_FMTSTR, 0); // New Signal (just for run again)
            return signal;                                 // Send Signal
        }
        if (*fmt_ptr != '%' && write_ptr < buff + size - 1) {
            *write_ptr = *fmt_ptr;
            write_ptr++;
            fmt_ptr++;
            continue;
        }
        fmt_arg = read_fmtarg(&fmt_ptr, args);
        fmt_ptr++;
        *format = fmt_ptr;
        if (fmt_arg != 0) {
            /* Debug */
            while (fmt_arg->last_write < fmt_arg->size + fmt_arg->buff) {
                *write_ptr = *fmt_arg->last_write;
                write_ptr++;
                fmt_arg->last_write++;
                if (write_ptr >= buff + size - 1) {
                    *write_ptr++ = '\0';
                    signal       = new_overflow(OFLOW_AT_FMTARG, fmt_arg); // New Signal
                    return signal;
                }
            }

            /* Write finished */
            free_fmtarg(fmt_arg);
            fmt_arg = 0;
        }
    }
    *write_ptr = '\0';
    return 0; // No overflow
}
