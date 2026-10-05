/*
 *
 *      exception_entry.h
 *      Canonical CPU exception frame shared by every exception entry
 *
 *      2026/9/13 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_EXCEPTION_ENTRY_H_
#define INCLUDE_EXCEPTION_ENTRY_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Registers and iret state pushed by every exception entry.  error_code holds the CPU-pushed code, or the zero an entry synthesises for vectors that push none. */
typedef struct exception_frame {
        uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
        uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
        uint64_t error_code;
        uint64_t rip, cs, rflags, rsp, ss;
} exception_frame_t;

/* One assertion per field: a reordered push sequence or struct must fail the build. */
_Static_assert(offsetof(exception_frame_t, r15) == 0 * sizeof(uint64_t), "exception frame: bad r15 offset");
_Static_assert(offsetof(exception_frame_t, r14) == 1 * sizeof(uint64_t), "exception frame: bad r14 offset");
_Static_assert(offsetof(exception_frame_t, r13) == 2 * sizeof(uint64_t), "exception frame: bad r13 offset");
_Static_assert(offsetof(exception_frame_t, r12) == 3 * sizeof(uint64_t), "exception frame: bad r12 offset");
_Static_assert(offsetof(exception_frame_t, r11) == 4 * sizeof(uint64_t), "exception frame: bad r11 offset");
_Static_assert(offsetof(exception_frame_t, r10) == 5 * sizeof(uint64_t), "exception frame: bad r10 offset");
_Static_assert(offsetof(exception_frame_t, r9) == 6 * sizeof(uint64_t), "exception frame: bad r9 offset");
_Static_assert(offsetof(exception_frame_t, r8) == 7 * sizeof(uint64_t), "exception frame: bad r8 offset");
_Static_assert(offsetof(exception_frame_t, rdi) == 8 * sizeof(uint64_t), "exception frame: bad rdi offset");
_Static_assert(offsetof(exception_frame_t, rsi) == 9 * sizeof(uint64_t), "exception frame: bad rsi offset");
_Static_assert(offsetof(exception_frame_t, rbp) == 10 * sizeof(uint64_t), "exception frame: bad rbp offset");
_Static_assert(offsetof(exception_frame_t, rdx) == 11 * sizeof(uint64_t), "exception frame: bad rdx offset");
_Static_assert(offsetof(exception_frame_t, rcx) == 12 * sizeof(uint64_t), "exception frame: bad rcx offset");
_Static_assert(offsetof(exception_frame_t, rbx) == 13 * sizeof(uint64_t), "exception frame: bad rbx offset");
_Static_assert(offsetof(exception_frame_t, rax) == 14 * sizeof(uint64_t), "exception frame: bad rax offset");
_Static_assert(offsetof(exception_frame_t, error_code) == 15 * sizeof(uint64_t), "exception frame: bad error_code offset");
_Static_assert(offsetof(exception_frame_t, rip) == 16 * sizeof(uint64_t), "exception frame: bad rip offset");
_Static_assert(offsetof(exception_frame_t, cs) == 17 * sizeof(uint64_t), "exception frame: bad cs offset");
_Static_assert(offsetof(exception_frame_t, rflags) == 18 * sizeof(uint64_t), "exception frame: bad rflags offset");
_Static_assert(offsetof(exception_frame_t, rsp) == 19 * sizeof(uint64_t), "exception frame: bad rsp offset");
_Static_assert(offsetof(exception_frame_t, ss) == 20 * sizeof(uint64_t), "exception frame: bad ss offset");
_Static_assert(sizeof(exception_frame_t) == 21 * sizeof(uint64_t), "exception frame: bad size");

/* Handlers reached from the exception entry stubs */
void                           fixed_exception_handle_frame(exception_frame_t *frame, uint32_t vector);
void                           page_fault_handle_frame(exception_frame_t *frame, uint32_t vector);
__attribute__((noreturn)) void exception_reserved_panic(exception_frame_t *frame, uint32_t vector);

/* Deliver a synchronous signal for a user-mode exception and redirect the frame into the handler.  `trap` marks a vector whose RIP has already advanced. */
void exception_deliver_signal(exception_frame_t *frame, int sig, int code, uintptr_t addr, const char *message, bool trap);

#endif // INCLUDE_EXCEPTION_ENTRY_H_
