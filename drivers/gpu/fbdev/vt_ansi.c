/*
 *
 *      vt_ansi.c
 *      Virtual Terminal ANSI
 *
 *      2026/7/27 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/fbdev/vt_ansi.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>

static const uint32_t ansi_palette[16] = {
    0x000000, 0xaa0000, 0x00aa00, 0xaaaa00, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
    0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff,
};

static uint32_t vt_ansi_palette(uint8_t idx)
{
    if (idx < 16) return ansi_palette[idx];
    return 0;
}

static bool vt_ansi_is_reverse(const vt_ansi_state_t *s)
{
    return s->attrs.reverse ^ s->reverse_video;
}

static uint32_t vt_ansi_bg(const vt_ansi_state_t *s)
{
    uint32_t c = s->bg_color.fg;
    if (vt_ansi_is_reverse(s)) c = s->fg_color.fg;
    if (s->attrs.concealed) return c;
    return c;
}

static uint32_t vt_ansi_fg(const vt_ansi_state_t *s)
{
    uint32_t c = s->fg_color.fg;
    if (vt_ansi_is_reverse(s)) c = s->bg_color.fg;
    if (s->attrs.concealed) return vt_ansi_bg(s);
    return c;
}

static void vt_ansi_default_attr(vt_ansi_state_t *s)
{
    s->attrs.intensity         = ANSI_INTENSITY_NORMAL;
    s->attrs.italic            = false;
    s->attrs.underline         = false;
    s->attrs.blink             = false;
    s->attrs.reverse           = false;
    s->attrs.concealed         = false;
    s->attrs.crossed_out       = false;
    s->attrs.fraktur           = false;
    s->attrs.doubly_underlined = false;
    s->attrs.overline          = false;
    s->fg_color.fg             = s->fg_color.bg;
    s->bg_color.fg             = s->bg_color.bg;
}

static void vt_ansi_gotoxy(vt_ansi_state_t *s, int new_x, int new_y)
{
    uint32_t min_y = s->origin_mode ? s->scroll_top : 0;
    uint32_t max_y = s->origin_mode ? s->scroll_bottom : s->rows;

    if (new_x < 0)
        s->x = 0;
    else if ((uint32_t)new_x >= s->cols)
        s->x = s->cols - 1;
    else
        s->x = (uint32_t)new_x;

    if (new_y < (int)min_y)
        s->y = min_y;
    else if ((uint32_t)new_y >= max_y)
        s->y = max_y - 1;
    else
        s->y = (uint32_t)new_y;

    s->wrap_next = false;
}

static void vt_ansi_gotoxay(vt_ansi_state_t *s, int new_x, int new_y)
{
    vt_ansi_gotoxy(s, new_x, s->origin_mode ? (int)(s->scroll_top + new_y) : new_y);
}

static void vt_ansi_put_char(const vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb, char c)
{
    if (cb && cb->draw_char) {
        uint32_t fg = vt_ansi_fg(s);
        uint32_t bg = vt_ansi_bg(s);
        if (s->attrs.blink) {
            uint32_t tmp = fg;
            fg           = bg;
            bg           = tmp;
        }
        if (s->attrs.intensity == ANSI_INTENSITY_BOLD) {
            uint32_t r = (fg >> 16) & 0xFF, g = (fg >> 8) & 0xFF, b = fg & 0xFF;
            r  = r ? (r < 128 ? r * 2 : 255) : 0;
            g  = g ? (g < 128 ? g * 2 : 255) : 0;
            b  = b ? (b < 128 ? b * 2 : 255) : 0;
            fg = (r << 16) | (g << 8) | b;
        } else if (s->attrs.intensity == ANSI_INTENSITY_HALF_BRIGHT) {
            fg = ((fg & 0xFEFEFE) >> 1) | 0x555555;
        }
        cb->draw_char(c, s->x, s->y, fg, bg);
    }
}

static void vt_ansi_lf(vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb)
{
    s->x = 0;
    if (s->y + 1 == s->scroll_bottom) {
        if (cb && cb->scroll_up) cb->scroll_up(s->scroll_top, s->scroll_bottom, 1);
    } else if (s->y + 1 < s->rows) {
        s->y++;
    }
    s->wrap_next = false;
}

static void vt_ansi_ri(vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb)
{
    if (s->y == s->scroll_top) {
        if (cb && cb->scroll_down) cb->scroll_down(s->scroll_top, s->scroll_bottom, 1);
    } else if (s->y > 0) {
        s->y--;
    }
    s->wrap_next = false;
}

static void vt_ansi_cr(vt_ansi_state_t *s)
{
    s->x         = 0;
    s->wrap_next = false;
}

static void vt_ansi_tab(vt_ansi_state_t *s)
{
    uint32_t max_col = s->cols < VT_ANSI_TABS ? s->cols : VT_ANSI_TABS;
    uint32_t i       = s->x + 1;
    while (i < max_col && !s->tab_stops[i]) i++;
    s->x         = i >= s->cols ? s->cols - 1 : i;
    s->wrap_next = false;
}

static void vt_ansi_bs(vt_ansi_state_t *s)
{
    if (s->x > 0) {
        s->x--;
        s->wrap_next = false;
    }
}

static void vt_ansi_csi_j(vt_ansi_state_t *s, uint32_t mode, const vt_ansi_callbacks_t *cb)
{
    if (cb && cb->erase_display) cb->erase_display(mode, s->x, s->y);
    s->wrap_next = false;
}

static void vt_ansi_csi_k(vt_ansi_state_t *s, uint32_t mode, const vt_ansi_callbacks_t *cb)
{
    if (cb && cb->erase_line) cb->erase_line(mode, s->x, s->y);
    s->wrap_next = false;
}

static void vt_ansi_csi_x(vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb)
{
    uint32_t n     = s->par[0] ? s->par[0] : 1;
    uint32_t count = n < s->cols - s->x ? n : s->cols - s->x;
    if (cb && cb->erase_chars) cb->erase_chars(s->x, s->y, count);
    s->wrap_next = false;
}

static void vt_ansi_csi_at(vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb)
{
    uint32_t n = s->par[0] ? s->par[0] : 1;
    if (cb && cb->insert_chars) cb->insert_chars(s->x, s->y, n, s->cols);
    s->wrap_next = false;
}

static void vt_ansi_csi_l(vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb)
{
    uint32_t n     = s->par[0] ? s->par[0] : 1;
    uint32_t max_n = s->scroll_bottom - s->y;
    if (n > max_n) n = max_n;
    if (cb && cb->insert_lines) cb->insert_lines(s->y, s->scroll_bottom, n);
    s->wrap_next = false;
}

static void vt_ansi_csi_m(vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb)
{
    uint32_t n     = s->par[0] ? s->par[0] : 1;
    uint32_t max_n = s->scroll_bottom - s->y;
    if (n > max_n) n = max_n;
    if (cb && cb->delete_lines) cb->delete_lines(s->y, s->scroll_bottom, n);
    s->wrap_next = false;
}

static void vt_ansi_csi_i(vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb)
{
    (void)cb;
    uint32_t n = s->par[0] ? s->par[0] : 1;
    for (uint32_t k = 0; k < n; k++) vt_ansi_tab(s);
}

static void vt_ansi_csi_z(vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb)
{
    (void)cb;
    uint32_t n = s->par[0] ? s->par[0] : 1;
    for (uint32_t k = 0; k < n; k++) {
        int32_t i = (int32_t)s->x - 1;
        while (i >= 0 && !s->tab_stops[i]) i--;
        s->x = (uint32_t)(i >= 0 ? i : 0);
    }
    s->wrap_next = false;
}

static char *vt_ansi_utoa(uint32_t v, char *p)
{
    char tmp[12], *t = tmp;
    do {
        *t++ = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    do {
        *p++ = *--t;
    } while (t != tmp);
    return p;
}

static void vt_ansi_csi_b(vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb)
{
    if (!s->last_char) return;
    uint32_t n = s->par[0] ? s->par[0] : 1;
    char     c = s->last_char;
    for (uint32_t k = 0; k < n; k++) {
        if (s->wrap_next && s->auto_wrap) {
            vt_ansi_cr(s);
            vt_ansi_lf(s, cb);
        }
        vt_ansi_put_char(s, cb, c);
        if (s->x + 1 >= s->cols) {
            s->wrap_next = s->auto_wrap;
        } else {
            s->x++;
        }
    }
}

static void vt_ansi_csi_p(vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb)
{
    uint32_t n     = s->par[0] ? s->par[0] : 1;
    uint32_t count = n < s->cols - s->x ? n : s->cols - s->x;
    if (cb && cb->delete_chars) cb->delete_chars(s->x, s->y, count, s->cols);
    s->wrap_next = false;
}

typedef struct rgb {
        uint8_t r, g, b;
} rgb_t;

static void vt_ansi_rgb_from_256_color(uint8_t i, rgb_t *c)
{
    if (i < 16) {
        uint32_t p = vt_ansi_palette(i);
        c->r       = (p >> 16) & 0xFF;
        c->g       = (p >> 8) & 0xFF;
        c->b       = p & 0xFF;
        return;
    }
    if (i < 232) {
        i -= 16;
        c->b = (i % 6) * 255 / 6;
        i /= 6;
        c->g = (i % 6) * 255 / 6;
        i /= 6;
        c->r = i * 255 / 6;
    } else {
        c->r = c->g = c->b = i * 10 - 2312;
    }
}

static void vt_ansi_set_fg_rgb(vt_ansi_state_t *s, const rgb_t *c)
{
    s->fg_color.fg = ((uint32_t)c->r << 16) | ((uint32_t)c->g << 8) | c->b;
}

static void vt_ansi_set_bg_rgb(vt_ansi_state_t *s, const rgb_t *c)
{
    s->bg_color.fg = ((uint32_t)c->r << 16) | ((uint32_t)c->g << 8) | c->b;
}

static int vt_ansi_t416_color(vt_ansi_state_t *s, int i, int is_fg)
{
    rgb_t c;
    i++;
    if ((uint32_t)i > s->npar) return i;
    if (s->par[i] == 5 && (uint32_t)(i + 1) <= s->npar) {
        i++;
        vt_ansi_rgb_from_256_color(s->par[i] & 0xff, &c);
    } else if (s->par[i] == 2 && (uint32_t)(i + 3) <= s->npar) {
        c.r = s->par[i + 1] & 0xff;
        c.g = s->par[i + 2] & 0xff;
        c.b = s->par[i + 3] & 0xff;
        i += 3;
    } else {
        return i;
    }
    if (is_fg)
        vt_ansi_set_fg_rgb(s, &c);
    else
        vt_ansi_set_bg_rgb(s, &c);
    return i;
}

static void vt_ansi_sgr(vt_ansi_state_t *s, const vt_ansi_callbacks_t *cb)
{
    (void)cb;
    for (uint32_t i = 0; i <= s->npar; i++) {
        switch (s->par[i]) {
            case 0 :
                vt_ansi_default_attr(s);
                break;
            case 1 :
                s->attrs.intensity = ANSI_INTENSITY_BOLD;
                break;
            case 2 :
                s->attrs.intensity = ANSI_INTENSITY_HALF_BRIGHT;
                break;
            case 3 :
                s->attrs.italic = true;
                break;
            case 4 :
                s->attrs.underline         = true;
                s->attrs.doubly_underlined = false;
                break;
            case 5 :
            case 6 :
                s->attrs.blink = true;
                break;
            case 7 :
                s->attrs.reverse = true;
                break;
            case 8 :
                s->attrs.concealed = true;
                break;
            case 9 :
                s->attrs.crossed_out = true;
                break;
            case 10 :
                break;
            case 20 :
                s->attrs.fraktur = true;
                break;
            case 21 :
                s->attrs.doubly_underlined = true;
                s->attrs.underline         = true;
                break;
            case 22 :
                s->attrs.intensity = ANSI_INTENSITY_NORMAL;
                break;
            case 23 :
                s->attrs.italic  = false;
                s->attrs.fraktur = false;
                break;
            case 24 :
                s->attrs.underline         = false;
                s->attrs.doubly_underlined = false;
                break;
            case 25 :
                s->attrs.blink = false;
                break;
            case 27 :
                s->attrs.reverse = false;
                break;
            case 28 :
                s->attrs.concealed = false;
                break;
            case 29 :
                s->attrs.crossed_out = false;
                break;
            case 30 :
            case 31 :
            case 32 :
            case 33 :
            case 34 :
            case 35 :
            case 36 :
            case 37 :
                s->fg_color.fg = vt_ansi_palette(s->par[i] - 30);
                break;
            case 38 :
                i = (uint32_t)vt_ansi_t416_color(s, (int)i, 1);
                break;
            case 39 :
                s->fg_color.fg = s->fg_color.bg;
                break;
            case 40 :
            case 41 :
            case 42 :
            case 43 :
            case 44 :
            case 45 :
            case 46 :
            case 47 :
                s->bg_color.fg = vt_ansi_palette(s->par[i] - 40);
                break;
            case 48 :
                i = (uint32_t)vt_ansi_t416_color(s, (int)i, 0);
                break;
            case 49 :
                s->bg_color.fg = s->bg_color.bg;
                break;
            case 53 :
                s->attrs.overline = true;
                break;
            case 55 :
                s->attrs.overline = false;
                break;
            case 58 :
                break;
            case 90 :
            case 91 :
            case 92 :
            case 93 :
            case 94 :
            case 95 :
            case 96 :
            case 97 :
                s->fg_color.fg = vt_ansi_palette(s->par[i] - 90 + 8);
                break;
            case 100 :
            case 101 :
            case 102 :
            case 103 :
            case 104 :
            case 105 :
            case 106 :
            case 107 :
                s->bg_color.fg = vt_ansi_palette(s->par[i] - 100 + 8);
                break;
            default :
                break;
        }
    }
}

static void vt_ansi_hl(vt_ansi_state_t *s, bool set, const vt_ansi_callbacks_t *cb)
{
    (void)cb;
    for (uint32_t i = 0; i <= s->npar; i++) {
        switch (s->par[i]) {
            case 4 :
                s->insert_mode = set;
                break;
            case 20 :
            default :
                break;
        }
    }
}

static void vt_ansi_dec_hl(vt_ansi_state_t *s, bool set, const vt_ansi_callbacks_t *cb)
{
    for (uint32_t i = 0; i <= s->npar; i++) {
        switch (s->par[i]) {
            case 1 :
            case 3 :
                break;
            case 5 :
                s->reverse_video = set;
                if (cb && cb->erase_display) cb->erase_display(2, 0, 0);
                break;
            case 6 :
                s->origin_mode = set;
                vt_ansi_gotoxay(s, 0, 0);
                break;
            case 7 :
                s->auto_wrap = set;
                break;
            case 25 :
                s->cursor_visible = set;
                if (cb && cb->cursor_visible) cb->cursor_visible(set);
                break;
            case 1049 :
                if (!set) {
                    s->scroll_top    = 0;
                    s->scroll_bottom = s->rows;
                    s->origin_mode   = false;
                    s->wrap_next     = false;
                }
                break;
            case 2004 :
                s->bracketed_paste = set;
                break;
            default :
                break;
        }
    }
}

void vt_ansi_init(vt_ansi_state_t *s, uint32_t cols, uint32_t rows)
{
    uint32_t default_fg = s->default_fg ? s->default_fg : 0xaaaaaa;
    uint32_t default_bg = s->default_bg ? s->default_bg : 0x000000;

    s->x               = 0;
    s->y               = 0;
    s->saved_x         = 0;
    s->saved_y         = 0;
    s->cols            = cols;
    s->rows            = rows;
    s->scroll_top      = 0;
    s->scroll_bottom   = rows;
    s->npar            = 0;
    s->state           = ANSI_normal;
    s->priv            = ANSI_priv_ecma;
    s->wrap_next       = false;
    s->origin_mode     = false;
    s->auto_wrap       = true;
    s->cursor_visible  = true;
    s->insert_mode     = false;
    s->reverse_video   = false;
    s->bracketed_paste = false;
    s->osc_esc         = false;
    s->utf_char        = 0;
    s->utf_remaining   = 0;
    s->last_char       = 0;
    memset(s->tab_stops, 0, sizeof(s->tab_stops));
    for (uint32_t i = 0; i < VT_ANSI_TABS && i < cols; i += 8) s->tab_stops[i] = 1;

    s->default_fg = default_fg;
    s->default_bg = default_bg;

    s->attrs.intensity         = ANSI_INTENSITY_NORMAL;
    s->attrs.italic            = false;
    s->attrs.underline         = false;
    s->attrs.blink             = false;
    s->attrs.reverse           = false;
    s->attrs.concealed         = false;
    s->attrs.crossed_out       = false;
    s->attrs.fraktur           = false;
    s->attrs.doubly_underlined = false;
    s->attrs.overline          = false;
    s->attrs.charset           = 0;
    s->attrs.g0_charset        = 0;
    s->attrs.g1_charset        = 0;

    s->fg_color.fg = default_fg;
    s->fg_color.bg = default_fg;
    s->bg_color.fg = default_bg;
    s->bg_color.bg = default_bg;

    s->saved_fg_color = s->fg_color;
    s->saved_bg_color = s->bg_color;
    memset(&s->saved_attrs, 0, sizeof(s->saved_attrs));
}

void vt_ansi_process(vt_ansi_state_t *s, uint8_t c, const vt_ansi_callbacks_t *cb, void *arg)
{
    (void)arg;

    if (s->utf_remaining) {
        if ((c & 0xC0) == 0x80) {
            s->utf_char = (s->utf_char << 6) | (c & 0x3F);
            s->utf_remaining--;
            if (s->utf_remaining == 0) {
                if (s->state == ANSI_normal) {
                    if (s->utf_char > 127) s->utf_char = '?';
                    s->last_char = (char)s->utf_char;
                    vt_ansi_put_char(s, cb, (char)s->utf_char);
                    if (s->auto_wrap && s->x + 1 >= s->cols) {
                        s->wrap_next = true;
                    } else {
                        s->x++;
                    }
                }
            }
        } else {
            s->utf_remaining = 0;
            goto process_byte;
        }
        return;
    }

    if (c >= 0x80 && s->state == ANSI_normal) {
        if ((c & 0xE0) == 0xC0) {
            s->utf_remaining = 1;
            s->utf_char      = c & 0x1F;
            return;
        }
        if ((c & 0xF0) == 0xE0) {
            s->utf_remaining = 2;
            s->utf_char      = c & 0x0F;
            return;
        }
        if ((c & 0xF8) == 0xF0) {
            s->utf_remaining = 3;
            s->utf_char      = c & 0x07;
            return;
        }
        c = '?';
    }

process_byte:
    switch (s->state) {
        case ANSI_normal :
            switch (c) {
                case 0 :
                    return;
                case 7 :
                    if (cb && cb->bell) cb->bell();
                    return;
                case 8 :
                    vt_ansi_bs(s);
                    return;
                case 9 :
                    vt_ansi_tab(s);
                    return;
                case 10 :
                case 11 :
                case 12 :
                    vt_ansi_lf(s, cb);
                    return;
                case 13 :
                    vt_ansi_cr(s);
                    return;
                case 14 :
                    s->attrs.charset = 1;
                    return;
                case 15 :
                    s->attrs.charset = 0;
                    return;
                case 24 :
                case 26 :
                    s->state = ANSI_normal;
                    return;
                case 27 :
                    s->state = ANSI_esc;
                    return;
                case 127 :
                    return;
                default :
                    if (c < 32) return;
                    s->last_char = (char)c;
                    if (s->wrap_next && s->auto_wrap) {
                        vt_ansi_cr(s);
                        vt_ansi_lf(s, cb);
                    }
                    if (s->insert_mode)
                        if (cb && cb->insert_chars) cb->insert_chars(s->x, s->y, 1, s->cols);
                    vt_ansi_put_char(s, cb, (char)c);
                    if (s->x + 1 >= s->cols) {
                        s->wrap_next = s->auto_wrap;
                    } else {
                        s->x++;
                    }
                    return;
            }

        case ANSI_esc :
            s->state = ANSI_normal;
            switch (c) {
                case '[' :
                    s->state = ANSI_csi;
                    s->priv  = ANSI_priv_ecma;
                    s->npar  = 0;
                    memset(s->par, 0, sizeof(s->par));
                    return;
                case ']' :
                    s->state  = ANSI_osc;
                    s->npar   = 0;
                    s->par[0] = 0;
                    return;
                case '(' :
                    s->state = ANSI_charset_g0;
                    return;
                case ')' :
                    s->state = ANSI_charset_g1;
                    return;
                case '#' :
                    s->state = ANSI_esc_hash;
                    return;
                case '7' :
                    s->saved_x        = s->x;
                    s->saved_y        = s->y;
                    s->saved_attrs    = s->attrs;
                    s->saved_fg_color = s->fg_color;
                    s->saved_bg_color = s->bg_color;
                    return;
                case '8' :
                    s->x        = s->saved_x;
                    s->y        = s->saved_y;
                    s->attrs    = s->saved_attrs;
                    s->fg_color = s->saved_fg_color;
                    s->bg_color = s->saved_bg_color;
                    return;
                case 'D' :
                    vt_ansi_lf(s, cb);
                    return;
                case 'E' :
                    vt_ansi_cr(s);
                    vt_ansi_lf(s, cb);
                    return;
                case 'H' :
                    if (s->x < VT_ANSI_TABS) s->tab_stops[s->x] = 1;
                    return;
                case 'M' :
                    vt_ansi_ri(s, cb);
                    return;
                case 'Z' :
                    return;
                case 'c' :
                    s->state = ANSI_normal;
                    vt_ansi_init(s, s->cols, s->rows);
                    if (cb && cb->erase_display) cb->erase_display(2, 0, 0);
                    return;
                case '>' :
                case '=' :
                    return;
                case '_' :
                case '^' :
                case 'P' :
                    s->state = ANSI_sos_pm_apc;
                    return;
                default :
                    return;
            }

        case ANSI_charset_g0 :
        case ANSI_charset_g1 :
            s->state = ANSI_normal;
            return;

        case ANSI_esc_hash :
            s->state = ANSI_normal;
            if (c == '8') {
                if (cb && cb->erase_display) cb->erase_display(2, 0, 0);
                s->wrap_next = false;
            }
            return;

        case ANSI_csi :
            s->state = ANSI_csi_getpars;
            switch (c) {
                case '?' :
                    s->priv = ANSI_priv_dec;
                    return;
                case '>' :
                    s->priv = ANSI_priv_gt;
                    return;
                default :
                    break;
            }
            s->priv = ANSI_priv_ecma;
            goto csi_getpars;

        case ANSI_csi_getpars :
csi_getpars:
            switch (c) {
                case ';' :
                case ':' :
                    if (s->npar < VT_ANSI_NPAR - 1) {
                        s->npar++;
                        return;
                    }
                    return;
                case '0' ... '9' :
                    s->par[s->npar] = s->par[s->npar] * 10 + (c - '0');
                    return;
                default :
                    break;
            }
            if ((c >= ' ' && c <= '/') || c == ':' || (c >= '<' && c <= '?')) {
                s->state = ANSI_csi_ignore;
                return;
            }
            goto csi_exec;

        case ANSI_csi_ignore :
            if (c >= ' ' && c <= '?') return;
            s->state = ANSI_normal;
            return;

        case ANSI_osc :
            switch (c) {
                case 'P' :
                case 'R' :
                    s->state = ANSI_normal;
                    return;
                case '0' ... '9' :
                    s->par[0] = s->par[0] * 10 + (c - '0');
                    return;
                case ';' :
                    s->state = ANSI_osc_string;
                    return;
                case 7 :
                case 0x9C :
                    s->state = ANSI_normal;
                    return;
                case 0x1B :
                    s->osc_esc = true;
                    s->state   = ANSI_osc_string;
                    return;
                default :
                    s->state = ANSI_normal;
                    return;
            }

        case ANSI_osc_string :
            if (s->osc_esc) {
                s->osc_esc = false;
                if (c == '\\') {
                    s->state = ANSI_normal;
                    return;
                }
            }
            if (c == 7 || c == 0x9C) {
                s->state = ANSI_normal;
                return;
            }
            if (c == 0x1B) {
                s->osc_esc = true;
                return;
            }
            return;

        case ANSI_sos_pm_apc :
            if (c == 0x9C) {
                s->state = ANSI_normal;
                return;
            }
            return;

        default :
            s->state = ANSI_normal;
            return;
    }

    return;

csi_exec:
    s->state = ANSI_normal;

    if (s->priv == ANSI_priv_dec) {
        switch (c) {
            case 'h' :
                vt_ansi_dec_hl(s, true, cb);
                return;
            case 'l' :
                vt_ansi_dec_hl(s, false, cb);
                return;
            case 'n' :
                if (cb && cb->write_response) {
                    char buf[32], *p = buf;
                    *p++ = 0x1B;
                    *p++ = '[';
                    *p++ = '?';
                    p    = vt_ansi_utoa(s->y + 1, p);
                    *p++ = ';';
                    p    = vt_ansi_utoa(s->x + 1, p);
                    *p++ = 'R';
                    cb->write_response(buf, (size_t)(p - buf));
                }
                return;
            case 'c' :
                if (cb && cb->write_response) cb->write_response("\033[?1;2c", 7);
                return;
            case 'm' :
            default :
                return;
        }
    }

    if (s->priv == ANSI_priv_gt) {
        switch (c) {
            case 'c' :
                if (cb && cb->write_response) cb->write_response("\033[>0;0;0c", 10);
                return;
            default :
                return;
        }
    }

    switch (c) {
        case 'A' :
            if (!s->par[0]) s->par[0] = 1;
            vt_ansi_gotoxy(s, (int)s->x, (int)(s->y - s->par[0]));
            return;
        case 'B' :
        case 'e' :
            if (!s->par[0]) s->par[0] = 1;
            vt_ansi_gotoxy(s, (int)s->x, (int)(s->y + s->par[0]));
            return;
        case 'C' :
        case 'a' :
            if (!s->par[0]) s->par[0] = 1;
            vt_ansi_gotoxy(s, (int)(s->x + s->par[0]), (int)s->y);
            return;
        case 'D' :
            if (!s->par[0]) s->par[0] = 1;
            vt_ansi_gotoxy(s, (int)(s->x - s->par[0]), (int)s->y);
            return;
        case 'E' :
            if (!s->par[0]) s->par[0] = 1;
            vt_ansi_gotoxy(s, 0, (int)(s->y + s->par[0]));
            return;
        case 'F' :
            if (!s->par[0]) s->par[0] = 1;
            vt_ansi_gotoxy(s, 0, (int)(s->y - s->par[0]));
            return;
        case 'G' :
        case '`' :
            if (s->par[0]) s->par[0]--;
            vt_ansi_gotoxy(s, (int)s->par[0], (int)s->y);
            return;
        case 'd' :
            if (s->par[0]) s->par[0]--;
            vt_ansi_gotoxay(s, (int)s->x, (int)s->par[0]);
            return;
        case 'f' :
        case 'H' :
            if (s->par[0]) s->par[0]--;
            if (s->par[1]) s->par[1]--;
            vt_ansi_gotoxay(s, (int)s->par[1], (int)s->par[0]);
            return;
        case 'J' :
            vt_ansi_csi_j(s, s->par[0], cb);
            return;
        case 'K' :
            vt_ansi_csi_k(s, s->par[0], cb);
            return;
        case 'L' :
            vt_ansi_csi_l(s, cb);
            return;
        case 'M' :
            vt_ansi_csi_m(s, cb);
            return;
        case 'P' :
            vt_ansi_csi_p(s, cb);
            return;
        case 'X' :
            vt_ansi_csi_x(s, cb);
            return;
        case '@' :
            vt_ansi_csi_at(s, cb);
            return;
        case 'I' :
            vt_ansi_csi_i(s, cb);
            return;
        case 'Z' :
            vt_ansi_csi_z(s, cb);
            return;
        case 'b' :
            vt_ansi_csi_b(s, cb);
            return;
        case 'c' :
            if (cb && cb->write_response) cb->write_response("\033[?1;2c", 7);
            return;
        case 'g' :
            if (s->par[0] == 0 && s->x < VT_ANSI_TABS)
                s->tab_stops[s->x] = 0;
            else if (s->par[0] == 3)
                memset(s->tab_stops, 0, sizeof(s->tab_stops));
            return;
        case 'h' :
            vt_ansi_hl(s, true, cb);
            return;
        case 'l' :
            vt_ansi_hl(s, false, cb);
            return;
        case 'm' :
            vt_ansi_sgr(s, cb);
            return;
        case 'n' :
            if (cb && cb->write_response) {
                if (s->par[0] == 5) {
                    cb->write_response("\033[0n", 4);
                } else if (s->par[0] == 6) {
                    char buf[32], *p = buf;
                    *p++ = 0x1B;
                    *p++ = '[';
                    p    = vt_ansi_utoa(s->y + 1, p);
                    *p++ = ';';
                    p    = vt_ansi_utoa(s->x + 1, p);
                    *p++ = 'R';
                    cb->write_response(buf, (size_t)(p - buf));
                }
            }
            return;
        case 'r' :
            if (!s->par[0]) s->par[0] = 1;
            if (!s->par[1]) s->par[1] = s->rows;
            if (s->par[0] < s->par[1] && s->par[1] <= s->rows) {
                s->scroll_top    = s->par[0] - 1;
                s->scroll_bottom = s->par[1];
                vt_ansi_gotoxay(s, 0, 0);
            }
            return;
        case 's' :
            s->saved_x        = s->x;
            s->saved_y        = s->y;
            s->saved_attrs    = s->attrs;
            s->saved_fg_color = s->fg_color;
            s->saved_bg_color = s->bg_color;
            return;
        case 'u' :
            s->x        = s->saved_x;
            s->y        = s->saved_y;
            s->attrs    = s->saved_attrs;
            s->fg_color = s->saved_fg_color;
            s->bg_color = s->saved_bg_color;
            return;
        case 't' :
        case ']' :
        default :
            return;
    }
}
