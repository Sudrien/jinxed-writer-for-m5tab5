/*
 * editor.c -- the text, the cursor, and the screen. See editor.h.
 *
 * The text is one flat byte buffer with a cursor index into it, ASCII
 * only: kbd_ascii() types nothing else yet. Every keystroke lays the
 * whole text out into lines again and repaints the screen. At the sizes
 * a day's notes reach that is far below a keystroke's worth of time, and
 * a layout that is always recomputed cannot disagree with the text.
 *
 * Wrapping is the editor's own, not gfx_wrap_line(): that one drops the
 * spaces it breaks at, which is right for a label and wrong for text
 * with a cursor in it, where every byte has to have a place on screen.
 * ark12's halfwidth cell is monospaced, so a line is a column count.
 *
 * SPDX-License-Identifier: MIT
 */
#include "editor.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "kbd.h"

static const char *TAG = "editor";

/* Text scale over the 12px font. Spec section 5 makes it a setting; it
 * is a constant until the settings page exists. 3 is 36px glyphs, 58
 * columns across the landscape panel. */
#define EDITOR_SCALE    (3)

#define TEXT_MAX        (256 * 1024)
#define MARGIN_X        (40)
#define MARGIN_Y        (32)
#define TAB_SPACES      (4)

/* Held-key repeat, which the keyboard does not do for us: boot reports
 * are a level, and kbd.c turns them into one press. */
#define REPEAT_DELAY_MS (400)
#define REPEAT_RATE_MS  (35)

static char     *s_text;        /* TEXT_MAX bytes, PSRAM */
static uint32_t  s_len;
static uint32_t  s_cur;
static bool      s_caps;
static int       s_want_col = -1;   /* up/down keep their column */

/* Layout, rebuilt by layout(). Line i is s_text[s_ls[i], s_le[i]) on
 * screen, and the next line starts at s_ls[i + 1]. */
static uint32_t *s_ls;
static uint32_t *s_le;
static int       s_lines;
static int       s_top;         /* first line on screen */

static int       s_cols, s_rows, s_line_h, s_status_y;
static char     *s_row_buf;     /* one line, NUL-terminated, for gfx */

/* ---- layout ----------------------------------------------------------- */

/*
 * One line from `s`: where its visible text ends and where the next
 * begins. A newline ends a line and is not drawn. A line that would pass
 * the right edge breaks after its last space -- the space stays on this
 * line, so the cursor has somewhere to sit before the next word -- or,
 * with no space to break at, mid-word at the edge.
 */
static void line_from(uint32_t s, uint32_t *end, uint32_t *next)
{
    uint32_t last_space = UINT32_MAX;
    for (uint32_t i = s; i < s_len; i++) {
        const char c = s_text[i];
        if (c == '\n') { *end = i; *next = i + 1; return; }
        if (i - s == (uint32_t)s_cols) {
            if (c == ' ') { *end = i; *next = i + 1; return; }
            if (last_space != UINT32_MAX) { *end = last_space; *next = last_space + 1; return; }
            *end = i; *next = i;
            return;
        }
        if (c == ' ') last_space = i;
    }
    *end = s_len;
    *next = s_len + 1;      /* past the end: this was the last line */
}

static void layout(void)
{
    uint32_t s = 0;
    s_lines = 0;
    for (;;) {
        uint32_t e, n;
        line_from(s, &e, &n);
        s_ls[s_lines] = s;
        s_le[s_lines] = e;
        s_lines++;
        /* Past the end: that was the last line. Text ending in a
         * newline gets an empty one after it, from line_from(s_len),
         * which is where the cursor goes after Enter. */
        if (n > s_len) break;
        s = n;
    }
}

static uint32_t line_next(int l)
{
    return (l + 1 < s_lines) ? s_ls[l + 1] : s_len + 1;
}

/* The line the cursor is on. A position belongs to the line it starts
 * before, except the very end of the text, which is on the last line. */
static int cursor_line(void)
{
    for (int l = 0; l < s_lines; l++) {
        if (s_cur >= s_ls[l] && s_cur < line_next(l)) return l;
    }
    return s_lines - 1;
}

/* The furthest the cursor can sit on line l. On a line cut mid-word the
 * end is the next line's start, which belongs to the next line. */
static uint32_t line_cursor_max(int l)
{
    const uint32_t e = s_le[l];
    if (l + 1 < s_lines && e == s_ls[l + 1] && e > s_ls[l]) return e - 1;
    return e;
}

/* ---- drawing ---------------------------------------------------------- */

static uint32_t word_count(void)
{
    uint32_t n = 0;
    bool in_word = false;
    for (uint32_t i = 0; i < s_len; i++) {
        const bool sp = s_text[i] == ' ' || s_text[i] == '\n' || s_text[i] == '\t';
        if (!sp && !in_word) n++;
        in_word = !sp;
    }
    return n;
}

static void redraw(void)
{
    layout();
    const int cl = cursor_line();
    if (cl < s_top) s_top = cl;
    if (cl >= s_top + s_rows) s_top = cl - s_rows + 1;

    const int W = gfx_w();
    const int H = gfx_h();
    const int gw = GFX_GLYPH_W(EDITOR_SCALE);
    gfx_fill_rect(0, 0, W, H, COL_BG);

    for (int r = 0; r < s_rows && s_top + r < s_lines; r++) {
        const int l = s_top + r;
        const uint32_t n = s_le[l] - s_ls[l];
        memcpy(s_row_buf, s_text + s_ls[l], n);
        s_row_buf[n] = '\0';
        gfx_draw_text(MARGIN_X, MARGIN_Y + r * s_line_h, s_row_buf, EDITOR_SCALE,
                      W - MARGIN_X, COL_TEXT);
    }

    /* A bar before the character, as tall as the glyph. */
    const int cx = MARGIN_X + (int)(s_cur - s_ls[cl]) * gw;
    const int cy = MARGIN_Y + (cl - s_top) * s_line_h;
    gfx_fill_rect(cx - EDITOR_SCALE, cy, EDITOR_SCALE, GFX_GLYPH_H(EDITOR_SCALE), COL_CURSOR);

    /* The status line: word count, and only that -- plus a note when
     * there is nothing to type on. Spec section 5. */
    char st[48];
    snprintf(st, sizeof(st), "%lu words", (unsigned long)word_count());
    gfx_draw_text(W - MARGIN_X - gfx_text_w(st, 2), s_status_y, st, 2, W, COL_DIM);
    if (!kbd_present()) gfx_draw_text(MARGIN_X, s_status_y, "no keyboard", 2, W, COL_DIM);
    if (s_caps)        gfx_draw_text(MARGIN_X + 15 * GFX_GLYPH_W(2), s_status_y, "caps", 2, W, COL_DIM);

    gfx_blit(0, H);
}

/* ---- editing ---------------------------------------------------------- */

static void insert(const char *s, uint32_t n)
{
    if (s_len + n > TEXT_MAX) {
        ESP_LOGW(TAG, "buffer full");
        return;
    }
    memmove(s_text + s_cur + n, s_text + s_cur, s_len - s_cur);
    memcpy(s_text + s_cur, s, n);
    s_len += n;
    s_cur += n;
}

static void erase(uint32_t from, uint32_t to)
{
    if (to > s_len) to = s_len;
    if (from >= to) return;
    memmove(s_text + from, s_text + to, s_len - to);
    s_len -= to - from;
    s_cur = from;
}

static void vertical(int dl)
{
    const int cl = cursor_line();
    if (s_want_col < 0) s_want_col = (int)(s_cur - s_ls[cl]);
    const int tl = cl + dl;
    if (tl < 0)        { s_cur = 0; return; }
    if (tl >= s_lines) { s_cur = s_len; return; }
    uint32_t p = s_ls[tl] + (uint32_t)s_want_col;
    const uint32_t mx = line_cursor_max(tl);
    s_cur = p > mx ? mx : p;
}

/* One key press (or repeat). True when the screen needs redrawing. */
static bool key(uint8_t usage, uint8_t mods)
{
    const bool gui  = (mods & KBD_MOD_GUI)  != 0;
    const bool ctrl = (mods & KBD_MOD_CTRL) != 0;
    const bool alt  = (mods & KBD_MOD_ALT)  != 0;
    const bool keep_col = usage == KBD_UP || usage == KBD_DOWN ||
                          usage == KBD_PAGEUP || usage == KBD_PAGEDOWN;
    if (!keep_col) s_want_col = -1;

    layout();
    const int cl = cursor_line();

    /* GUI is the app modifier (spec section 5): Windows key or Command. */
    if (gui) {
        switch (usage) {
        case KBD_LEFT:      s_cur = s_ls[cl]; return true;
        case KBD_RIGHT:     s_cur = line_cursor_max(cl); return true;
        case KBD_UP:        s_cur = 0; return true;
        case KBD_DOWN:      s_cur = s_len; return true;
        case KBD_BACKSPACE: {
            /* The whole visual line, and its newline if it has one. */
            uint32_t to = line_next(cl);
            erase(s_ls[cl], to);
            return true;
        }
        default:            return false;
        }
    }

    switch (usage) {
    case KBD_CAPSLOCK:  s_caps = !s_caps; return true;
    case KBD_LEFT:      if (s_cur > 0) s_cur--; return true;
    case KBD_RIGHT:     if (s_cur < s_len) s_cur++; return true;
    case KBD_UP:        vertical(-1); return true;
    case KBD_DOWN:      vertical(1); return true;
    case KBD_PAGEUP:    vertical(-(s_rows - 1)); return true;
    case KBD_PAGEDOWN:  vertical(s_rows - 1); return true;
    case KBD_HOME:      s_cur = s_ls[cl]; return true;
    case KBD_END:       s_cur = line_cursor_max(cl); return true;
    case KBD_BACKSPACE: if (s_cur > 0) erase(s_cur - 1, s_cur); return true;
    case KBD_DELETE:    erase(s_cur, s_cur + 1); return true;
    case KBD_TAB:       insert("    ", TAB_SPACES); return true;
    default:            break;
    }

    /* Ctrl and Alt chords type nothing; they are left for shortcuts. */
    if (ctrl || alt) return false;
    const char c = kbd_ascii(usage, mods, s_caps);
    if (c == '\n' || (c >= 0x20 && c < 0x7F)) {
        insert(&c, 1);
        return true;
    }
    return false;
}

static bool repeats(uint8_t usage)
{
    return usage != KBD_CAPSLOCK && usage != KBD_ESCAPE && usage < 0xE0;
}

/* ---- the loop --------------------------------------------------------- */

esp_err_t editor_init(void)
{
    s_text    = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    s_ls      = heap_caps_malloc((TEXT_MAX + 2) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_le      = heap_caps_malloc((TEXT_MAX + 2) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!s_text || !s_ls || !s_le) return ESP_ERR_NO_MEM;

    const int W = gfx_w();
    const int H = gfx_h();
    s_cols   = (W - 2 * MARGIN_X) / GFX_GLYPH_W(EDITOR_SCALE);
    s_line_h = GFX_GLYPH_H(EDITOR_SCALE) + 4 * EDITOR_SCALE;
    s_status_y = H - MARGIN_Y / 2 - GFX_GLYPH_H(2);
    s_rows   = (s_status_y - MARGIN_Y - 8) / s_line_h;
    s_row_buf = heap_caps_malloc((size_t)s_cols + 2, MALLOC_CAP_DEFAULT);
    if (!s_row_buf) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "%d columns, %d rows, scale %d", s_cols, s_rows, EDITOR_SCALE);
    redraw();
    return ESP_OK;
}

void editor_run(void)
{
    uint8_t  held = 0, held_mods = 0;   /* the key repeating, if any */
    TickType_t next_repeat = 0;
    bool had_kbd = kbd_present();

    for (;;) {
        TickType_t wait = pdMS_TO_TICKS(500);
        if (held) {
            const TickType_t now = xTaskGetTickCount();
            wait = (next_repeat > now) ? next_repeat - now : 0;
        }

        kbd_event_t ev;
        bool dirty = false;
        if (kbd_get(&ev, wait)) {
            if (ev.down) {
                dirty = key(ev.usage, ev.mods);
                if (repeats(ev.usage)) {
                    held = ev.usage;
                    held_mods = ev.mods;
                    next_repeat = xTaskGetTickCount() + pdMS_TO_TICKS(REPEAT_DELAY_MS);
                }
            } else if (ev.usage == held) {
                held = 0;
            }
        } else if (held) {
            dirty = key(held, held_mods);
            next_repeat = xTaskGetTickCount() + pdMS_TO_TICKS(REPEAT_RATE_MS);
        }

        /* Plugging or unplugging shows in the status line. */
        const bool has_kbd = kbd_present();
        if (has_kbd != had_kbd) { had_kbd = has_kbd; dirty = true; }

        if (dirty) redraw();
    }
}
