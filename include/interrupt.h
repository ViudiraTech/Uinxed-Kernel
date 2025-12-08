/*
 *
 *      interrupt.h
 *      Interrupt related header files
 *
 *      2024/8/1 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_INTERRUPT_H_
#define INCLUDE_INTERRUPT_H_

#include <idt.h>
#include <stdint.h>

#if defined(__clang__)
#    define INTERRUPT_BEGIN                                                                          \
        _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wexcessive-regsave\"") \
            __attribute__((interrupt, target("general-regs-only")))
#    define INTERRUPT_END _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#    define INTERRUPT_BEGIN __attribute__((interrupt, target("general-regs-only")))
#    define INTERRUPT_END
#else
#    error "Unknown compiler"
#endif

/* Empty function handling */
extern void (*empty_handle[256])(interrupt_frame_t *frame);

/* Register ISR interrupt processing */
void isr_registe_handle(void);

#endif // INCLUDE_INTERRUPT_H_
