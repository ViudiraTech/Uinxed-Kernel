/*
 *
 *      fbcon.c
 *      Framebuffer console
 *
 *      2026/5/16 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <libs/gfxs/gfx_proc.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <video/fbcon.h>
#include <video/klogo.h>
#include <video/video.h>
#include <video/vt_ansi.h>

vt_ansi_state_t vt_ansi_state;

/* Bitmap fonts */
extern uint8_t ascii_font[];

static char     *text_grid       = 0;
static uint32_t *color_grid      = 0;
static uint32_t *bg_grid         = 0;
static uint32_t *dirty_first_col = 0;
static uint32_t *dirty_last_col  = 0;
static uint8_t   full_redraw_pending;
static uint8_t   redraw_deferred;

#if BOOT_LOGO
static uint32_t fbcon_offset_x      = 0;
static uint32_t fbcon_offset_y      = 98;
static uint32_t fbcon_draw_offset_y = 12;
#endif

static void fbcon_mark_cell_dirty(uint32_t row, uint32_t col)
{
    if (!dirty_first_col || !dirty_last_col || row >= c_height || col >= c_width) return;

    if (dirty_first_col[row] > col) dirty_first_col[row] = col;
    if (dirty_last_col[row] < col) dirty_last_col[row] = col;
}

static void fbcon_clear_row(uint32_t row)
{
    if (!text_grid || !color_grid || row >= c_height) return;

    memset(text_grid + (size_t)row * c_width, ' ', c_width);
    for (uint32_t col = 0; col < c_width; col++) {
        size_t idx      = (size_t)row * c_width + col;
        color_grid[idx] = fore_color;
        if (bg_grid) bg_grid[idx] = back_color;
    }
}

static void fbcon_redraw_row_range(uint32_t row, uint32_t first_col, uint32_t last_col)
{
    if (!text_grid || !color_grid || row >= c_height) return;
    if (first_col >= c_width || last_col >= c_width || first_col > last_col) return;

    for (uint32_t col = first_col; col <= last_col; col++) {
        size_t   index = (size_t)row * c_width + col;
        uint32_t bg    = bg_grid ? bg_grid[index] : back_color;
        fbcon_draw_char_bg(text_grid[index], col * font_width, row * font_height, color_grid[index], bg);
    }
}

static void fbcon_flush_dirty_rows(void)
{
    uint32_t damage_x1 = (uint32_t)width;
    uint32_t damage_y1 = (uint32_t)height;
    uint32_t damage_x2 = 0;
    uint32_t damage_y2 = 0;
    bool     damaged   = false;

    if (!dirty_first_col || !dirty_last_col) return;
    for (uint32_t row = 0; row < c_height; row++) {
        if (dirty_first_col[row] > dirty_last_col[row]) continue;

#if BOOT_LOGO
        uint32_t x1 = dirty_first_col[row] * font_width + fbcon_offset_x;
        uint32_t y1 = row * font_height + fbcon_offset_y + fbcon_draw_offset_y;
        uint32_t x2 = (dirty_last_col[row] + 1) * font_width + fbcon_offset_x;
#else
        uint32_t x1 = dirty_first_col[row] * font_width;
        uint32_t y1 = row * font_height;
        uint32_t x2 = (dirty_last_col[row] + 1) * font_width;
#endif
        uint32_t y2 = y1 + font_height;

        if (x1 < damage_x1) damage_x1 = x1;
        if (y1 < damage_y1) damage_y1 = y1;
        if (x2 > damage_x2) damage_x2 = x2;
        if (y2 > damage_y2) damage_y2 = y2;
        damaged = true;

        fbcon_redraw_row_range(row, dirty_first_col[row], dirty_last_col[row]);
        dirty_first_col[row] = c_width;
        dirty_last_col[row]  = 0;
    }

    if (damaged) video_flush_rect(damage_x1, damage_y1, damage_x2 - damage_x1, damage_y2 - damage_y1);
}

static void fbcon_redraw_screen(void)
{
    if (!text_grid || !color_grid) return;

    for (uint32_t row = 0; row < c_height; row++) {
        fbcon_redraw_row_range(row, 0, c_width ? c_width - 1 : 0);
        if (dirty_first_col && dirty_last_col) {
            dirty_first_col[row] = c_width;
            dirty_last_col[row]  = 0;
        }
    }
}

static void fbcon_clear_uncovered_bottom(void)
{
    if (!buffer) return;
#if BOOT_LOGO
    uint32_t used_height = fbcon_offset_y + fbcon_draw_offset_y + c_height * font_height;
#else
    uint32_t used_height = c_height * font_height;
#endif
    if (used_height < height) {
        for (uint32_t y = used_height; y < height; y++) {
            uint32_t *line  = buffer + (size_t)y * stride;
            size_t    count = stride;
#if defined(__x86_64__) || defined(__i386__)
            __asm__ volatile("rep stosl" : "+D"(line), "+c"(count) : "a"(back_color) : "memory");
#else
            for (uint32_t x = 0; x < stride; x++) line[x] = back_color;
#endif
        }
    }
}

void fbcon_scroll_up(uint32_t top, uint32_t bottom, uint32_t lines)
{
    uint32_t region = bottom - top;
    if (region == 0 || lines == 0) return;
    if (lines >= region) lines = region;
    uint32_t move_bytes = (region - lines) * c_width;
    size_t   src_off    = (size_t)(top + lines) * c_width;
    size_t   dst_off    = (size_t)top * c_width;

    memmove(text_grid + dst_off, text_grid + src_off, move_bytes);
    memmove(color_grid + dst_off, color_grid + src_off, move_bytes * sizeof(uint32_t));
    if (bg_grid) memmove(bg_grid + dst_off, bg_grid + src_off, move_bytes * sizeof(uint32_t));

    if (dirty_first_col && dirty_last_col) {
        memmove(dirty_first_col + top, dirty_first_col + top + lines, (region - lines) * sizeof(uint32_t));
        memmove(dirty_last_col + top, dirty_last_col + top + lines, (region - lines) * sizeof(uint32_t));
    }

    for (uint32_t r = bottom - lines; r < bottom; r++) {
        memset(text_grid + (size_t)r * c_width, ' ', c_width);
        for (uint32_t col = 0; col < c_width; col++) {
            size_t idx      = (size_t)r * c_width + col;
            color_grid[idx] = fore_color;
            if (bg_grid) bg_grid[idx] = back_color;
        }
        if (dirty_first_col) {
            dirty_first_col[r] = 0;
            dirty_last_col[r]  = c_width - 1;
        }
    }
    full_redraw_pending = 1;
}

void fbcon_scroll_down(uint32_t top, uint32_t bottom, uint32_t lines)
{
    uint32_t region = bottom - top;
    if (region == 0 || lines == 0) return;
    if (lines >= region) lines = region;
    uint32_t move_bytes = (region - lines) * c_width;
    size_t   src_off    = (size_t)top * c_width;
    size_t   dst_off    = (size_t)(top + lines) * c_width;

    memmove(text_grid + dst_off, text_grid + src_off, move_bytes);
    memmove(color_grid + dst_off, color_grid + src_off, move_bytes * sizeof(uint32_t));
    if (bg_grid) memmove(bg_grid + dst_off, bg_grid + src_off, move_bytes * sizeof(uint32_t));

    if (dirty_first_col && dirty_last_col) {
        memmove(dirty_first_col + top + lines, dirty_first_col + top, (region - lines) * sizeof(uint32_t));
        memmove(dirty_last_col + top + lines, dirty_last_col + top, (region - lines) * sizeof(uint32_t));
    }

    for (uint32_t r = top; r < top + lines; r++) {
        memset(text_grid + (size_t)r * c_width, ' ', c_width);
        for (uint32_t col = 0; col < c_width; col++) {
            size_t idx      = (size_t)r * c_width + col;
            color_grid[idx] = fore_color;
            if (bg_grid) bg_grid[idx] = back_color;
        }
        if (dirty_first_col) {
            dirty_first_col[r] = 0;
            dirty_last_col[r]  = c_width - 1;
        }
    }
    full_redraw_pending = 1;
}

void fbcon_erase_display(uint32_t mode)
{
    if (!text_grid || !color_grid) return;
    switch (mode) {
        case 0 :
            for (uint32_t col = cx; col < c_width; col++) {
                size_t idx      = (size_t)cy * c_width + col;
                text_grid[idx]  = ' ';
                color_grid[idx] = fore_color;
                if (bg_grid) bg_grid[idx] = back_color;
                fbcon_mark_cell_dirty(cy, col);
            }
            for (uint32_t r = cy + 1; r < c_height; r++) {
                memset(text_grid + (size_t)r * c_width, ' ', c_width);
                for (uint32_t col = 0; col < c_width; col++) {
                    size_t idx      = (size_t)r * c_width + col;
                    color_grid[idx] = fore_color;
                    if (bg_grid) bg_grid[idx] = back_color;
                    fbcon_mark_cell_dirty(r, col);
                }
            }
            break;
        case 1 :
            for (uint32_t col = 0; col <= cx; col++) {
                size_t idx      = (size_t)cy * c_width + col;
                text_grid[idx]  = ' ';
                color_grid[idx] = fore_color;
                if (bg_grid) bg_grid[idx] = back_color;
                fbcon_mark_cell_dirty(cy, col);
            }
            for (uint32_t r = 0; r < cy; r++) {
                memset(text_grid + (size_t)r * c_width, ' ', c_width);
                for (uint32_t col = 0; col < c_width; col++) {
                    size_t idx      = (size_t)r * c_width + col;
                    color_grid[idx] = fore_color;
                    if (bg_grid) bg_grid[idx] = back_color;
                    fbcon_mark_cell_dirty(r, col);
                }
            }
            break;
        case 2 :
        case 3 :
            memset(text_grid, ' ', (size_t)c_height * c_width);
            for (uint32_t r = 0; r < c_height; r++) {
                size_t base = (size_t)r * c_width;
                for (uint32_t col = 0; col < c_width; col++) {
                    color_grid[base + col] = fore_color;
                    if (bg_grid) bg_grid[base + col] = back_color;
                }
                if (dirty_first_col) {
                    dirty_first_col[r] = 0;
                    dirty_last_col[r]  = c_width - 1;
                }
            }
            break;
    }
}

void fbcon_erase_line(uint32_t mode, uint32_t y)
{
    if (!text_grid || !color_grid || y >= c_height) return;
    switch (mode) {
        case 0 :
            for (uint32_t col = cx; col < c_width; col++) {
                size_t idx      = (size_t)y * c_width + col;
                text_grid[idx]  = ' ';
                color_grid[idx] = fore_color;
                if (bg_grid) bg_grid[idx] = back_color;
                fbcon_mark_cell_dirty(y, col);
            }
            break;
        case 1 :
            for (uint32_t col = 0; col <= cx; col++) {
                size_t idx      = (size_t)y * c_width + col;
                text_grid[idx]  = ' ';
                color_grid[idx] = fore_color;
                if (bg_grid) bg_grid[idx] = back_color;
                fbcon_mark_cell_dirty(y, col);
            }
            break;
        case 2 :
            memset(text_grid + (size_t)y * c_width, ' ', c_width);
            for (uint32_t col = 0; col < c_width; col++) {
                size_t idx      = (size_t)y * c_width + col;
                color_grid[idx] = fore_color;
                if (bg_grid) bg_grid[idx] = back_color;
                fbcon_mark_cell_dirty(y, col);
            }
            break;
    }
}

void fbcon_erase_chars(uint32_t x, uint32_t y, uint32_t count)
{
    if (!text_grid || !color_grid || y >= c_height) return;
    if (x + count > c_width) count = c_width - x;
    for (uint32_t col = x; col < x + count; col++) {
        size_t idx      = (size_t)y * c_width + col;
        text_grid[idx]  = ' ';
        color_grid[idx] = fore_color;
        if (bg_grid) bg_grid[idx] = back_color;
        fbcon_mark_cell_dirty(y, col);
    }
}

void fbcon_insert_chars(uint32_t x, uint32_t y, uint32_t n, uint32_t cols)
{
    if (!text_grid || !color_grid || y >= c_height) return;
    if (x >= cols || n == 0) return;
    if (n > cols - x) n = cols - x;
    uint32_t move = cols - x - n;
    size_t   base = (size_t)y * cols;
    if (move > 0) {
        memmove(text_grid + base + x + n, text_grid + base + x, move);
        memmove(color_grid + base + x + n, color_grid + base + x, move * sizeof(uint32_t));
        if (bg_grid) memmove(bg_grid + base + x + n, bg_grid + base + x, move * sizeof(uint32_t));
    }
    for (uint32_t col = x; col < x + n && col < cols; col++) {
        size_t idx      = base + col;
        text_grid[idx]  = ' ';
        color_grid[idx] = fore_color;
        if (bg_grid) bg_grid[idx] = back_color;
        fbcon_mark_cell_dirty(y, col);
    }
    for (uint32_t col = x; col < cols; col++) fbcon_mark_cell_dirty(y, col);
}

void fbcon_delete_chars(uint32_t x, uint32_t y, uint32_t n, uint32_t cols)
{
    if (!text_grid || !color_grid || y >= c_height) return;
    if (x >= cols || n == 0) return;
    if (n > cols - x) n = cols - x;
    uint32_t move = cols - x - n;
    size_t   base = (size_t)y * cols;
    if (move > 0) {
        memmove(text_grid + base + x, text_grid + base + x + n, move);
        memmove(color_grid + base + x, color_grid + base + x + n, move * sizeof(uint32_t));
        if (bg_grid) memmove(bg_grid + base + x, bg_grid + base + x + n, move * sizeof(uint32_t));
    }
    for (uint32_t col = cols - n; col < cols; col++) {
        size_t idx      = base + col;
        text_grid[idx]  = ' ';
        color_grid[idx] = fore_color;
        if (bg_grid) bg_grid[idx] = back_color;
        fbcon_mark_cell_dirty(y, col);
    }
    for (uint32_t col = x; col < cols; col++) fbcon_mark_cell_dirty(y, col);
}

static void fbcon_flush_screen_updates(void)
{
    if (redraw_deferred) return;

    if (full_redraw_pending) {
        fbcon_redraw_screen();
        fbcon_clear_uncovered_bottom();
        full_redraw_pending = 0;
        video_flush_rect(0, 0, (uint32_t)width, (uint32_t)height);
        return;
    }

    fbcon_flush_dirty_rows();
}

/* Initialize framebuffer console */
void fbcon_init(void)
{
    /* Bitmap font: 9x16 pixels */
    font_width  = 9;
    font_height = 16;

    cx = cy = 0;

#if BOOT_LOGO
    c_width  = width > fbcon_offset_x ? (width - fbcon_offset_x) / font_width : 80;
    c_height = height > fbcon_offset_y ? (height - fbcon_offset_y) / font_height : 25;
#else
    c_width  = width / font_width;
    c_height = height / font_height;
#endif

    fore_color = color_to_fb_color((color_t) {0xaa, 0xaa, 0xaa});
    back_color = color_to_fb_color((color_t) {0x00, 0x00, 0x00});

    text_grid       = calloc((size_t)c_width * c_height, sizeof(char));
    color_grid      = malloc((size_t)c_width * c_height * sizeof(uint32_t));
    bg_grid         = malloc((size_t)c_width * c_height * sizeof(uint32_t));
    dirty_first_col = malloc((size_t)c_height * sizeof(uint32_t));
    dirty_last_col  = malloc((size_t)c_height * sizeof(uint32_t));
    if (!text_grid || !color_grid || !dirty_first_col || !dirty_last_col) {
        free(text_grid);
        free(color_grid);
        free(bg_grid);
        free(dirty_first_col);
        free(dirty_last_col);
        text_grid       = NULL;
        color_grid      = NULL;
        bg_grid         = NULL;
        dirty_first_col = NULL;
        dirty_last_col  = NULL;
    } else {
        for (uint32_t row = 0; row < c_height; row++) {
            fbcon_clear_row(row);
            dirty_first_col[row] = c_width;
            dirty_last_col[row]  = 0;
        }
    }

    full_redraw_pending = 0;
    redraw_deferred     = 0;

    vt_ansi_init(&vt_ansi_state, c_width, c_height);
    vt_ansi_set_default_colors(&vt_ansi_state, 0xaaaaaa, 0x000000);
}

/*
 * fbcon_resize ?reallocate text/color/dirty grids after a framebuffer
 * switch changes the screen dimensions.  Preserves the font size but
 * recalculates the character grid.
 */
void fbcon_resize(void)
{
    free(text_grid);
    free(color_grid);
    free(bg_grid);
    free(dirty_first_col);
    free(dirty_last_col);

#if BOOT_LOGO
    c_width  = (width - fbcon_offset_x) / font_width;
    c_height = (height - fbcon_offset_y) / font_height;
#else
    c_width  = width / font_width;
    c_height = height / font_height;
#endif

    text_grid       = calloc((size_t)c_width * c_height, sizeof(char));
    color_grid      = malloc((size_t)c_width * c_height * sizeof(uint32_t));
    bg_grid         = malloc((size_t)c_width * c_height * sizeof(uint32_t));
    dirty_first_col = malloc((size_t)c_height * sizeof(uint32_t));
    dirty_last_col  = malloc((size_t)c_height * sizeof(uint32_t));

    if (!text_grid || !color_grid || !dirty_first_col || !dirty_last_col) {
        free(text_grid);
        free(color_grid);
        free(bg_grid);
        free(dirty_first_col);
        free(dirty_last_col);
        text_grid       = NULL;
        color_grid      = NULL;
        bg_grid         = NULL;
        dirty_first_col = NULL;
        dirty_last_col  = NULL;
    } else {
        for (uint32_t row = 0; row < c_height; row++) {
            fbcon_clear_row(row);
            dirty_first_col[row] = c_width;
            dirty_last_col[row]  = 0;
        }
    }

    cx              = 0;
    cy              = 0;
    redraw_deferred = 0;

    vt_ansi_init(&vt_ansi_state, c_width, c_height);
    vt_ansi_set_default_colors(&vt_ansi_state, 0xaaaaaa, 0x000000);

    full_redraw_pending = 1;
    fbcon_flush_screen_updates();
}

/* Draw a character with per-cell foreground and background color */
void fbcon_draw_char_bg(const char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg)
{
    if (!buffer) return;
    uint8_t *char_font = ascii_font + (size_t)(uint8_t)c * font_height;
#if BOOT_LOGO
    uint32_t char_base_addr = (y + fbcon_offset_y + fbcon_draw_offset_y) * stride + (x + fbcon_offset_x);
#else
    uint32_t char_base_addr = y * stride + x;
#endif

    for (uint32_t row = 0; row < font_height; row++) {
        uint32_t *row_buf  = buffer + char_base_addr + row * stride;
        uint8_t   font_row = char_font[row];
        for (uint32_t col = 0; col < font_width; col++) row_buf[col] = (font_row & (0x80 >> col)) ? fg : bg;
    }
}

/* ANSI escape sequence parser instance and callbacks */

static uint32_t fbcon_rgb24_to_fb(uint32_t rgb24)
{
    color_t c = {(rgb24 >> 16) & 0xFF, (rgb24 >> 8) & 0xFF, rgb24 & 0xFF};
    return color_to_fb_color(c);
}

static void vt_ansicb_draw_char(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg)
{
    if (!text_grid || !color_grid || y >= c_height || x >= c_width) return;
    size_t idx      = (size_t)y * c_width + x;
    text_grid[idx]  = c;
    color_grid[idx] = fbcon_rgb24_to_fb(fg);
    if (bg_grid) bg_grid[idx] = fbcon_rgb24_to_fb(bg);
    fbcon_mark_cell_dirty(y, x);
}

static void vt_ansicb_scroll_up(uint32_t top, uint32_t bottom, uint32_t lines)
{
    fbcon_scroll_up(top, bottom, lines);
}

static void vt_ansicb_scroll_down(uint32_t top, uint32_t bottom, uint32_t lines)
{
    fbcon_scroll_down(top, bottom, lines);
}

static void vt_ansicb_erase_display(uint32_t mode, uint32_t x, uint32_t y)
{
    cx = x;
    cy = y;
    fbcon_erase_display(mode);
}

static void vt_ansicb_erase_line(uint32_t mode, uint32_t x, uint32_t y)
{
    cx = x;
    cy = y;
    fbcon_erase_line(mode, y);
}

static void vt_ansicb_erase_chars(uint32_t x, uint32_t y, uint32_t count)
{
    fbcon_erase_chars(x, y, count);
}

static void vt_ansicb_insert_lines(uint32_t y, uint32_t bottom, uint32_t n)
{
    fbcon_scroll_down(y, bottom, n);
}

static void vt_ansicb_delete_lines(uint32_t y, uint32_t bottom, uint32_t n)
{
    fbcon_scroll_up(y, bottom, n);
}

static void vt_ansicb_insert_chars(uint32_t x, uint32_t y, uint32_t n, uint32_t cols)
{
    fbcon_insert_chars(x, y, n, cols);
}

static void vt_ansicb_delete_chars(uint32_t x, uint32_t y, uint32_t n, uint32_t cols)
{
    fbcon_delete_chars(x, y, n, cols);
}

static void vt_ansicb_cursor_visible(bool visible)
{
    (void)visible;
}

static const vt_ansi_callbacks_t vt_ansi_cb = {
    .draw_char      = vt_ansicb_draw_char,
    .scroll_up      = vt_ansicb_scroll_up,
    .scroll_down    = vt_ansicb_scroll_down,
    .erase_display  = vt_ansicb_erase_display,
    .erase_line     = vt_ansicb_erase_line,
    .erase_chars    = vt_ansicb_erase_chars,
    .insert_lines   = vt_ansicb_insert_lines,
    .delete_lines   = vt_ansicb_delete_lines,
    .insert_chars   = vt_ansicb_insert_chars,
    .delete_chars   = vt_ansicb_delete_chars,
    .cursor_visible = vt_ansicb_cursor_visible,
};

/* Process a buffer of characters through the ANSI escape sequence parser */
void fbcon_ansi_write(const uint8_t *buf, size_t len)
{
    redraw_deferred++;
    for (size_t i = 0; i < len; i++) { vt_ansi_process(&vt_ansi_state, buf[i], &vt_ansi_cb, NULL); }
    redraw_deferred--;
    fbcon_flush_screen_updates();
}
