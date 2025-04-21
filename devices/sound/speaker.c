/*
 *
 *      speaker.c
 *      System speakers
 *
 *      2024/6/29 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "common.h"

/* Set the system speaker status */
void system_speaker(int hertz)
{
    int i;
    if (hertz == 0) {
        i = inb(0x61);        // Read the current onboard buzzer status
        outb(0x61, i & 0x0d); // Turn off the onboard buzzer
    } else {
        i = hertz;
        outb(0x43, 0xb6);              // Send command to set timer 2
        outb(0x42, i & 0xff);          // Send the low byte of the frequency division value
        outb(0x42, i >> 8);            // Send the high byte of the frequency division value
        i = inb(0x61);                 // Read the current onboard buzzer status
        outb(0x61, (i | 0x03) & 0x0f); // Turn on the onboard buzzer
    }
}
