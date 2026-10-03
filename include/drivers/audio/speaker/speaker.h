/*
 *
 *      speaker.h
 *      System speaker header file
 *
 *      2024/6/29 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SPEAKER_H_
#define INCLUDE_SPEAKER_H_

/* Set the PC-speaker tone: divisor 0 silences it, else tone ≈ 1.193182 MHz / divisor. */
#if CONFIG_AUDIO_PCSPKR
void system_speaker(int divisor);
#else
static inline void system_speaker(int) {}
#endif

#endif // INCLUDE_SPEAKER_H_
