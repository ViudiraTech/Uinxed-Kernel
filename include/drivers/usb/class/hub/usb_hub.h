/*
 *
 *      usb_hub.h
 *      USB Hub Class driver — Hub descriptor, port status and hub management
 *
 *      2026/8/27 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_USB_HUB_H_
#define INCLUDE_USB_HUB_H_

#include <drivers/usb/core/usb.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Hub class requests (USB 2.0 §11.24.2, bmRequestType = Class, recipient Device/Other). */
#define USB_HUB_REQ_GET_STATUS       0x00
#define USB_HUB_REQ_CLEAR_FEATURE    0x01
#define USB_HUB_REQ_SET_FEATURE      0x03
#define USB_HUB_REQ_GET_DESCRIPTOR   0x06
#define USB_HUB_REQ_SET_DESCRIPTOR   0x07
#define USB_HUB_REQ_CLEAR_TT_BUFFER  0x08
#define USB_HUB_REQ_RESET_TT         0x09
#define USB_HUB_REQ_GET_TT_STATE     0x0a
#define USB_HUB_REQ_STOP_TT          0x0b

/* Hub descriptor characteristics (wHubCharacteristics). */
#define USB_HUB_CHAR_GANGED_POWER    0x0000
#define USB_HUB_CHAR_INDIV_POWER     0x0001
#define USB_HUB_CHAR_NO_OCPM         0x0000
#define USB_HUB_CHAR_OVER_CURRENT    0x0008
#define USB_HUB_CHAR_TT_THINK_8      0x0000
#define USB_HUB_CHAR_TT_THINK_16     0x0020
#define USB_HUB_CHAR_TT_THINK_24     0x0040
#define USB_HUB_CHAR_TT_THINK_32     0x0060
#define USB_HUB_CHAR_PORT_INDICATOR  0x0080
#define USB_HUB_CHAR_COMPOUND        0x0004

/* Hub status change interrupt: 1 bit per port + hub, rounded to bytes. */
#define USB_HUB_MAX_PORTS      15
#define USB_HUB_MAX_STATUS_BYTES 2

/* Hub port power-good delay is in 2 ms units; add margin per §9.2.6.2. */
#define USB_HUB_POWER_GOOD_MARGIN_MS 20

/* Port debounce and reset recovery per USB 2.0 §7.1.7.3 / §9.2.6.2. */
#define USB_HUB_DEBOUNCE_MS          100
#define USB_HUB_RESET_RECOVERY_MS    10
#define USB_HUB_RESET_TIMEOUT_MS     500
#define USB_HUB_POWER_TIMEOUT_MS     500

/* Hub class driver entry points. */
int  usb_hub_probe(usb_interface_t *interface);
void usb_hub_disconnect(usb_interface_t *interface);

#endif // INCLUDE_USB_HUB_H_
