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
 * Saving: s_base is the text as the log on the card replays to. When
 * typing pauses (or has gone on without a pause for SAVE_MAX_MS) the
 * difference between s_base and s_text goes to doclog_append() as one
 * changeset, on this task. The card is leased per flush and never held.
 * With no card, or a failed write, s_text simply runs ahead of s_base
 * and the next flush that lands carries everything since.
 *
 * Spelling (spell.h) is checked only when asked, F7 or GUI+semicolon:
 * the screen splits, the text above with the word marked, the word and
 * what it might be below. A correction is an ordinary edit, but logged
 * on its own and marked as the checker's (DOCLOG_SRC_SYMSPELL): what is
 * pending is flushed before it and it is flushed straight after. If a
 * flush cannot land (no card), user's and checker's changes end up in one
 * changeset, and that is marked as the user's -- the checker is credited
 * only with what is wholly its own.
 *
 * SPDX-License-Identifier: MIT
 */
#include "editor.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "doclog.h"
#include "kbd.h"
#include "rtc8130.h"
#include "settings.h"
#include "spell.h"
#include "storage.h"
#include "touch.h"

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

/* Flush to the card this long after the last keystroke, or after this
 * long of typing without such a pause. */
#define SAVE_PAUSE_MS   (1500)
#define SAVE_MAX_MS     (10000)
/* After a failed open, wait this long before trying the card again. */
#define ATTACH_RETRY_MS (5000)

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

/* Saving. See the top of the file. */
static char     *s_base;        /* TEXT_MAX bytes, PSRAM */
static uint32_t  s_base_len;
static char      s_name[32];    /* "2026-11-02.md" */
static bool      s_pending;     /* s_text may differ from s_base */
static doclog_src_t s_src;      /* who made what is pending */
static doclog_src_t s_edit_src = DOCLOG_SRC_USER;  /* who is editing now */
static TickType_t s_last_edit, s_first_edit;
static bool      s_save_failed;
static bool      s_ever_open;
static uint32_t  s_gen;
static TickType_t s_attach_after;

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

/* The line position p is on. A position belongs to the line it starts
 * before, except the very end of the text, which is on the last line. */
static int pos_line(uint32_t p)
{
    for (int l = 0; l < s_lines; l++) {
        if (p >= s_ls[l] && p < line_next(l)) return l;
    }
    return s_lines - 1;
}

static int cursor_line(void) { return pos_line(s_cur); }

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

/* `rows` lines of text from line `top`, with s_text[hl_a, hl_b) marked
 * behind. After layout(). */
static void draw_rows(int top, int rows, uint32_t hl_a, uint32_t hl_b)
{
    const int W = gfx_w();
    const int gw = GFX_GLYPH_W(EDITOR_SCALE);
    for (int r = 0; r < rows && top + r < s_lines; r++) {
        const int l = top + r;
        const int y = MARGIN_Y + r * s_line_h;
        const uint32_t a = hl_a > s_ls[l] ? hl_a : s_ls[l];
        const uint32_t b = hl_b < s_le[l] ? hl_b : s_le[l];
        if (a < b) {
            gfx_fill_rect(MARGIN_X + (int)(a - s_ls[l]) * gw - EDITOR_SCALE, y - EDITOR_SCALE,
                          (int)(b - a) * gw + EDITOR_SCALE,
                          GFX_GLYPH_H(EDITOR_SCALE) + 2 * EDITOR_SCALE, COL_BTN);
        }
        const uint32_t n = s_le[l] - s_ls[l];
        memcpy(s_row_buf, s_text + s_ls[l], n);
        s_row_buf[n] = '\0';
        gfx_draw_text(MARGIN_X, y, s_row_buf, EDITOR_SCALE, W - MARGIN_X, COL_TEXT);
    }
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
    draw_rows(s_top, s_rows, 0, 0);

    /* A bar before the character, as tall as the glyph. */
    const int cx = MARGIN_X + (int)(s_cur - s_ls[cl]) * gw;
    const int cy = MARGIN_Y + (cl - s_top) * s_line_h;
    gfx_fill_rect(cx - EDITOR_SCALE, cy, EDITOR_SCALE, GFX_GLYPH_H(EDITOR_SCALE), COL_CURSOR);

    /* The status line: word count, and only that -- plus a note when
     * there is nothing to type on. Spec section 5. */
    char st[48];
    snprintf(st, sizeof(st), "%lu words", (unsigned long)word_count());
    /* Right-aligned, leaving the corner to the gear. */
    gfx_draw_text(W - 80 - gfx_text_w(st, 2), s_status_y, st, 2, W, COL_DIM);
    settings_draw_gear();
    if (!kbd_present()) gfx_draw_text(MARGIN_X, s_status_y, "no keyboard", 2, W, COL_DIM);
    if (s_caps)        gfx_draw_text(MARGIN_X + 15 * GFX_GLYPH_W(2), s_status_y, "caps", 2, W, COL_DIM);
    if (!storage_present(STORAGE_SD)) {
        gfx_draw_text(MARGIN_X + 21 * GFX_GLYPH_W(2), s_status_y, "no card", 2, W, COL_DIM);
    } else if (s_save_failed) {
        gfx_draw_text(MARGIN_X + 21 * GFX_GLYPH_W(2), s_status_y, "not saved", 2, W, COL_DIM);
    }

    gfx_blit(0, H);
}

/* ---- saving ----------------------------------------------------------- */

static bool rtc_now(int64_t *now)
{
    if (rtc8130_read(now)) return true;
    *now = 0;
    return false;
}

static void edited(void)
{
    const TickType_t now = xTaskGetTickCount();
    if (!s_pending) {
        s_first_edit = now;
        s_src = s_edit_src;
    } else if (s_src != s_edit_src) {
        s_src = DOCLOG_SRC_USER;    /* mixed: the user's */
    }
    s_pending = true;
    s_last_edit = now;
}

/*
 * Get the log open on whatever card is in the slot. At boot that is a
 * plain recovery into the editor. Later -- a card that arrived after
 * typing started, or a card swapped -- what the card holds is put
 * before what is in RAM, so nothing on either side is lost: the next
 * flush appends the RAM text to the card's document as one changeset.
 * True when the screen needs redrawing.
 */
static bool attach(void)
{
    if (!storage_present(STORAGE_SD)) return false;
    const uint32_t gen = storage_generation();
    if (doclog_is_open() && gen == s_gen) return false;
    if (xTaskGetTickCount() < s_attach_after) return false;

    /* The same card back, untouched: carry on where we were. */
    if (s_ever_open && doclog_verify() == ESP_OK) {
        s_gen = gen;
        return false;
    }

    int64_t now;
    const bool now_ok = rtc_now(&now);
    uint32_t rl = 0;
    if (doclog_open(s_name, s_base, &rl, now, now_ok) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open the log for %s; retrying", s_name);
        s_attach_after = xTaskGetTickCount() + pdMS_TO_TICKS(ATTACH_RETRY_MS);
        return false;
    }
    s_base_len = rl;
    s_gen = gen;

    if (s_len == 0) {
        /* Daily mode lands at the end of today's file (spec 5). */
        memcpy(s_text, s_base, rl);
        s_len = rl;
        s_cur = s_len;
    } else if (rl > 0 && (uint64_t)rl + s_len <= TEXT_MAX) {
        if (s_ever_open) ESP_LOGW(TAG, "a different log on this card; keeping both texts");
        memmove(s_text + rl, s_text, s_len);
        memcpy(s_text, s_base, rl);
        s_len += rl;
        s_cur += rl;
        edited();
    } else if (rl > 0) {
        /* Both will not fit. Keep RAM's; the card's text stays in its
         * log's history, which the next flush builds on. */
        ESP_LOGW(TAG, "card text and RAM text do not fit together; keeping RAM's");
        edited();
    } else if (s_len) {
        edited();
    }
    s_ever_open = true;
    return true;
}

/* Flush if typing has paused. True when the status line changed. */
static bool save_tick(bool force)
{
    bool redraw_needed = attach();
    if (!s_pending || !doclog_is_open()) return redraw_needed;

    const TickType_t now = xTaskGetTickCount();
    if (!force && now - s_last_edit < pdMS_TO_TICKS(SAVE_PAUSE_MS) &&
        now - s_first_edit < pdMS_TO_TICKS(SAVE_MAX_MS)) {
        return redraw_needed;
    }

    int64_t t;
    const bool t_ok = rtc_now(&t);
    const bool was_failed = s_save_failed;
    if (doclog_append(s_base, s_base_len, s_text, s_len, t, t_ok, s_src) == ESP_OK) {
        memcpy(s_base, s_text, s_len);
        s_base_len = s_len;
        s_pending = false;
        s_save_failed = false;
    } else {
        /* Try again after another pause's worth. */
        s_save_failed = true;
        s_last_edit = now;
        s_first_edit = now;
    }
    return redraw_needed || was_failed != s_save_failed;
}

/* The day's file name, from the RTC. Fixed for the session for now;
 * midnight rollover (spec 5) is later. */
static void pick_name(void)
{
    int64_t now;
    if (!rtc_now(&now)) {
        strcpy(s_name, "undated.md");
        return;
    }
    const time_t tt = (time_t)now;
    struct tm tm;
    gmtime_r(&tt, &tm);
    snprintf(s_name, sizeof(s_name), "%04d-%02d-%02d.md",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
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
    edited();
}

static void erase(uint32_t from, uint32_t to)
{
    if (to > s_len) to = s_len;
    if (from >= to) return;
    memmove(s_text + from, s_text + to, s_len - to);
    s_len -= to - from;
    s_cur = from;
    edited();
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

/* ---- spelling -------------------------------------------------------- */

/* Lines of text shown above the word being checked. */
#define SPELL_ABOVE     (1)
#define SPELL_SCALE     (EDITOR_SCALE)
#define SPELL_LEGEND    "1-9 or Enter: change   Tab: skip   I: ignore all   A: add   Esc: done"

typedef struct {
    uint32_t     a, b;          /* the word, when there is one */
    const char  *msg;           /* or this, instead of a word */
    const char  *note;          /* beside the word: what last went wrong */
    spell_word_t sugg[SPELL_SUGG_MAX];
    int          n_sugg, sel;
} spell_view_t;

/* Suggestions that fit the pane. */
static int spell_fit(void)
{
    const int H = gfx_h();
    const int pane = MARGIN_Y + (s_rows / 2) * s_line_h + 8;
    const int row = GFX_GLYPH_H(SPELL_SCALE) + 3 * SPELL_SCALE;
    int n = (H - pane - 24 - row - 16 - GFX_GLYPH_H(2) - 16) / row;
    if (n > SPELL_SUGG_MAX) n = SPELL_SUGG_MAX;
    return n < 1 ? 1 : n;
}

static void spell_draw(const spell_view_t *v)
{
    layout();
    const int W = gfx_w();
    const int H = gfx_h();
    const int rows = s_rows / 2;
    const int pane = MARGIN_Y + rows * s_line_h + 8;
    const int row = GFX_GLYPH_H(SPELL_SCALE) + 3 * SPELL_SCALE;
    gfx_fill_rect(0, 0, W, H, COL_BG);

    /* Above: the text around the word, or around the cursor. */
    const bool word = !v->msg;
    int top = pos_line(word ? v->a : s_cur) - SPELL_ABOVE;
    if (top > s_lines - rows) top = s_lines - rows;
    if (top < 0) top = 0;
    draw_rows(top, rows, word ? v->a : 0, word ? v->b : 0);

    /* Below: the word and what it might be, or a message. */
    gfx_fill_rect(0, pane, W, H - pane, COL_HEAD);
    gfx_fill_rect(0, pane, W, 2, COL_RULE);
    int y = pane + 24;
    if (!word) {
        gfx_draw_text(MARGIN_X, y, v->msg, SPELL_SCALE, W - MARGIN_X, COL_TEXT);
        gfx_draw_text(MARGIN_X, y + row, "Any key to close.", 2, W - MARGIN_X, COL_DIM);
        gfx_blit(0, H);
        return;
    }
    char w[SPELL_WORD_MAX + 1];
    const uint32_t n = v->b - v->a;
    memcpy(w, s_text + v->a, n);
    w[n] = '\0';
    gfx_draw_text(MARGIN_X, y, w, SPELL_SCALE, W - MARGIN_X, COL_BAD);
    if (v->note) {
        gfx_draw_text(MARGIN_X + ((int)n + 2) * GFX_GLYPH_W(SPELL_SCALE),
                      y + GFX_GLYPH_H(SPELL_SCALE) - GFX_GLYPH_H(2), v->note, 2, W - MARGIN_X, COL_DIM);
    }
    y += row + 16;
    if (v->n_sugg == 0) {
        gfx_draw_text(MARGIN_X, y, "no suggestions", SPELL_SCALE, W - MARGIN_X, COL_DIM);
    }
    for (int i = 0; i < v->n_sugg; i++) {
        const int yy = y + i * row;
        if (i == v->sel) {
            gfx_fill_rect(MARGIN_X - 12, yy - SPELL_SCALE, W - 2 * MARGIN_X + 24,
                          GFX_GLYPH_H(SPELL_SCALE) + 2 * SPELL_SCALE, COL_BTN);
        }
        char line[SPELL_WORD_MAX + 8];
        snprintf(line, sizeof(line), "%d  %s", i + 1, v->sugg[i]);
        gfx_draw_text(MARGIN_X, yy, line, SPELL_SCALE, W - MARGIN_X, COL_TEXT);
    }
    gfx_draw_text(MARGIN_X, H - 16 - GFX_GLYPH_H(2), SPELL_LEGEND, 2, W - MARGIN_X, COL_DIM);
    gfx_blit(0, H);
}

/*
 * The next misspelling from `*at`, going round to the top once and
 * stopping at `stop`. False when there are no more, or no dictionary
 * (v->msg says which).
 */
static bool spell_find(spell_view_t *v, uint32_t *at, bool *wrapped, uint32_t stop, bool any)
{
    for (;;) {
        /* A card swapped mid-check unloads it; the new card's is used. */
        if (!spell_load()) {
            v->msg = "No dictionary: jinxed/spell/" SPELL_DICT " on the card.";
            return false;
        }
        uint32_t a, b;
        const bool got = spell_next_word(s_text, s_len, *at, &a, &b);
        if (!got || (*wrapped && a >= stop)) {
            if (got || *wrapped) {
                v->msg = any ? "Spelling checked." : "No misspellings found.";
                return false;
            }
            *wrapped = true;
            *at = 0;
            continue;
        }
        *at = b;
        if (spell_known(s_text + a, b - a)) continue;
        v->a = a;
        v->b = b;
        v->msg = NULL;
        v->note = NULL;
        v->sel = 0;
        v->n_sugg = spell_suggest(s_text + a, b - a, v->sugg, spell_fit());
        for (int i = 0; i < v->n_sugg; i++) spell_case_like(s_text + a, b - a, v->sugg[i]);
        return true;
    }
}

/* The check, from the word at the cursor round to it again. Runs on the
 * editor's task, as settings_page() does, until closed. */
static void spell_view(void)
{
    static spell_view_t v;          /* the suggestions are 400 bytes */
    memset(&v, 0, sizeof(v));
    uint32_t a0, b0;
    uint32_t stop = spell_next_word(s_text, s_len, s_cur, &a0, &b0) ? a0 : s_len;
    uint32_t at = s_cur;
    bool wrapped = false, any = false;

    bool open = spell_find(&v, &at, &wrapped, stop, any);
    if (open) { s_cur = v.a; any = true; }
    spell_draw(&v);

    for (;;) {
        kbd_event_t ev;
        if (!kbd_get(&ev, pdMS_TO_TICKS(250))) {
            save_tick(false);           /* changes are saved as typing is */
            continue;
        }
        if (!ev.down) continue;
        if (!open) break;               /* the message: any key closes */

        const bool gui = (ev.mods & (KBD_MOD_GUI | KBD_MOD_CTRL)) != 0;
        const bool plain = (ev.mods & (KBD_MOD_GUI | KBD_MOD_CTRL | KBD_MOD_ALT)) == 0;
        int pick = -1;
        bool next = false;
        switch (ev.usage) {
        case KBD_ESCAPE:
        case KBD_F7:        goto out;
        case KBD_SEMICOLON: if (gui) goto out; break;
        case KBD_UP:        if (v.sel > 0) v.sel--; break;
        case KBD_DOWN:      if (v.sel + 1 < v.n_sugg) v.sel++; break;
        case KBD_ENTER:
        case KBD_KP_ENTER:  if (v.n_sugg) pick = v.sel; else next = true; break;
        case KBD_TAB:
        case KBD_SPACE:
        case KBD_RIGHT:     next = true; break;
        case KBD_I:
            if (plain) { spell_ignore(s_text + v.a, v.b - v.a); next = true; }
            break;
        case KBD_A:
            if (plain) {
                if (spell_add(s_text + v.a, v.b - v.a)) next = true;
                else v.note = "not saved to the card";
            }
            break;
        default:
            if (plain && ev.usage >= KBD_1 && ev.usage < KBD_1 + v.n_sugg) pick = ev.usage - KBD_1;
            break;
        }

        if (pick >= 0) {
            const uint32_t n = (uint32_t)strlen(v.sugg[pick]);
            const uint32_t old = v.b - v.a;
            if (s_len - old + n <= TEXT_MAX) {
                save_tick(true);        /* the user's edits, as theirs */
                s_edit_src = DOCLOG_SRC_SYMSPELL;
                erase(v.a, v.b);        /* the cursor goes to a */
                insert(v.sugg[pick], n);
                s_edit_src = DOCLOG_SRC_USER;
                save_tick(true);        /* the correction, on its own */
                if (v.a < stop) stop = stop - old + n;
                at = v.a + n;
                next = true;
            } else {
                v.note = "the text is full";
            }
        }
        if (next) {
            open = spell_find(&v, &at, &wrapped, stop, any);
            if (open) s_cur = v.a;
        }
        spell_draw(&v);
    }
out:
    s_want_col = -1;
}

/* ---- the loop --------------------------------------------------------- */

esp_err_t editor_init(void)
{
    s_text    = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    s_ls      = heap_caps_malloc((TEXT_MAX + 2) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_le      = heap_caps_malloc((TEXT_MAX + 2) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_base    = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    if (!s_text || !s_ls || !s_le || !s_base) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(doclog_init(TEXT_MAX), TAG, "doclog");

    const int W = gfx_w();
    const int H = gfx_h();
    s_cols   = (W - 2 * MARGIN_X) / GFX_GLYPH_W(EDITOR_SCALE);
    s_line_h = GFX_GLYPH_H(EDITOR_SCALE) + 4 * EDITOR_SCALE;
    s_status_y = H - MARGIN_Y / 2 - GFX_GLYPH_H(2);
    s_rows   = (s_status_y - MARGIN_Y - 8) / s_line_h;
    s_row_buf = heap_caps_malloc((size_t)s_cols + 2, MALLOC_CAP_DEFAULT);
    if (!s_row_buf) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "%d columns, %d rows, scale %d", s_cols, s_rows, EDITOR_SCALE);
    pick_name();
    attach();
    redraw();
    return ESP_OK;
}

void editor_run(void)
{
    uint8_t  held = 0, held_mods = 0;   /* the key repeating, if any */
    TickType_t next_repeat = 0;
    bool had_kbd = kbd_present();
    bool had_card = storage_present(STORAGE_SD);
    bool touch_down = false;
    int  touch_x = 0, touch_y = 0;

    for (;;) {
        TickType_t wait = pdMS_TO_TICKS(500);
        if (held) {
            const TickType_t now = xTaskGetTickCount();
            wait = (next_repeat > now) ? next_repeat - now : 0;
        }
        if (s_pending && wait > pdMS_TO_TICKS(250)) wait = pdMS_TO_TICKS(250);
        /* The gear needs the panel polled; a key still wakes us at once. */
        if (touch_present() && wait > pdMS_TO_TICKS(30)) wait = pdMS_TO_TICKS(30);

        bool open_settings = false;
        bool open_spell = false;

        kbd_event_t ev;
        bool dirty = false;
        if (kbd_get(&ev, wait)) {
            if (ev.down && ev.usage == KBD_COMMA && (ev.mods & KBD_MOD_GUI)) {
                open_settings = true;       /* spec 7: from the keyboard too */
            } else if (ev.down && (ev.usage == KBD_F7 || (ev.usage == KBD_SEMICOLON &&
                                   (ev.mods & (KBD_MOD_GUI | KBD_MOD_CTRL))))) {
                open_spell = true;          /* spec 5: Word's F7, the Mac's Cmd+; */
            } else if (ev.down) {
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

        /* A tap on the gear: down and up, both on it. */
        int tx, ty;
        const bool t_now = touch_get(&tx, &ty);
        if (t_now && !touch_down) { touch_x = tx; touch_y = ty; }
        if (!t_now && touch_down && settings_gear_hit(touch_x, touch_y)) open_settings = true;
        touch_down = t_now;

        if (open_settings) {
            held = 0;
            save_tick(true);            /* the card is current before we leave */
            settings_page();
            touch_down = false;
            dirty = true;
        }

        if (open_spell) {
            held = 0;
            spell_view();
            touch_down = false;
            dirty = true;
        }

        /* Plugging or unplugging shows in the status line. */
        const bool has_kbd = kbd_present();
        if (has_kbd != had_kbd) { had_kbd = has_kbd; dirty = true; }
        const bool has_card = storage_present(STORAGE_SD);
        if (has_card != had_card) { had_card = has_card; dirty = true; }

        /* Not while a key is held: the repeat is still typing. */
        if (!held && save_tick(false)) dirty = true;

        if (dirty) redraw();
    }
}
