/*
 *
 *      writer.h
 *      Writer interface header file
 *
 *      2026/5/17 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_WRITER_H_
#define INCLUDE_WRITER_H_

#include <libs/std/stdint.h>

struct writer;

/**
 * A handle of writing a char
 * `uint8_t` is a bool, if != 0 means write success, if == 0 means write failure
 */
typedef uint8_t (*write_handler)(struct writer *writer, char ch);

/* A interface of writing a char (May be extended in the future) */
typedef struct writer {
        void         *data; // Any data
        write_handler handler;
} writer;

#endif // INCLUDE_WRITER_H_
