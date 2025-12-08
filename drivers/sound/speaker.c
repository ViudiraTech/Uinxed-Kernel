/*
 *
 *      speaker.c
 *      System speakers
 *
 *      2024/6/29 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>

/* Set the system speaker status */
void system_speaker(int hertz)
{
    if (!hertz) {
        outb(0x61, inb(0x61) & 0x0d); // Turn off the onboard buzzer
    } else {
        outb(0x43, 0xb6);                      // Send command to set timer 2
        outb(0x42, hertz & 0xff);              // Send the low byte of the frequency division
        outb(0x42, hertz >> 8);                // Send the high byte of the frequency division
        outb(0x61, (inb(0x61) | 0x03) & 0x0f); // Turn on the onboard buzzer
    }
}
