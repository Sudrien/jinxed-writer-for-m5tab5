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
 * Spelling (spell.h) is checked only when asked -- F7, or Command+;
 * on an Apple keyboard: the screen splits, the text above with the word marked, the word and
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
#include "sender.h"
#include "settings.h"
#include "spell.h"
#include "storage.h"
#include "touch.h"

static const char *TAG = "editor";

/* Text scale over the 12px font: display_text_scale(), 2 to 5 (0022),
 * taken by measure(). 3 is 36px glyphs, 58 columns across the landscape
 * panel. FOOT_SCALE is the status line's and the spelling view's small
 * print, a step smaller. */
static int       s_scale = DISPLAY_SCALE_DEFAULT, s_foot = DISPLAY_SCALE_MIN;
#define EDITOR_SCALE    (s_scale)
#define FOOT_SCALE      (s_foot)
static void measure(void);

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
    gfx_draw_text(W - settings_gear_w() - gfx_text_w(st, FOOT_SCALE), s_status_y, st, FOOT_SCALE, W, COL_DIM);
    settings_draw_gear();
    if (!kbd_present()) gfx_draw_text(MARGIN_X, s_status_y, "no keyboard", FOOT_SCALE, W, COL_DIM);
    if (s_caps)        gfx_draw_text(MARGIN_X + 15 * GFX_GLYPH_W(FOOT_SCALE), s_status_y, "caps", FOOT_SCALE, W, COL_DIM);
    if (!storage_present(STORAGE_SD)) {
        gfx_draw_text(MARGIN_X + 21 * GFX_GLYPH_W(FOOT_SCALE), s_status_y, "no card", FOOT_SCALE, W, COL_DIM);
    } else if (s_save_failed) {
        gfx_draw_text(MARGIN_X + 21 * GFX_GLYPH_W(FOOT_SCALE), s_status_y, "not saved", FOOT_SCALE, W, COL_DIM);
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

/* ---- words and paragraphs (0027) ------------------------------------- */

static bool wordc(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '\'' || c == '_' || (unsigned char)c >= 0x80;
}

/* The start of this word, or of the one before if already there. */
static uint32_t word_back(uint32_t p)
{
    while (p > 0 && !wordc(s_text[p - 1])) p--;
    while (p > 0 && wordc(s_text[p - 1])) p--;
    return p;
}

/* Word's Ctrl+Right: the start of the next word -- past this one (or
 * this run of punctuation) and the spaces after it. */
static uint32_t word_next_start(uint32_t p)
{
    if (p < s_len && wordc(s_text[p])) {
        while (p < s_len && wordc(s_text[p])) p++;
    } else {
        while (p < s_len && !wordc(s_text[p]) && s_text[p] != ' ' && s_text[p] != '\n') p++;
    }
    while (p < s_len && s_text[p] == ' ') p++;
    if (p < s_len && s_text[p] == '\n' && p == s_cur) p++;   /* across a line end */
    return p;
}

/* The Mac's Option+Right: the end of this word, or the next. */
static uint32_t word_next_end(uint32_t p)
{
    while (p < s_len && !wordc(s_text[p])) p++;
    while (p < s_len && wordc(s_text[p])) p++;
    return p;
}

/* The start of this paragraph (text after a newline), or the one
 * before if already there. */
static uint32_t para_back(uint32_t p)
{
    if (p > 0 && s_text[p - 1] == '\n') p--;
    while (p > 0 && s_text[p - 1] != '\n') p--;
    return p;
}

/* Word's Ctrl+Down: the start of the next paragraph, or the end. */
static uint32_t para_next_start(uint32_t p)
{
    while (p < s_len && s_text[p] != '\n') p++;
    return p < s_len ? p + 1 : p;
}

/* The Mac's Option+Down: the end of this paragraph, or the next. */
static uint32_t para_next_end(uint32_t p)
{
    if (p < s_len && s_text[p] == '\n') p++;
    while (p < s_len && s_text[p] != '\n') p++;
    return p;
}

/*
 * One key press (or repeat). True when the screen needs redrawing.
 *
 * The keys follow the keyboard (0027, spec 5). On an Apple keyboard,
 * the Mac's: Command with the arrows for the line and the document,
 * Option for words and paragraphs, and Ctrl a shortcut for nothing. On
 * any other, Word's: Ctrl with the arrows for words and paragraphs,
 * Ctrl+Home / End for the document. There the Windows key and Alt are
 * shortcuts for nothing (0028), as Notepad++ leaves Alt+arrows alone.
 */
static bool key(const kbd_event_t *ev)
{
    const uint8_t usage = ev->usage, mods = ev->mods;
    const bool apple = ev->apple;
    const bool cmd   = ev->cmd;
    const bool alt   = (mods & KBD_MOD_ALT)   != 0;
    const bool shift = (mods & KBD_MOD_SHIFT) != 0;
    const bool keep_col = usage == KBD_UP || usage == KBD_DOWN ||
                          usage == KBD_PAGEUP || usage == KBD_PAGEDOWN;
    if (!keep_col) s_want_col = -1;

    layout();
    const int cl = cursor_line();

    /* Text larger / smaller (0022), number row or keypad. */
    if (cmd && (usage == KBD_EQUAL || usage == KBD_KP_PLUS ||
                usage == KBD_MINUS || usage == KBD_KP_MINUS)) {
        const int up = usage == KBD_EQUAL || usage == KBD_KP_PLUS;
        const int was = display_text_scale();
        display_set_text_scale(was + (up ? 1 : -1));
        ESP_LOGI(TAG, "text size %d -> %d", was, display_text_scale());
        if (display_text_scale() == was) return false;
        measure();                      /* the redraw rewraps and finds the cursor */
        return true;
    }

    /* The whole visual line, and its newline if it has one, on a PC:
     * Notepad++'s Ctrl+L, or VS Code's Ctrl+Shift+K (0028). On a Mac,
     * Command+Delete deletes to the line's start, as macOS does. */
    if (!apple && cmd && (usage == KBD_L || (shift && usage == KBD_K))) {
        erase(s_ls[cl], line_next(cl));
        return true;
    }
    if (apple && cmd && usage == KBD_BACKSPACE) {
        erase(s_ls[cl], s_cur);
        return true;
    }

    if (apple && cmd) {                 /* Command */
        switch (usage) {
        case KBD_LEFT:      s_cur = s_ls[cl]; return true;
        case KBD_RIGHT:     s_cur = line_cursor_max(cl); return true;
        case KBD_UP:        s_cur = 0; return true;
        case KBD_DOWN:      s_cur = s_len; return true;
        default:            return false;
        }
    }
    if (apple && alt) {                 /* Option */
        switch (usage) {
        case KBD_LEFT:      s_cur = word_back(s_cur); return true;
        case KBD_RIGHT:     s_cur = word_next_end(s_cur); return true;
        case KBD_UP:        s_cur = para_back(s_cur); return true;
        case KBD_DOWN:      s_cur = para_next_end(s_cur); return true;
        case KBD_BACKSPACE: erase(word_back(s_cur), s_cur); return true;
        case KBD_DELETE:    erase(s_cur, word_next_end(s_cur)); return true;
        default:            return false;
        }
    }
    if (!apple && cmd) {                /* Ctrl */
        switch (usage) {
        case KBD_LEFT:      s_cur = word_back(s_cur); return true;
        case KBD_RIGHT:     s_cur = word_next_start(s_cur); return true;
        case KBD_UP:        s_cur = para_back(s_cur); return true;
        case KBD_DOWN:      s_cur = para_next_start(s_cur); return true;
        case KBD_HOME:      s_cur = 0; return true;
        case KBD_END:       s_cur = s_len; return true;
        case KBD_BACKSPACE: erase(word_back(s_cur), s_cur); return true;
        case KBD_DELETE:    erase(s_cur, word_next_start(s_cur)); return true;
        default:            return false;
        }
    }
    /* Ctrl on an Apple keyboard, the Windows key and Alt on a PC one, and
     * anything else held with Command or Ctrl: no shortcut, no movement,
     * and it types nothing. */
    if (mods & (KBD_MOD_CTRL | KBD_MOD_GUI | KBD_MOD_ALT)) return false;

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
/* The keys, in pieces a line may break between (0019): one line wide,
 * two or three on end. */
static const char *const k_legend[] = {
    "1-9 Enter: change", "E: type", "Tab: skip", "I: ignore all", "A: add", "Esc: done",
};
static const char *const k_type_legend[] = { "Enter: change to this", "Esc: back to the suggestions" };
static const char *const k_dict_hint[] = { "D: download it (15 MB, over Wi-Fi)", "Any other key: close" };
#define LEGEND_N(a)     ((int)(sizeof(a) / sizeof((a)[0])))
#define LEGEND_GAP      "   "
#define LEGEND_LH       (GFX_GLYPH_H(FOOT_SCALE) + 8)
/* What a word may be typed as: a few words, if it was one run together. */
#define SPELL_TYPE_MAX  (64)

typedef struct {
    uint32_t     a, b;          /* the word, when there is one */
    const char  *msg;           /* or this, instead of a word */
    const char  *note;          /* beside the word: what last went wrong */
    bool         no_dict;       /* msg is "no dictionary": D fetches one */
    bool         fetching;      /* and the send task is fetching it */
    char         status[160];   /* the send task's line, under msg */
    spell_word_t sugg[SPELL_SUGG_MAX];
    int          n_sugg, sel;
    /* E: the word typed by hand, when none of the suggestions is it. */
    bool         typing;
    char         typed[SPELL_TYPE_MAX + 1];
    int          tlen, tcur;
} spell_view_t;

/* Lay pieces out in lines no wider than the screen's margins allow;
 * draw them from `y` down when `draw`. The number of lines. */
static int legend(const char *const *part, int n, int y, bool draw)
{
    const int room = gfx_w() - 2 * MARGIN_X;
    const int gw = GFX_GLYPH_W(FOOT_SCALE);
    char line[160];
    int lines = 0;
    for (int i = 0; i < n; ) {
        size_t len = 0;
        line[0] = '\0';
        while (i < n) {
            const size_t add = strlen(part[i]) + (len ? strlen(LEGEND_GAP) : 0);
            if (len && (int)(len + add) * gw > room) break;
            if (len + add >= sizeof(line)) break;
            if (len) strcat(line, LEGEND_GAP);
            strcat(line, part[i++]);
            len = strlen(line);
        }
        if (draw) gfx_draw_text(MARGIN_X, y + lines * LEGEND_LH, line, FOOT_SCALE, room, COL_DIM);
        lines++;
    }
    return lines;
}

/* The same, at the foot of the pane. */
static void legend_foot(const char *const *part, int n)
{
    const int lines = legend(part, n, 0, false);
    legend(part, n, gfx_h() - 16 - lines * LEGEND_LH + 8, true);
}

/* Suggestions that fit the pane, above however many lines the legend
 * takes at this width. */
static int spell_fit(void)
{
    const int H = gfx_h();
    const int pane = MARGIN_Y + (s_rows / 2) * s_line_h + 8;
    const int row = GFX_GLYPH_H(SPELL_SCALE) + 3 * SPELL_SCALE;
    const int foot = legend(k_legend, LEGEND_N(k_legend), 0, false) * LEGEND_LH;
    int n = (H - pane - 24 - row - 16 - foot - 16) / row;
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
        int hy = y + row;
        if (v->no_dict && v->status[0]) {
            /* The send task's line, in up to two lines: a reason can be
             * long, and the screen may be on end (0019). */
            const char *st = v->status;
            for (int l = 0; l < 2 && *st; l++) {
                const char *next = st;
                size_t n = gfx_wrap_line(st, FOOT_SCALE, W - 2 * MARGIN_X, &next);
                char line[sizeof(v->status)];
                if (l == 1 || n >= sizeof(line)) n = strlen(st) < sizeof(line) ? strlen(st) : sizeof(line) - 1;
                memcpy(line, st, n);
                line[n] = '\0';
                gfx_draw_text(MARGIN_X, hy, line, FOOT_SCALE, W - 2 * MARGIN_X, COL_TEXT);
                hy += LEGEND_LH;
                if (next == st) break;
                st = next;
            }
        }
        hy += row - LEGEND_LH;
        static const char *const close1[] = { "Any key to close." };
        static const char *const closef[] = { "Any key to close;", "the download goes on." };
        if (!v->no_dict)     legend(close1, 1, hy, true);
        else if (v->fetching) legend(closef, 2, hy, true);
        else                 legend(k_dict_hint, LEGEND_N(k_dict_hint), hy, true);
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
                      y + GFX_GLYPH_H(SPELL_SCALE) - GFX_GLYPH_H(FOOT_SCALE), v->note, FOOT_SCALE, W - MARGIN_X, COL_DIM);
    }
    y += row + 16;
    if (v->typing) {
        /* The field: the text, and a bar where the next letter goes. */
        const int gw = GFX_GLYPH_W(SPELL_SCALE);
        gfx_draw_text(MARGIN_X, y, "Type it as it should be:", FOOT_SCALE, W - MARGIN_X, COL_DIM);
        const int fy = y + GFX_GLYPH_H(FOOT_SCALE) + 12;
        gfx_fill_rect(MARGIN_X - 12, fy - 2 * SPELL_SCALE, W - 2 * MARGIN_X + 24,
                      GFX_GLYPH_H(SPELL_SCALE) + 4 * SPELL_SCALE, COL_FIELD);
        /* Wider than the field: the part around the cursor (0019). */
        const int fit = (W - 2 * MARGIN_X) / gw - 1;
        const int from = v->tcur > fit ? v->tcur - fit : 0;
        char shown[SPELL_TYPE_MAX + 1];
        snprintf(shown, sizeof(shown), "%.*s", fit + 1, v->typed + from);
        gfx_draw_text(MARGIN_X, fy, shown, SPELL_SCALE, W - 2 * MARGIN_X, COL_TEXT);
        gfx_fill_rect(MARGIN_X + (v->tcur - from) * gw, fy - SPELL_SCALE, 2 * SPELL_SCALE,
                      GFX_GLYPH_H(SPELL_SCALE) + 2 * SPELL_SCALE, COL_CURSOR);
        legend_foot(k_type_legend, LEGEND_N(k_type_legend));
        gfx_blit(0, H);
        return;
    }
    if (v->n_sugg == 0) {
        gfx_draw_text(MARGIN_X, y, "no suggestions -- E to type it", SPELL_SCALE, W - MARGIN_X, COL_DIM);
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
    legend_foot(k_legend, LEGEND_N(k_legend));
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
            v->msg = "No dictionary on the card.";
            v->no_dict = true;
            /* A fetch already running, from an earlier view. */
            v->fetching = sender_status(v->status, sizeof(v->status));
            if (!v->fetching) v->status[0] = '\0';     /* not an old send's */
            return false;
        }
        v->no_dict = false;
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
        v->typing = false;
        v->n_sugg = spell_suggest(s_text + a, b - a, v->sugg, spell_fit());
        for (int i = 0; i < v->n_sugg; i++) spell_case_like(s_text + a, b - a, v->sugg[i]);
        return true;
    }
}

/*
 * Put `text` where the word is. A suggestion taken (SYMSPELL) is logged
 * on its own and as the checker's: what is pending is flushed first, and
 * it straight after. A word typed by hand (USER) is the user's, an edit
 * like any other. False, with a note, when it will not fit.
 */
static bool spell_replace(spell_view_t *v, const char *text, uint32_t n, doclog_src_t src,
                          uint32_t *stop, uint32_t *at)
{
    const uint32_t old = v->b - v->a;
    if (s_len - old + n > TEXT_MAX) {
        v->note = "the text is full";
        return false;
    }
    if (src == DOCLOG_SRC_SYMSPELL) save_tick(true);    /* the user's edits, as theirs */
    s_edit_src = src;
    erase(v->a, v->b);              /* the cursor goes to a */
    insert(text, n);
    s_edit_src = DOCLOG_SRC_USER;
    if (src == DOCLOG_SRC_SYMSPELL) save_tick(true);    /* the correction, on its own */
    if (v->a < *stop) *stop = *stop - old + n;
    *at = v->a + n;
    return true;
}

/* A key in the typing field. 1: take what is typed; -1: back to the
 * suggestions; 0: carry on typing. */
static int spell_type_key(spell_view_t *v, const kbd_event_t *ev)
{
    switch (ev->usage) {
    case KBD_ENTER:
    case KBD_KP_ENTER:  return 1;
    case KBD_ESCAPE:    return -1;
    case KBD_CAPSLOCK:  s_caps = !s_caps; return 0;
    case KBD_LEFT:      if (v->tcur > 0) v->tcur--; return 0;
    case KBD_RIGHT:     if (v->tcur < v->tlen) v->tcur++; return 0;
    case KBD_HOME:      v->tcur = 0; return 0;
    case KBD_END:       v->tcur = v->tlen; return 0;
    case KBD_BACKSPACE:
        if (v->tcur > 0) {
            memmove(v->typed + v->tcur - 1, v->typed + v->tcur, (size_t)(v->tlen - v->tcur + 1));
            v->tcur--;
            v->tlen--;
        }
        return 0;
    case KBD_DELETE:
        if (v->tcur < v->tlen) {
            memmove(v->typed + v->tcur, v->typed + v->tcur + 1, (size_t)(v->tlen - v->tcur));
            v->tlen--;
        }
        return 0;
    default: break;
    }
    if (ev->mods & (KBD_MOD_GUI | KBD_MOD_CTRL | KBD_MOD_ALT)) return 0;
    const char c = kbd_ascii(ev->usage, ev->mods, s_caps);
    /* One line: a word, or a few. */
    if (c < ' ' || c > '~' || v->tlen >= SPELL_TYPE_MAX) return 0;
    memmove(v->typed + v->tcur + 1, v->typed + v->tcur, (size_t)(v->tlen - v->tcur + 1));
    v->typed[v->tcur++] = c;
    v->tlen++;
    return 0;
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
    uint32_t seen = sender_gen();

    for (;;) {
        kbd_event_t ev;
        if (!kbd_get(&ev, pdMS_TO_TICKS(250))) {
            save_tick(false);           /* changes are saved as typing is */
            /* A fetch's progress, and the check once it lands. */
            if (v.no_dict && sender_gen() != seen) {
                seen = sender_gen();
                const bool busy = sender_status(v.status, sizeof(v.status));
                if (v.fetching && !busy) {
                    v.fetching = false;
                    if (sender_fetched()) {
                        open = spell_find(&v, &at, &wrapped, stop, any);
                        if (open) { s_cur = v.a; any = true; }
                    }
                }
                spell_draw(&v);
            }
            continue;
        }
        if (!ev.down) continue;
        if (!open && v.no_dict && !v.fetching && ev.usage == KBD_D &&
            !(ev.mods & (KBD_MOD_GUI | KBD_MOD_CTRL | KBD_MOD_ALT))) {
            if (sender_fetch_dict()) {
                v.fetching = true;
            } else {
                snprintf(v.status, sizeof(v.status), "Busy sending; try again in a moment.");
            }
            seen = sender_gen();
            spell_draw(&v);
            continue;
        }
        if (!open) break;               /* the message: any other key closes */

        if (v.typing) {
            const int r = spell_type_key(&v, &ev);
            bool moved = false;
            if (r < 0) {
                v.typing = false;
            } else if (r > 0) {
                v.typing = false;
                const uint32_t old = v.b - v.a;
                if (v.tlen == 0) {
                    v.note = "nothing typed";
                } else if ((uint32_t)v.tlen == old && !memcmp(v.typed, s_text + v.a, old)) {
                    moved = true;       /* unchanged: as a skip */
                } else {
                    moved = spell_replace(&v, v.typed, (uint32_t)v.tlen, DOCLOG_SRC_USER, &stop, &at);
                }
            }
            if (moved) {
                open = spell_find(&v, &at, &wrapped, stop, any);
                if (open) s_cur = v.a;
            }
            spell_draw(&v);
            continue;
        }

        const bool gui = ev.cmd && ev.apple;     /* Command+; closes; F7 on a PC (0028) */
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
        case KBD_E:
            if (plain) {
                /* Typed by hand, starting from the word: it is usually a
                 * letter or two from right. */
                const uint32_t n = v.b - v.a;
                memcpy(v.typed, s_text + v.a, n);
                v.typed[n] = '\0';
                v.tlen = v.tcur = (int)n;
                v.typing = true;
                v.note = NULL;
            }
            break;
        default:
            if (plain && ev.usage >= KBD_1 && ev.usage < KBD_1 + v.n_sugg) pick = ev.usage - KBD_1;
            break;
        }

        if (pick >= 0 &&
            spell_replace(&v, v.sugg[pick], (uint32_t)strlen(v.sugg[pick]), DOCLOG_SRC_SYMSPELL, &stop, &at)) {
            next = true;
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

/* Columns and rows for the screen as it is now: at start, and after the
 * settings page, which may have turned it (0019). Lines rewrap at the
 * next redraw, which also scrolls the cursor back into view. */
static void measure(void)
{
    const int W = gfx_w();
    const int H = gfx_h();
    s_scale  = display_text_scale();
    s_foot   = display_foot_scale();
    s_cols   = (W - 2 * MARGIN_X) / GFX_GLYPH_W(EDITOR_SCALE);
    s_line_h = GFX_GLYPH_H(EDITOR_SCALE) + 4 * EDITOR_SCALE;
    s_status_y = H - MARGIN_Y / 2 - GFX_GLYPH_H(FOOT_SCALE);
    s_rows   = (s_status_y - MARGIN_Y - 8) / s_line_h;
    ESP_LOGI(TAG, "%d columns, %d rows, scale %d", s_cols, s_rows, EDITOR_SCALE);
}

esp_err_t editor_init(void)
{
    s_text    = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    s_ls      = heap_caps_malloc((TEXT_MAX + 2) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_le      = heap_caps_malloc((TEXT_MAX + 2) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_base    = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    if (!s_text || !s_ls || !s_le || !s_base) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(doclog_init(TEXT_MAX), TAG, "doclog");

    /* A row is drawn from here: as wide as the wider way up at the
     * smallest text, so neither turning the screen nor a text size
     * (0022) ever reallocates it. */
    const int wide = gfx_w() > gfx_h() ? gfx_w() : gfx_h();
    s_row_buf = heap_caps_malloc((size_t)((wide - 2 * MARGIN_X) / GFX_GLYPH_W(DISPLAY_SCALE_MIN)) + 2,
                                 MALLOC_CAP_DEFAULT);
    if (!s_row_buf) return ESP_ERR_NO_MEM;
    measure();
    pick_name();
    attach();
    redraw();
    return ESP_OK;
}

void editor_run(void)
{
    uint8_t  held = 0;                  /* the key repeating, if any */
    kbd_event_t held_ev = { 0 };
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
            if (ev.down && ev.usage == KBD_COMMA && ev.cmd) {
                open_settings = true;       /* spec 7: from the keyboard too */
            } else if (ev.down && (ev.usage == KBD_F7 || (ev.usage == KBD_SEMICOLON && ev.cmd && ev.apple))) {
                open_spell = true;          /* spec 5: Word's F7, the Mac's Cmd+; */
            } else if (ev.down) {
                dirty = key(&ev);
                if (repeats(ev.usage)) {
                    held = ev.usage;
                    held_ev = ev;
                    next_repeat = xTaskGetTickCount() + pdMS_TO_TICKS(REPEAT_DELAY_MS);
                }
            } else if (ev.usage == held) {
                held = 0;
            }
        } else if (held) {
            dirty = key(&held_ev);
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
            measure();                  /* it may have turned the screen */
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
