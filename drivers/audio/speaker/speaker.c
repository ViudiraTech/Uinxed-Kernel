/*
 *
 *      speaker.c
 *      System speakers
 *
 *      2024/6/29 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/misc/common.h>

#if CONFIG_AUDIO_PCSPKR

/* Set the PC-speaker tone; divisor 0 silences it. */
void system_speaker(int divisor)
{
    if (!divisor) {
        outb(0x61, inb(0x61) & 0x0d); // Turn off the onboard buzzer
    } else {
        outb(0x43, 0xb6);                      // Send command to set timer 2
        outb(0x42, divisor & 0xff);            // Send the low byte of the frequency division
        outb(0x42, divisor >> 8);              // Send the high byte of the frequency division
        outb(0x61, (inb(0x61) | 0x03) & 0x0f); // Turn on the onboard buzzer
    }
}

#endif
