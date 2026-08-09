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
#include <drivers/gpu/fbdev/fbcon.h>
#include <drivers/gpu/fbdev/klogo.h>
#include <drivers/gpu/fbdev/video.h>
#include <drivers/gpu/fbdev/vt_ansi.h>
#include <drivers/tty/tty.h>
#include <kernel/timer/timer.h>
#include <libs/gfxs/fonts.h>
#include <libs/gfxs/gfx_proc.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

vt_ansi_state_t vt_ansi_state;

/* Bitmap fonts */

static char     *text_grid       = 0;
static uint32_t *color_grid      = 0;
static uint32_t *bg_grid         = 0;
static uint32_t *dirty_first_col = 0;
static uint32_t *dirty_last_col  = 0;
static uint8_t   full_redraw_pending;
static uint8_t   redraw_deferred;
static bool      handoff_in_progress;

/*
 * Serializes all text-grid / framebuffer mutations: tty and printk output
 * (console_emit_lock -> fbcon_lock) plus the cursor blink path, which takes
 * only fbcon_lock.  Never acquire console_emit_lock or video_state_lock
 * while holding this lock.
 */
static spinlock_t fbcon_lock;

/*
 * Blink state.  cursor_drawn records where the block cursor currently sits
 * so the next phase flip can restore that cell from the text grid.
 */
#define CURSOR_BLINK_INTERVAL ((TIMER_HZ * 2) / 5) // 400 ms per phase

static uint64_t cursor_last_tick;
static bool     cursor_phase;
static bool     cursor_drawn;
static uint32_t cursor_drawn_row;
static uint32_t cursor_drawn_col;

#if BOOT_LOGO
static uint32_t logo_rows;       // grid rows covered by the boot logo
static uint32_t logo_cover_rows; // top rows still showing the logo (redraw-protected)
static bool     logo_released;   // true once kernel init handed the screen back
#endif

/*
 * True while a row still sits inside the boot-logo area.  Such rows must
 * never be repainted from the (blank) text grid, otherwise the logo bitmap
 * drawn on top of them would be erased before the console scrolls over it.
 * After kernel init this count shrinks line by line as scrolling consumes
 * the blank rows, so the logo fades out naturally.
 */
static bool fbcon_row_logo_protected(uint32_t row)
{
#if BOOT_LOGO
    return row < logo_cover_rows;
#else
    (void)row;
    return false;
#endif
}

static void fbcon_mark_cell_dirty(uint32_t row, uint32_t col)
{
    if (!dirty_first_col || !dirty_last_col || row >= c_height || col >= c_width) return;

    if (dirty_first_col[row] > col) dirty_first_col[row] = col;
    if (dirty_last_col[row] < col) dirty_last_col[row] = col;
}

#if BOOT_LOGO
/*
 * Reserve the top logo_rows rows for the boot logo.  The console scrolls
 * below them; text starts just under the logo.  Once kernel init has handed
 * the screen back (fbcon_release_logo) a late redraw never re-reserves.
 */
static void fbcon_set_logo_active_locked(bool active)
{
    if (!active) {
        logo_cover_rows          = 0;
        vt_ansi_state.scroll_top = 0;
        return;
    }
    if (logo_released) return;
    if (!logo_rows && font_height) logo_rows = (KLOGO_AREA_HEIGHT + font_height - 1) / font_height;
    if (logo_rows == 0 || logo_rows >= c_height) return;
    logo_cover_rows          = logo_rows;
    vt_ansi_state.scroll_top = logo_rows;
    if (vt_ansi_state.y < logo_rows) vt_ansi_state.y = logo_rows;
}

/*
 * An explicit erase that reaches above the logo reclaims the area for good
 * (like Linux clearing above vc_top); the logo is then wiped by the clear.
 */
static void fbcon_erase_release_logo_locked(void)
{
    if (!logo_cover_rows) return;
    logo_cover_rows          = 0;
    vt_ansi_state.scroll_top = 0;
}

/*
 * Kernel-init completion: hand the whole screen back to the console without
 * touching the logo pixels.  The top rows stay blank (logo still visible)
 * and the first console scrolls cover them line by line.
 */
static void fbcon_release_logo_locked(void)
{
    if (!logo_cover_rows) return;
    logo_released            = true;
    vt_ansi_state.scroll_top = 0;
}
#endif

/* Reserve the boot-logo area (called when the logo is actually drawn). */
void fbcon_set_logo_active(bool active)
{
#if BOOT_LOGO
    spin_lock(&fbcon_lock);
    fbcon_set_logo_active_locked(active);
    spin_unlock(&fbcon_lock);
#else
    (void)active;
#endif
}

/*
 * Release the boot-logo area at kernel-init completion.  The logo bitmap is
 * left untouched; the console merely reclaims the full screen and scrolling
 * gradually covers it, matching Linux fbcon behaviour.
 */
void fbcon_release_logo(void)
{
#if BOOT_LOGO
    spin_lock(&fbcon_lock);
    fbcon_release_logo_locked();
    spin_unlock(&fbcon_lock);
#endif
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
    if (fbcon_row_logo_protected(row)) return;
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
        if (fbcon_row_logo_protected(row)) {
            dirty_first_col[row] = c_width;
            dirty_last_col[row]  = 0;
            continue;
        }

        uint32_t x1 = dirty_first_col[row] * font_width;
        uint32_t y1 = row * font_height;
        uint32_t x2 = (dirty_last_col[row] + 1) * font_width;
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
    uint32_t used_height = c_height * font_height;
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
        if (dirty_first_col && dirty_last_col) {
            dirty_first_col[r] = 0;
            dirty_last_col[r]  = c_width - 1;
        }
    }
#if BOOT_LOGO
    /*
     * A scroll that spans the top of the screen consumes the blank rows
     * still hiding the logo, so the logo is covered one line at a time.
     */
    if (top == 0 && logo_cover_rows) {
        if (lines >= logo_cover_rows)
            logo_cover_rows = 0;
        else
            logo_cover_rows -= lines;
    }
#endif
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
        if (dirty_first_col && dirty_last_col) {
            dirty_first_col[r] = 0;
            dirty_last_col[r]  = c_width - 1;
        }
    }
    full_redraw_pending = 1;
}

void fbcon_erase_display(uint32_t mode)
{
    if (!text_grid || !color_grid) return;
#if BOOT_LOGO
    /*
     * A clear that reaches above the logo reclaims the area just like Linux
     * (clearing above vc_top releases the logo).  Mode 0 reaches the top
     * rows only when the cursor sits inside the logo area.
     */
    if (logo_cover_rows && (mode == 1 || mode == 2 || mode == 3 || (mode == 0 && cy < logo_cover_rows))) fbcon_erase_release_logo_locked();
#endif
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
                if (dirty_first_col && dirty_last_col) {
                    dirty_first_col[r] = 0;
                    dirty_last_col[r]  = c_width - 1;
                }
            }
            break;
        default :
            break;
    }
}

void fbcon_erase_line(uint32_t mode, uint32_t y)
{
    if (!text_grid || !color_grid || y >= c_height) return;
#if BOOT_LOGO
    if (logo_cover_rows && y < logo_cover_rows) fbcon_erase_release_logo_locked();
#endif
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
        default :
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

    /*
     * The console always owns the full framebuffer grid.  The boot logo is
     * an overlay on top of the first rows, not a reduction of the console
     * area; it is reserved (and later reclaimed by scrolling) dynamically.
     */
    c_width  = width ? (uint32_t)(width / font_width) : 80;
    c_height = height ? (uint32_t)((height + font_height - 1) / font_height) : 25;

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

    cursor_last_tick = 0;
    cursor_phase     = false;
    cursor_drawn     = false;

    vt_ansi_init(&vt_ansi_state, c_width, c_height);
    vt_ansi_set_default_colors(&vt_ansi_state, 0xaaaaaa, 0x000000);

#if BOOT_LOGO
    logo_rows       = (KLOGO_AREA_HEIGHT + font_height - 1) / font_height;
    logo_cover_rows = 0;
    logo_released   = false;
#endif

    tty_console_resize((uint16_t)c_height, (uint16_t)c_width);
}

/*
 * fbcon_resize ?reallocate text/color/dirty grids after a framebuffer
 * switch changes the screen dimensions.  Preserves the font size but
 * recalculates the character grid.  When the resolution is unchanged the
 * existing grids and cursor state are kept so a seamless buffer handoff
 * (e.g. boot framebuffer -> DRM GEM) keeps the visible content intact.
 */
void fbcon_resize(void)
{
    uint32_t  new_cw      = width ? (uint32_t)(width / font_width) : 80;
    uint32_t  new_ch      = height ? (uint32_t)((height + font_height - 1) / font_height) : 25;
    bool      dim_changed = false;
    bool      rebuilt     = false;
    char     *new_text = NULL, *old_text = NULL;
    uint32_t *new_color = NULL, *new_bg = NULL, *new_first = NULL, *new_last = NULL;
    uint32_t *old_color = NULL, *old_bg = NULL, *old_first = NULL, *old_last = NULL;

    if (!new_cw) new_cw = 1;
    if (!new_ch) new_ch = 1;

    if (new_cw != c_width || new_ch != c_height) {
        dim_changed  = true;
        size_t cells = (size_t)new_cw * new_ch;
        new_text     = malloc(cells);
        new_color    = malloc(cells * sizeof(*new_color));
        new_bg       = malloc(cells * sizeof(*new_bg));
        new_first    = malloc((size_t)new_ch * sizeof(*new_first));
        new_last     = malloc((size_t)new_ch * sizeof(*new_last));
        if (new_text && new_color && new_bg && new_first && new_last) {
            memset(new_text, ' ', cells);
            for (size_t i = 0; i < cells; i++) {
                new_color[i] = fore_color;
                new_bg[i]    = back_color;
            }
            for (uint32_t row = 0; row < new_ch; row++) {
                new_first[row] = new_cw;
                new_last[row]  = 0;
            }
        } else {
            free(new_text);
            free(new_color);
            free(new_bg);
            free(new_first);
            free(new_last);
            new_text  = NULL;
            new_color = new_bg = new_first = new_last = NULL;
        }
    }

    spin_lock(&fbcon_lock);
    if (dim_changed) {
        rebuilt         = true;
        old_text        = text_grid;
        old_color       = color_grid;
        old_bg          = bg_grid;
        old_first       = dirty_first_col;
        old_last        = dirty_last_col;
        c_width         = new_cw;
        c_height        = new_ch;
        text_grid       = new_text;
        color_grid      = new_color;
        bg_grid         = new_bg;
        dirty_first_col = new_first;
        dirty_last_col  = new_last;

        cx = 0;
        cy = 0;

        vt_ansi_init(&vt_ansi_state, c_width, c_height);
        vt_ansi_set_default_colors(&vt_ansi_state, 0xaaaaaa, 0x000000);
    }

#if BOOT_LOGO
    /* Keep the boot-logo reservation in sync after any rebuild. */
    if (logo_cover_rows) {
        if (logo_rows && c_height > logo_rows) {
            vt_ansi_state.scroll_top = logo_rows;
            if (vt_ansi_state.y < logo_rows) vt_ansi_state.y = logo_rows;
        } else {
            logo_cover_rows = 0;
        }
    }
#endif

    cursor_last_tick = 0;
    cursor_phase     = false;
    cursor_drawn     = false;

    if (rebuilt) full_redraw_pending = 1;
    uint16_t rows = (uint16_t)c_height;
    uint16_t cols = (uint16_t)c_width;
    spin_unlock(&fbcon_lock);

    free(old_text);
    free(old_color);
    free(old_bg);
    free(old_first);
    free(old_last);
    tty_console_resize(rows, cols);
}

void fbcon_handoff_begin(void)
{
    spin_lock(&fbcon_lock);
    if (!handoff_in_progress) {
        handoff_in_progress = true;
        redraw_deferred++;
    }
    spin_unlock(&fbcon_lock);
}

void fbcon_handoff_end(void)
{
    spin_lock(&fbcon_lock);
    if (handoff_in_progress) {
        handoff_in_progress = false;
        if (redraw_deferred) redraw_deferred--;
    }
    fbcon_flush_screen_updates();
    spin_unlock(&fbcon_lock);
}

/*
 * Draw a character with per-cell foreground and background color.  The
 * grid uses ceil(height / font_height) rows so the console fills the whole
 * screen; the last row may extend past the physical height and is clipped
 * here.
 */
void fbcon_draw_char_bg(const char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg)
{
    uint32_t draw_rows;
    uint32_t draw_cols;
    uint32_t row;

    if (!buffer || x >= (uint32_t)width || y >= (uint32_t)height) return;
    draw_rows = font_height;
    draw_cols = font_width;
    if ((uint64_t)y + font_height > height) draw_rows = (uint32_t)height - y;
    if ((uint64_t)x + font_width > width) draw_cols = (uint32_t)width - x;

    uint8_t *char_font      = ascii_font + (size_t)(uint8_t)c * font_height;
    uint32_t char_base_addr = y * stride + x;

    for (row = 0; row < draw_rows; row++) {
        uint32_t *row_buf  = buffer + char_base_addr + row * stride;
        uint8_t   font_row = char_font[row];
        for (uint32_t col = 0; col < draw_cols; col++) row_buf[col] = (font_row & (0x80 >> col)) ? fg : bg;
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
    spin_lock(&fbcon_lock);
    redraw_deferred++;
    for (size_t i = 0; i < len; i++) { vt_ansi_process(&vt_ansi_state, buf[i], &vt_ansi_cb, NULL); }
    redraw_deferred--;
    fbcon_flush_screen_updates();
    spin_unlock(&fbcon_lock);
}

/* Flip the block cursor phase and repaint directly into the framebuffer. */
void fbcon_cursor_tick(uint64_t now_ticks)
{
    spin_lock(&fbcon_lock);
    if (handoff_in_progress) goto out;
    if (now_ticks - cursor_last_tick < CURSOR_BLINK_INTERVAL) goto out;
    cursor_last_tick = now_ticks;
    cursor_phase     = !cursor_phase;

    /* Restore the cell where the cursor was drawn last phase. */
    if (cursor_drawn) {
        if (cursor_drawn_row < c_height && cursor_drawn_col < c_width)
            fbcon_redraw_row_range(cursor_drawn_row, cursor_drawn_col, cursor_drawn_col);
        cursor_drawn = false;
    }

    /*
     * Paint the block cursor at the current cursor position (reverse video).
     * Never paint over the still-visible boot logo.
     */
    if (cursor_phase && vt_ansi_state.cursor_visible && text_grid && color_grid) {
        uint32_t row = vt_ansi_state.y;
        uint32_t col = vt_ansi_state.x;
        if (row < c_height && col < c_width && !fbcon_row_logo_protected(row)) {
            size_t   idx = (size_t)row * c_width + col;
            uint32_t fg  = color_grid[idx];
            uint32_t bg  = bg_grid ? bg_grid[idx] : back_color;
            fbcon_draw_char_bg(text_grid[idx], col * font_width, row * font_height, bg, fg);
            cursor_drawn     = true;
            cursor_drawn_row = row;
            cursor_drawn_col = col;
        }
    }
out:
    spin_unlock(&fbcon_lock);
}
