/*
 *
 *      eflags.h
 *      x86-64 RFLAGS bits
 *
 *      2026/10/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_EFLAGS_H_
#define INCLUDE_EFLAGS_H_

/* RFLAGS bits */
#define X86_EFLAGS_CF    (1ULL << 0)
#define X86_EFLAGS_FIXED (1ULL << 1)
#define X86_EFLAGS_PF    (1ULL << 2)
#define X86_EFLAGS_AF    (1ULL << 4)
#define X86_EFLAGS_ZF    (1ULL << 6)
#define X86_EFLAGS_SF    (1ULL << 7)
#define X86_EFLAGS_TF    (1ULL << 8)
#define X86_EFLAGS_IF    (1ULL << 9)
#define X86_EFLAGS_DF    (1ULL << 10)
#define X86_EFLAGS_OF    (1ULL << 11)
#define X86_EFLAGS_IOPL  (3ULL << 12)
#define X86_EFLAGS_NT    (1ULL << 14)
#define X86_EFLAGS_RF    (1ULL << 16)
#define X86_EFLAGS_VM    (1ULL << 17)
#define X86_EFLAGS_AC    (1ULL << 18)
#define X86_EFLAGS_VIF   (1ULL << 19)
#define X86_EFLAGS_VIP   (1ULL << 20)
#define X86_EFLAGS_ID    (1ULL << 21)

#endif // INCLUDE_EFLAGS_H_
