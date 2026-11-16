/*
 *
 *      info.c
 *      ULSA /proc/asound procfs stub (implemented in feat/ulsa-procfs)
 *
 *      2026/11/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/audio/ulsa/internal.h>

#if CONFIG_AUDIO

/* Initialize global /proc/asound entries (no-op stub). */
int ulsa_info_init(void)
{
    return EOK;
}

/* Register per-card /proc/asound entries (no-op stub). */
int ulsa_info_card_register(ulsa_card_t *card)
{
    (void)card;
    return EOK;
}

/* Unregister per-card /proc/asound entries (no-op stub). */
void ulsa_info_card_free(ulsa_card_t *card)
{
    (void)card;
}

#endif
