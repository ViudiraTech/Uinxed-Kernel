/*
 *
 *      vt_ansi.h
 *      Virtual Terminal ANSI
 *
 *      2026/7/27 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VT_ANSI_H_
#define INCLUDE_VT_ANSI_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define VT_ANSI_TABS 256
#define VT_ANSI_NPAR 32

enum vt_ansi_state {
    ANSI_normal,
    ANSI_esc,
    ANSI_csi,
    ANSI_csi_getpars,
    ANSI_csi_ignore,
    ANSI_osc,
    ANSI_osc_string,
    ANSI_charset_g0,
    ANSI_charset_g1,
    ANSI_sos_pm_apc,
    ANSI_esc_hash,
};

enum vt_ansi_priv {
    ANSI_priv_ecma,
    ANSI_priv_dec,
    ANSI_priv_gt,
};

enum vt_ansi_intensity {
    ANSI_INTENSITY_NORMAL,
    ANSI_INTENSITY_BOLD,
    ANSI_INTENSITY_HALF_BRIGHT,
};

typedef struct {
        uint8_t intensity;
        bool    italic;
        bool    underline;
        bool    blink;
        bool    reverse;
        bool    concealed;
        bool    crossed_out;
        bool    fraktur;
        bool    doubly_underlined;
        bool    overline;
        uint8_t charset;
        uint8_t g0_charset;
        uint8_t g1_charset;
} vt_ansi_attrs_t;

typedef struct {
        uint32_t fg;
        uint32_t bg;
} vt_ansi_color_t;

typedef struct {
        uint32_t        x, y;
        uint32_t        saved_x, saved_y;
        uint32_t        cols, rows;
        uint32_t        scroll_top, scroll_bottom;
        uint32_t        tab_stops[VT_ANSI_TABS];
        uint32_t        par[VT_ANSI_NPAR];
        uint32_t        npar;
        uint8_t         state;
        uint8_t         priv;
        vt_ansi_attrs_t attrs;
        vt_ansi_attrs_t saved_attrs;
        vt_ansi_color_t fg_color;
        vt_ansi_color_t bg_color;
        vt_ansi_color_t saved_fg_color;
        vt_ansi_color_t saved_bg_color;
        bool            wrap_next;
        bool            origin_mode;
        bool            auto_wrap;
        bool            cursor_visible;
        bool            insert_mode;
        bool            reverse_video;
        bool            bracketed_paste;
        uint32_t        utf_char;
        uint32_t        utf_remaining;
        uint32_t        default_fg;
        uint32_t        default_bg;
        bool            osc_esc;
        char            last_char;
} vt_ansi_state_t;

typedef struct {
        void (*draw_char)(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);
        void (*scroll_up)(uint32_t top, uint32_t bottom, uint32_t lines);
        void (*scroll_down)(uint32_t top, uint32_t bottom, uint32_t lines);
        void (*erase_display)(uint32_t mode, uint32_t x, uint32_t y);
        void (*erase_line)(uint32_t mode, uint32_t x, uint32_t y);
        void (*erase_chars)(uint32_t x, uint32_t y, uint32_t count);
        void (*insert_lines)(uint32_t y, uint32_t bottom, uint32_t n);
        void (*delete_lines)(uint32_t y, uint32_t bottom, uint32_t n);
        void (*insert_chars)(uint32_t x, uint32_t y, uint32_t n, uint32_t cols);
        void (*delete_chars)(uint32_t x, uint32_t y, uint32_t n, uint32_t cols);
        void (*cursor_visible)(bool visible);
        void (*bell)(void);
        void (*write_response)(const char *buf, size_t len);
} vt_ansi_callbacks_t;

void vt_ansi_init(vt_ansi_state_t *state, uint32_t cols, uint32_t rows);
void vt_ansi_process(vt_ansi_state_t *state, uint8_t c, const vt_ansi_callbacks_t *cb, void *arg);

static inline void vt_ansi_set_default_colors(vt_ansi_state_t *s, uint32_t fg, uint32_t bg)
{
    s->default_fg  = fg;
    s->default_bg  = bg;
    s->fg_color.fg = fg;
    s->fg_color.bg = fg;
    s->bg_color.fg = bg;
    s->bg_color.bg = bg;
}

#endif // INCLUDE_VT_ANSI_H_
