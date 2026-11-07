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
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "md.h"
#include "doclog.h"
#include "files.h"
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
/* 0036: export this long after the last keystroke, once the log has
 * what is to be exported. */
#define EXPORT_PAUSE_MS (5000)
/* After a failed open, wait this long before trying the card again. */
#define ATTACH_RETRY_MS (5000)

static char     *s_text;        /* TEXT_MAX bytes, PSRAM */
static uint32_t  s_len;
static uint32_t  s_cur;
static bool      s_caps;
static int       s_want_col = -1;   /* up/down keep their column */
/* 0033: the selection is [min, max) of the anchor and the cursor, or
 * none. Any edit ends it (edited()). The clipboard is RAM only, kept
 * until power-off, PSRAM; without it there is no copy or paste. */
#define NO_ANCHOR       (UINT32_MAX)
static uint32_t  s_anchor = NO_ANCHOR;
static char     *s_clip;        /* TEXT_MAX bytes, PSRAM */
static uint32_t  s_clip_len;

/* Layout, rebuilt by layout(). Line i is s_text[s_ls[i], s_le[i]) on
 * screen, and the next line starts at s_ls[i + 1]. */
static uint32_t *s_ls;
static uint32_t *s_le;
static int       s_lines;
static int       s_top;         /* first line on screen */

static int       s_cols, s_rows, s_line_h, s_status_y;
static int       s_cols_head;   /* 0032: a heading's columns, heavy cells */
static uint8_t  *s_mds;         /* 0031: markdown styles, TEXT_MAX bytes, PSRAM */
/* F11's bare page (0029): no word count and no gear. The status line's
 * row is given to the text unless something there must be seen --
 * no keyboard, caps, no card, not saved -- which brings it back. Until
 * power-off. */
static bool      s_bare, s_foot_on = true;
static char     *s_row_buf;     /* one line, NUL-terminated, for gfx */

/* Saving. See the top of the file. */
static char     *s_base;        /* TEXT_MAX bytes, PSRAM */
static uint32_t  s_base_len;
static char      s_name[FILES_NAME_MAX + 1];    /* "2026-11-02.md"; 0044: or file mode's */
static bool      s_pending;     /* s_text may differ from s_base */
static doclog_src_t s_src;      /* who made what is pending */
static doclog_src_t s_edit_src = DOCLOG_SRC_USER;  /* who is editing now */
static TickType_t s_last_edit, s_first_edit;
static bool      s_save_failed;
static bool      s_export_due;  /* 0036: the log has more than the export */
static bool      s_conflict;    /* the export was edited outside */
/* 0038: a send of ours, and its line in the status line until
 * SEND_SHOW_MS after it ends, or the next key. */
#define SEND_SHOW_MS    (10000)
static bool      s_sending;
static char      s_send_line[160];  /* sender_status()'s line, whole */
static TickType_t s_send_until;
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
/* 0032: is the logical line holding text position `at` a markdown
 * heading? Its rows are drawn heavy, in GFX_HEAVY_W cells, and wrap at
 * s_cols_head. */
static bool head_line(uint32_t at)
{
    if (!display_markdown() || !s_mds) return false;
    uint32_t a = at;
    while (a > 0 && s_text[a - 1] != '\n') a--;
    uint32_t b = a;
    while (b < s_len && s_text[b] != '\n' && b - a < 16) b++;   /* "   ###### " fits */
    return md_is_heading(s_text + a, b - a);
}

/* A row's cell width: heavy on a heading (0032). */
static int row_gw(int l)
{
    return head_line(s_ls[l]) ? GFX_HEAVY_W(EDITOR_SCALE) : GFX_GLYPH_W(EDITOR_SCALE);
}

static void line_from(uint32_t s, int cols, uint32_t *end, uint32_t *next)
{
    uint32_t last_space = UINT32_MAX;
    for (uint32_t i = s; i < s_len; i++) {
        const char c = s_text[i];
        if (c == '\n') { *end = i; *next = i + 1; return; }
        if (i - s == (uint32_t)cols) {
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
    bool head = false;
    for (;;) {
        uint32_t e, n;
        if (s == 0 || s_text[s - 1] == '\n') head = head_line(s);
        line_from(s, head ? s_cols_head : s_cols, &e, &n);
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

/* The selection, [a, b), or false when there is none. */
static bool sel(uint32_t *a, uint32_t *b)
{
    if (s_anchor == NO_ANCHOR || s_anchor > s_len || s_anchor == s_cur) return false;
    *a = s_anchor < s_cur ? s_anchor : s_cur;
    *b = s_anchor < s_cur ? s_cur : s_anchor;
    return true;
}

/* `rows` lines of text from line `top`, with s_text[hl_a, hl_b) marked
 * behind. After layout(). */
/*
 * 0031: markdown. Each logical line's bytes get a style (md.h), kept by
 * position in s_mds and worked out once per draw for the line a row is
 * in. A row with no style is drawn as it always was; one with styles in
 * runs -- each run where the whole row's gfx_draw_text() would have put
 * it, so the columns do not move: code's background first, then the
 * spelling mark, then the text, markers dim, then strikes.
 */
static uint32_t  s_md_from;     /* the logical line in s_mds, or UINT32_MAX */

static void md_for(uint32_t at)
{
    uint32_t a = at;
    while (a > 0 && s_text[a - 1] != '\n') a--;
    if (a == s_md_from) return;
    uint32_t b = at;
    while (b < s_len && s_text[b] != '\n') b++;
    md_line(s_text + a, b - a, s_mds + a);
    s_md_from = a;
}

static unsigned gfx_style(uint8_t m)
{
    return ((m & MD_HEAD) ? GFX_STYLE_HEAVY : (m & MD_BOLD) ? GFX_STYLE_BOLD : 0) |
           ((m & MD_ITALIC) ? GFX_STYLE_ITALIC : 0);
}

/* One pass over a row's runs: what each run is, and where it starts. */
typedef void (*run_fn_t)(int x, int y, const char *s, int w, uint8_t m);

static void row_runs(uint32_t rs, uint32_t re, int y, run_fn_t fn)
{
    int x = MARGIN_X;
    uint32_t i = rs;
    while (i < re) {
        const uint8_t m = s_mds[i];
        uint32_t j = i + 1;
        while (j < re && s_mds[j] == m) j++;
        memcpy(s_row_buf, s_text + i, j - i);
        s_row_buf[j - i] = '\0';
        const int w = gfx_text_w_style(s_row_buf, EDITOR_SCALE, gfx_style(m));
        fn(x, y, s_row_buf, w, m);
        x += w;
        i = j;
    }
}

static void run_bg(int x, int y, const char *s, int w, uint8_t m)
{
    (void)s;
    if (m & MD_CODE)
        gfx_fill_rect(x, y - EDITOR_SCALE, w, GFX_GLYPH_H(EDITOR_SCALE) + 2 * EDITOR_SCALE, COL_FIELD);
}

static void run_text(int x, int y, const char *s, int w, uint8_t m)
{
    (void)w;
    gfx_draw_text_style(x, y, s, EDITOR_SCALE, gfx_w() - x, (m & MD_MARK) ? COL_DIM : COL_TEXT,
                        gfx_style(m));
}

static void run_strike(int x, int y, const char *s, int w, uint8_t m)
{
    (void)s;
    if ((m & MD_STRIKE) && !(m & MD_MARK))
        gfx_fill_rect(x, y + GFX_STRIKE_Y(EDITOR_SCALE), w, GFX_STRIKE_H(EDITOR_SCALE), COL_TEXT);
}

static void draw_rows(int top, int rows, uint32_t hl_a, uint32_t hl_b)
{
    const int W = gfx_w();
    const bool md = display_markdown() && s_mds;
    s_md_from = UINT32_MAX;             /* the text may have changed */
    for (int r = 0; r < rows && top + r < s_lines; r++) {
        const int l = top + r;
        const int y = MARGIN_Y + r * s_line_h;
        const uint32_t rs = s_ls[l], re = s_le[l];
        bool styled = false;
        if (md && re > rs) {
            md_for(rs);
            for (uint32_t i = rs; i < re && !styled; i++) styled = s_mds[i] != 0;
        }
        if (styled) row_runs(rs, re, y, run_bg);
        const uint32_t a = hl_a > rs ? hl_a : rs;
        const uint32_t b = hl_b < re ? hl_b : re;
        /* 0033: a selected newline, or the space a wrap broke at, is a
         * cell past the row's text, so a selection shows across lines. */
        const uint32_t end = (hl_a <= re && hl_b > re && re < s_len && re < line_next(l)) ? 1 : 0;
        const int gw = (a < b || end) ? row_gw(l) : 0;
        if (a < b || end) {
            gfx_fill_rect(MARGIN_X + (int)(a - rs) * gw - EDITOR_SCALE, y - EDITOR_SCALE,
                          (int)(b - a + end) * gw + EDITOR_SCALE,
                          GFX_GLYPH_H(EDITOR_SCALE) + 2 * EDITOR_SCALE, COL_BTN);
        }
        if (styled) {
            row_runs(rs, re, y, run_text);
            row_runs(rs, re, y, run_strike);
            continue;
        }
        const uint32_t n = re - rs;
        memcpy(s_row_buf, s_text + rs, n);
        s_row_buf[n] = '\0';
        gfx_draw_text(MARGIN_X, y, s_row_buf, EDITOR_SCALE, W - MARGIN_X, COL_TEXT);
    }
}

/* Something the status line must show even on a bare page. */
static bool foot_warns(void)
{
    return !kbd_present() || s_caps || !storage_present(STORAGE_SD) || s_save_failed || s_conflict;
}

static bool s_hold_blit;        /* 0039: find draws its bar over, then blits */

static void redraw(void)
{
    const bool foot = !s_bare || foot_warns();
    if (foot != s_foot_on) { s_foot_on = foot; measure(); }
    layout();
    const int cl = cursor_line();
    if (cl < s_top) s_top = cl;
    if (cl >= s_top + s_rows) s_top = cl - s_rows + 1;

    const int W = gfx_w();
    const int H = gfx_h();
    gfx_fill_rect(0, 0, W, H, COL_BG);
    uint32_t sa = 0, sb = 0;
    sel(&sa, &sb);
    draw_rows(s_top, s_rows, sa, sb);

    /* A bar before the character, as tall as the glyph -- in a heading's
     * wider cells on a heading (0032). */
    const int cx = MARGIN_X + (int)(s_cur - s_ls[cl]) * row_gw(cl);
    const int cy = MARGIN_Y + (cl - s_top) * s_line_h;
    gfx_fill_rect(cx - EDITOR_SCALE, cy, EDITOR_SCALE, GFX_GLYPH_H(EDITOR_SCALE), COL_CURSOR);

    /* The status line: word count, and only that -- plus a note when
     * there is nothing to type on. Spec section 5. */
    if (!s_bare || !kbd_present()) {     /* no keyboard: the gear is the way out */
        char st[48];
        snprintf(st, sizeof(st), "%lu words", (unsigned long)word_count());
        /* Right-aligned, leaving the corner to the gear. */
        gfx_draw_text(W - settings_gear_w() - gfx_text_w(st, FOOT_SCALE), s_status_y, st, FOOT_SCALE, W, COL_DIM);
        settings_draw_gear();
    }
    if (!kbd_present()) gfx_draw_text(MARGIN_X, s_status_y, "no keyboard", FOOT_SCALE, W, COL_DIM);
    if (s_send_line[0]) {               /* 0038: after the card's slot, up to the word count */
        const int sx = MARGIN_X + 31 * GFX_GLYPH_W(FOOT_SCALE);
        const int room = W - settings_gear_w() - gfx_text_w("000000 words", FOOT_SCALE) - sx;
        if (room > 0) gfx_draw_text(sx, s_status_y, s_send_line, FOOT_SCALE, room, COL_DIM);
    }
    if (s_caps)        gfx_draw_text(MARGIN_X + 15 * GFX_GLYPH_W(FOOT_SCALE), s_status_y, "caps", FOOT_SCALE, W, COL_DIM);
    if (!storage_present(STORAGE_SD)) {
        gfx_draw_text(MARGIN_X + 21 * GFX_GLYPH_W(FOOT_SCALE), s_status_y, "no card", FOOT_SCALE, W, COL_DIM);
    } else if (s_save_failed) {
        gfx_draw_text(MARGIN_X + 21 * GFX_GLYPH_W(FOOT_SCALE), s_status_y, "not saved", FOOT_SCALE, W, COL_DIM);
    } else if (s_conflict) {            /* 0036: the .md was edited outside */
        gfx_draw_text(MARGIN_X + 21 * GFX_GLYPH_W(FOOT_SCALE), s_status_y, "conflict", FOOT_SCALE, W, COL_DIM);
    }

    if (!s_hold_blit) gfx_blit(0, H);
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
    s_anchor = NO_ANCHOR;           /* 0033: an edit ends the selection */
}

static void undo_clear(void);     /* 0039 */

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
        undo_clear();                   /* 0039: every position moves */
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
    s_export_due = true;                /* 0036: a cheap check if it matches */
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
    if (doclog_append(s_base, s_base_len, s_text, s_len, t, t_ok, s_src, settings_author()) == ESP_OK) {
        memcpy(s_base, s_text, s_len);
        s_base_len = s_len;
        s_pending = false;
        s_save_failed = false;
        s_export_due = true;
    } else {
        /* Try again after another pause's worth. */
        s_save_failed = true;
        s_last_edit = now;
        s_first_edit = now;
    }
    return redraw_needed || was_failed != s_save_failed;
}

/*
 * 0036: write the export once typing has paused, or now when `force`,
 * from s_base -- what the log holds -- so only flushed text goes out.
 * A conflict (the .md edited outside) writes name.conflict.md and says
 * so in the status line until an export lands. True when that changed.
 */
static bool export_tick(bool force)
{
    if (!s_export_due || s_pending || !doclog_is_open()) return false;
    if (!force && xTaskGetTickCount() - s_last_edit < pdMS_TO_TICKS(EXPORT_PAUSE_MS)) return false;
    const bool was = s_conflict;
    int64_t t;
    const bool t_ok = rtc_now(&t);
    const esp_app_desc_t *d = esp_app_get_description();     /* 0037: the footer */
    const esp_err_t e = doclog_export(s_base, s_base_len, settings_author(), t, t_ok,
                                      d ? d->version : NULL);
    if (e == ESP_OK) {
        s_export_due = false;
        s_conflict = false;
    } else if (e == ESP_ERR_INVALID_STATE && storage_present(STORAGE_SD)) {
        s_export_due = false;           /* written beside it; again at the next flush */
        s_conflict = true;
    } else {
        s_last_edit = xTaskGetTickCount();  /* another pause's worth */
    }
    return was != s_conflict;
}

/*
 * 0038: send today's text now, whole (spec 6: a full copy on a manual
 * send), through the send task, after the log and the export are
 * current. The subject names the seqs it covers, so a duplicate is
 * plain; if the text could not be flushed (no card) it goes anyway,
 * marked unsaved. The body ends in the export's footer. No checkpoint
 * is written yet: the background send will need one, and its record.
 */
static bool send_now(void)
{
    save_tick(true);
    export_tick(true);
    char range[40], foot[DOCLOG_FOOT_MAX];
    if (!s_pending && doclog_is_open())
        snprintf(range, sizeof range, "seq 1-%lu", (unsigned long)doclog_seq());
    else
        snprintf(range, sizeof range, "not yet saved");
    int64_t t;
    const bool t_ok = rtc_now(&t);
    const esp_app_desc_t *d = esp_app_get_description();
    doclog_footer(foot, settings_author(), t, t_ok, d ? d->version : NULL);
    const bool ok = sender_send_doc(s_name, range, s_text, s_len, foot, doclog_copy_log);
    if (ok) {
        s_sending = true;
        s_send_until = 0;
    } else {
        snprintf(s_send_line, sizeof s_send_line, "busy; send again in a moment");
        s_send_until = xTaskGetTickCount() + pdMS_TO_TICKS(SEND_SHOW_MS);
    }
    return ok;
}

/* The send's line: what it is doing while it runs, how it went for a
 * while after. True when the status line changed. */
static bool send_tick(void)
{
    static uint32_t seen;
    const uint32_t g = sender_gen();
    if (s_sending && g != seen) {
        seen = g;
        char line[160];
        const bool busy = sender_status(line, sizeof line);
        snprintf(s_send_line, sizeof s_send_line, "%s", line);
        if (!busy) {
            s_sending = false;
            s_send_until = xTaskGetTickCount() + pdMS_TO_TICKS(SEND_SHOW_MS);
        }
        return true;
    }
    if (!s_sending && s_send_line[0] && s_send_until && xTaskGetTickCount() >= s_send_until) {
        s_send_line[0] = '\0';
        return true;
    }
    return false;
}

/* The day's file name, from the RTC. Fixed for the session for now;
 * midnight rollover (spec 5) is later. 0044: in file mode, the note
 * last opened, or notes.md when there is none. */
static void pick_name(void)
{
    if (files_mode() == FILES_FILE) {
        if (!files_last(s_name, sizeof s_name)) strcpy(s_name, FILES_DEFAULT);
        return;
    }
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

/*
 * 0044: Cmd+O. The text is flushed and exported, the log let go, and the
 * notes page (files.h) shown; after it, the note it chose, or this one
 * again -- renamed, perhaps, or gone to the trash. Not with text the card
 * does not have yet: that would be left behind in RAM.
 */
static void doc_reset(const char *name)
{
    snprintf(s_name, sizeof s_name, "%s", name);
    s_len = s_cur = s_base_len = 0;
    s_anchor = NO_ANCHOR;
    s_want_col = -1;
    s_top = 0;
    s_md_from = UINT32_MAX;
    undo_clear();
    s_ever_open = false;
    s_export_due = false;
    s_conflict = false;
    s_save_failed = false;
    s_attach_after = 0;
    if (files_mode() == FILES_FILE) files_set_last(s_name);
}

static void open_files(bool apple)
{
    save_tick(true);
    export_tick(true);
    if (s_pending || !doclog_is_open()) {
        snprintf(s_send_line, sizeof s_send_line, "not saved yet; the notes wait for the card");
        s_send_until = xTaskGetTickCount() + pdMS_TO_TICKS(SEND_SHOW_MS);
        return;
    }
    char cur[FILES_NAME_MAX + 1];
    snprintf(cur, sizeof cur, "%s", s_name);
    doclog_close();
    const files_result_t r = files_page(cur, sizeof cur, apple);
    if (r == FILES_STAY && !strcmp(cur, s_name)) {
        /* the same note: open its log again, and keep the cursor and
         * the undo history if it is the text we have */
        int64_t now;
        const bool now_ok = rtc_now(&now);
        uint32_t rl = 0;
        if (doclog_open(s_name, s_base, &rl, now, now_ok) == ESP_OK && rl == s_len &&
            !memcmp(s_base, s_text, rl)) {
            s_base_len = rl;
            s_gen = storage_generation();
            return;
        }
        doclog_close();
    }
    if (!cur[0]) {
        /* this one went to the trash: today's, or file mode's first */
        pick_name();
        if (files_mode() == FILES_FILE) strcpy(s_name, FILES_DEFAULT);
        snprintf(cur, sizeof cur, "%s", s_name);
    }
    doc_reset(cur);
    attach();
}

/* ---- editing ---------------------------------------------------------- */

static void undo_record(uint32_t pos, const char *del, uint32_t dn, const char *ins, uint32_t in);

static void insert(const char *s, uint32_t n)
{
    if (s_len + n > TEXT_MAX) {
        ESP_LOGW(TAG, "buffer full");
        return;
    }
    undo_record(s_cur, NULL, 0, s, n);
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
    undo_record(from, s_text + from, to - from, NULL, 0);
    memmove(s_text + from, s_text + to, s_len - to);
    s_len -= to - from;
    s_cur = from;
    edited();
}

/* ---- undo (0039) -------------------------------------------------------
 *
 * Every insert() and erase() is kept, in RAM, until power-off: where,
 * what went, what came, where the cursor was. Undo takes the text back
 * by one group of them and redo forward again -- and that is all: the
 * next flush logs the difference like any other, so undo appends a
 * changeset that reverses the last and never takes one out of the log
 * (spec 5). A group is one key's worth (a type-over's delete and its
 * letter, a paste, an indent of many lines), or a run of typing up to
 * where a word begins, or a run of Backspace or Delete, with no pause
 * of UNDO_MERGE_MS in it. The oldest groups are let go when the arena
 * or the table is full; an edit too big for the arena forgets it all.
 */
#define UNDO_ARENA      (1024 * 1024)
#define UNDO_OPS        (8192)
#define UNDO_MERGE_MS   (2000)

typedef struct {
    uint32_t pos, del, ins;     /* at pos, del bytes went and ins came */
    uint32_t off;               /* in the arena: the del bytes, then the ins */
    uint32_t cur, anchor;       /* the cursor and the selection's anchor before */
    uint32_t group;
} undo_op_t;

static undo_op_t *s_uops;       /* UNDO_OPS, PSRAM */
static char      *s_uarena;     /* UNDO_ARENA, PSRAM */
static uint32_t   s_un;         /* ops kept */
static uint32_t   s_udone;      /* [0, s_udone) done; the rest undone, for redo */
static uint32_t   s_ugroup;     /* never reused */
static uint32_t   s_action;     /* bumped per key, so one key is one group */
static uint32_t   s_uaction;
static TickType_t s_ulast;
static bool       s_replaying;

static uint32_t undo_used(uint32_t n) { return n ? s_uops[n - 1].off + s_uops[n - 1].del + s_uops[n - 1].ins : 0; }

static void undo_clear(void) { s_un = s_udone = 0; s_ugroup++; }

/* Let the oldest group go. */
static void undo_drop(void)
{
    uint32_t k = 1;
    while (k < s_un && s_uops[k].group == s_uops[0].group) k++;
    const uint32_t cut = k < s_un ? s_uops[k].off : undo_used(s_un);
    memmove(s_uarena, s_uarena + cut, undo_used(s_un) - cut);
    memmove(s_uops, s_uops + k, (s_un - k) * sizeof *s_uops);
    s_un -= k;
    s_udone = s_udone > k ? s_udone - k : 0;
    for (uint32_t i = 0; i < s_un; i++) s_uops[i].off -= cut;
}

static bool undo_merges(const undo_op_t *l, uint32_t pos, uint32_t dn, const char *ins, uint32_t in)
{
    if (xTaskGetTickCount() - s_ulast >= pdMS_TO_TICKS(UNDO_MERGE_MS)) return false;
    if (l->del == 0 && dn == 0 && l->ins == 1 && in == 1 && pos == l->pos + 1) {
        const char prev = s_uarena[l->off];
        return ins[0] != '\n' && !(prev == ' ' && ins[0] != ' ');   /* a word starts a group */
    }
    return l->ins == 0 && in == 0 && l->del == 1 && dn == 1 && (pos + 1 == l->pos || pos == l->pos);
}

static void undo_record(uint32_t pos, const char *del, uint32_t dn, const char *ins, uint32_t in)
{
    if (s_replaying || !s_uops || !s_uarena) return;
    s_un = s_udone;                     /* a new edit: redo is gone */
    if (dn + in > UNDO_ARENA) {
        ESP_LOGW(TAG, "edit too big to undo; history cleared");
        undo_clear();
        return;
    }
    const bool same = s_un && s_uops[s_un - 1].group == s_ugroup &&
                      (s_action == s_uaction || undo_merges(&s_uops[s_un - 1], pos, dn, ins, in));
    if (!same) s_ugroup++;
    while (s_un && (s_un >= UNDO_OPS || undo_used(s_un) + dn + in > UNDO_ARENA)) undo_drop();
    undo_op_t *o = &s_uops[s_un];
    *o = (undo_op_t){ .pos = pos, .del = dn, .ins = in, .off = undo_used(s_un), .cur = s_cur,
                      .anchor = s_anchor, .group = s_ugroup };
    if (dn) memcpy(s_uarena + o->off, del, dn);
    if (in) memcpy(s_uarena + o->off + dn, ins, in);
    s_un++;
    s_udone = s_un;
    s_uaction = s_action;
    s_ulast = xTaskGetTickCount();
}

/* At pos, `rm` bytes go and `n` of `add` come: undo's and redo's edit. */
static void undo_apply(uint32_t pos, uint32_t rm, const char *add, uint32_t n)
{
    memmove(s_text + pos + n, s_text + pos + rm, s_len - pos - rm);
    memcpy(s_text + pos, add, n);
    s_len = s_len - rm + n;
    edited();
}

static bool undo(void)
{
    if (!s_udone) return false;
    const uint32_t g = s_uops[s_udone - 1].group;
    uint32_t cur = s_cur, anchor = NO_ANCHOR;
    s_replaying = true;
    while (s_udone && s_uops[s_udone - 1].group == g) {
        const undo_op_t *o = &s_uops[--s_udone];
        undo_apply(o->pos, o->ins, s_uarena + o->off, o->del);
        cur = o->cur;
        anchor = o->anchor;
    }
    s_replaying = false;
    s_cur = cur > s_len ? s_len : cur;
    s_anchor = anchor <= s_len ? anchor : NO_ANCHOR;    /* what was selected, again */
    s_ugroup++;                         /* typing after starts afresh */
    return true;
}

static bool redo(void)
{
    if (s_udone >= s_un) return false;
    const uint32_t g = s_uops[s_udone].group;
    uint32_t cur = s_cur;
    s_replaying = true;
    while (s_udone < s_un && s_uops[s_udone].group == g) {
        const undo_op_t *o = &s_uops[s_udone++];
        undo_apply(o->pos, o->del, s_uarena + o->off + o->del, o->ins);
        cur = o->pos + o->ins;
    }
    s_replaying = false;
    s_cur = cur > s_len ? s_len : cur;
    s_ugroup++;
    return true;
}

/* ---- indent (0039) ---------------------------------------------------- */

/* Insert at `pos`, the cursor where it is: undo keeps it as it was. */
static void put_at(uint32_t pos, const char *t, uint32_t n)
{
    if (s_len + n > TEXT_MAX) return;
    undo_record(pos, NULL, 0, t, n);
    memmove(s_text + pos + n, s_text + pos, s_len - pos);
    memcpy(s_text + pos, t, n);
    s_len += n;
    edited();
}

static uint32_t lstart(uint32_t p)
{
    while (p > 0 && s_text[p - 1] != '\n') p--;
    return p;
}

/* Tab / Shift+Tab over the lines from a to b (b > a: those it touches),
 * last line first so the earlier positions hold. With a selection, the
 * lines are selected after, whole; without, the cursor keeps its place
 * in the text. */
static void indent(uint32_t a, uint32_t b, bool out, bool keep_sel)
{
    const uint32_t first = lstart(a);
    uint32_t ls = lstart(b > a ? b - 1 : b);
    uint32_t cur = s_cur;
    /* Where the last line touched ends, as a distance from the text's
     * end, which no edit here reaches past. */
    uint32_t end = b;
    if (!(b > a && s_text[b - 1] == '\n')) while (end < s_len && s_text[end] != '\n') end++;
    const uint32_t tail = s_len - end;
    for (;;) {
        if (out) {
            uint32_t n = 0;
            while (n < TAB_SPACES && ls + n < s_len && s_text[ls + n] == ' ') n++;
            if (n) {
                erase(ls, ls + n);
                if (cur > ls) cur -= cur - ls < n ? cur - ls : n;
            }
        } else if (s_len + TAB_SPACES <= TEXT_MAX) {
            put_at(ls, "    ", TAB_SPACES);
            if (cur >= ls) cur += TAB_SPACES;
        }
        if (ls <= first) break;
        ls = lstart(ls - 1);
    }
    if (keep_sel) {
        s_anchor = first;
        s_cur = s_len - tail;
    } else {
        s_cur = cur > s_len ? s_len : cur;
    }
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
static bool key_do(const kbd_event_t *ev)
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

/*
 * 0033: selection, around key_do(). Shift with any movement key
 * extends the selection from where the cursor was; a movement without
 * it ends the selection, and plain Left / Right go to its start / end,
 * as Word and the Mac do. Cmd+A selects all; Cmd+C / X / V copy, cut
 * and paste. With a selection, typing, Enter, Tab, Backspace and Delete
 * (with or without the word modifier) replace or delete it.
 */
static bool moves(uint8_t usage)
{
    return usage == KBD_LEFT || usage == KBD_RIGHT || usage == KBD_UP || usage == KBD_DOWN ||
           usage == KBD_HOME || usage == KBD_END || usage == KBD_PAGEUP || usage == KBD_PAGEDOWN;
}

static bool key(const kbd_event_t *ev)
{
    const uint8_t usage = ev->usage, mods = ev->mods;
    const bool shift = (mods & KBD_MOD_SHIFT) != 0;
    const bool other = (mods & (KBD_MOD_CTRL | KBD_MOD_GUI | KBD_MOD_ALT)) != 0;
    uint32_t a = 0, b = 0;
    const bool has = sel(&a, &b);
    s_action++;                         /* 0039: one key, one undo group at most */

    /* 0039: undo, redo. Cmd+Z, Cmd+Shift+Z, and Ctrl+Y on a PC. */
    if (ev->cmd && (usage == KBD_Z || (usage == KBD_Y && !ev->apple && !shift))) {
        s_want_col = -1;
        return (usage == KBD_Z && !shift) ? undo() : redo();
    }
    /* 0039: Tab over a selection of lines indents them; Shift+Tab
     * takes up to four spaces off the start of each, or of the
     * cursor's line. */
    if (usage == KBD_TAB && !other) {
        const bool lines = has && memchr(s_text + a, '\n', b - a) != NULL;
        if (shift || lines) {
            s_want_col = -1;
            indent(has ? a : s_cur, has ? b : s_cur, shift, has);
            return true;
        }
    }

    if (moves(usage)) {
        if (!shift && !other && has && (usage == KBD_LEFT || usage == KBD_RIGHT)) {
            s_cur = usage == KBD_LEFT ? a : b;
            s_anchor = NO_ANCHOR;
            s_want_col = -1;
            return true;
        }
        const uint32_t from = s_cur;
        const bool r = key_do(ev);
        if (shift) {
            if (s_anchor == NO_ANCHOR) s_anchor = from;
        } else if (s_anchor != NO_ANCHOR) {
            s_anchor = NO_ANCHOR;
            return true;
        }
        return r;
    }

    if (ev->cmd && !shift) {
        switch (usage) {
        case KBD_A:
            s_anchor = 0;
            s_cur = s_len;
            s_want_col = -1;
            return true;
        case KBD_C:
        case KBD_X:
            if (!has || !s_clip) return false;
            memcpy(s_clip, s_text + a, b - a);
            s_clip_len = b - a;
            if (usage == KBD_X) erase(a, b);
            return usage == KBD_X;
        case KBD_V:
            if (!s_clip || !s_clip_len) return false;
            if (s_len - (b - a) + s_clip_len > TEXT_MAX) {
                ESP_LOGW(TAG, "paste does not fit");
                return false;
            }
            s_want_col = -1;
            if (has) erase(a, b);
            insert(s_clip, s_clip_len);
            return true;
        default:
            break;
        }
    }

    if (has) {
        /* Backspace and Delete, alone or with the keyboard's word or
         * line modifier: the selection, and only it. */
        const bool none = ev->apple ? (mods & KBD_MOD_CTRL) : (mods & (KBD_MOD_GUI | KBD_MOD_ALT));
        if (!none && (usage == KBD_BACKSPACE || usage == KBD_DELETE)) {
            s_want_col = -1;
            erase(a, b);
            return true;
        }
        const char c = other ? 0 : kbd_ascii(usage, mods, s_caps);
        if (usage == KBD_TAB || c == '\n' || (c >= 0x20 && c < 0x7F)) {
            erase(a, b);                /* then typed where it was */
        }
    }
    return key_do(ev);
}

static bool repeats(uint8_t usage)
{
    return usage != KBD_CAPSLOCK && usage != KBD_ESCAPE && usage < 0xE0;
}

/* ---- find (0039) ------------------------------------------------------
 *
 * Cmd+F opens a bar in the status line's row. What is typed is found as
 * it is typed, from where the cursor was, ignoring case (ASCII), and
 * the match is selected. Enter or F3 finds the next, Shift with either
 * the one before -- and Cmd+G / Cmd+Shift+G on an Apple keyboard -- each
 * going round the end. Esc closes the bar and leaves the match selected.
 * The query stays until power-off: F3 or Cmd+G in the editor use it
 * with the bar shut. A Cmd+F with a query shows it, and the first letter
 * typed replaces it.
 */
#define FIND_MAX        (64)
static char s_find[FIND_MAX + 1];
static int  s_find_len;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool match_at(uint32_t i)
{
    for (int k = 0; k < s_find_len; k++) if (lower(s_text[i + (uint32_t)k]) != lower(s_find[k])) return false;
    return true;
}

/* The next match from `from` (or the one before it), round the end
 * once. Selected when found. */
static bool find_go(uint32_t from, bool fwd)
{
    const uint32_t n = (uint32_t)s_find_len;
    if (!n || n > s_len) return false;
    const uint32_t last = s_len - n;     /* the last place a match can start */
    if (from > last + 1) from = last + 1;
    uint32_t hit = UINT32_MAX;
    if (fwd) {
        for (uint32_t i = from; i <= last && hit == UINT32_MAX; i++) if (match_at(i)) hit = i;
        for (uint32_t i = 0; i < from && i <= last && hit == UINT32_MAX; i++) if (match_at(i)) hit = i;
    } else {
        for (uint32_t i = from; i-- > 0 && hit == UINT32_MAX; ) if (i <= last && match_at(i)) hit = i;
        for (uint32_t i = last + 1; i-- > from && hit == UINT32_MAX; ) if (match_at(i)) hit = i;
    }
    if (hit == UINT32_MAX) return false;
    s_anchor = hit;
    s_cur = hit + n;
    s_want_col = -1;
    return true;
}

/* Next from the selection's end, or the one before its start. */
static bool find_step(bool fwd)
{
    uint32_t a = s_cur, b = s_cur;
    sel(&a, &b);
    return find_go(fwd ? b : a, fwd);
}

static void find_bar(const char *note)
{
    s_hold_blit = true;
    redraw();
    s_hold_blit = false;
    const int W = gfx_w(), H = gfx_h();
    const int y = H - MARGIN_Y / 2 - GFX_GLYPH_H(FOOT_SCALE) - 8;
    gfx_fill_rect(0, y, W, H - y, COL_HEAD);
    gfx_fill_rect(0, y, W, 2, COL_RULE);
    const int gw = GFX_GLYPH_W(FOOT_SCALE);
    const int ty = y + 8;
    gfx_draw_text(MARGIN_X, ty, "Find:", FOOT_SCALE, W, COL_DIM);
    const int fx = MARGIN_X + 6 * gw;
    gfx_draw_text(fx, ty, s_find, FOOT_SCALE, W - fx - MARGIN_X, COL_TEXT);
    gfx_fill_rect(fx + s_find_len * gw, ty, FOOT_SCALE, GFX_GLYPH_H(FOOT_SCALE), COL_CURSOR);
    if (note) {
        const int nx = fx + (s_find_len + 3) * gw;
        gfx_draw_text(nx, ty, note, FOOT_SCALE, W - nx - MARGIN_X, COL_DIM);
    }
    gfx_blit(0, H);
}

static void find_view(void)
{
    uint32_t origin = s_cur, b0 = s_cur;
    sel(&origin, &b0);
    bool fresh = s_find_len > 0;        /* the old query: the first letter replaces it */
    const char *note = NULL;
    find_bar(NULL);
    for (;;) {
        kbd_event_t ev;
        if (!kbd_get(&ev, pdMS_TO_TICKS(250))) { save_tick(false); continue; }
        if (!ev.down) continue;
        const bool shift = (ev.mods & KBD_MOD_SHIFT) != 0;
        const bool plain = !(ev.mods & (KBD_MOD_CTRL | KBD_MOD_GUI | KBD_MOD_ALT));
        bool changed = false, step = false, fwd = true;
        switch (ev.usage) {
        case KBD_ESCAPE:    return;
        case KBD_ENTER:
        case KBD_KP_ENTER:
        case KBD_F3:        step = true; fwd = !shift; break;
        case KBD_G:         if (ev.cmd && ev.apple) { step = true; fwd = !shift; } break;
        case KBD_F:         if (ev.cmd) return; break;      /* Cmd+F again closes */
        case KBD_CAPSLOCK:  s_caps = !s_caps; break;
        case KBD_BACKSPACE:
            if (fresh) { s_find_len = 0; fresh = false; }
            else if (s_find_len > 0) s_find_len--;
            s_find[s_find_len] = '\0';
            changed = true;
            break;
        default: break;
        }
        if (!step && !changed && plain && ev.usage != KBD_CAPSLOCK && ev.usage != KBD_ENTER &&
            ev.usage != KBD_KP_ENTER && ev.usage != KBD_F3) {
            const char c = kbd_ascii(ev.usage, ev.mods, s_caps);
            if (c >= ' ' && c <= '~') {
                if (fresh) { s_find_len = 0; fresh = false; }
                if (s_find_len < FIND_MAX) { s_find[s_find_len++] = c; s_find[s_find_len] = '\0'; }
                changed = true;
            }
        }
        if (changed) {
            fresh = false;
            if (s_find_len == 0) { s_anchor = NO_ANCHOR; s_cur = origin; note = NULL; }
            else if (!find_go(origin, true)) { s_anchor = NO_ANCHOR; s_cur = origin; note = "not found"; }
            else note = NULL;
        } else if (step) {
            fresh = false;
            note = find_step(fwd) ? NULL : "not found";
        }
        find_bar(note);
    }
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
                if (!next || next == st) break;     /* NULL: done (gfx.h; 0040) */
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
    s_action++;                     /* 0039: each correction its own undo */
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
/* ---- F1 and F12: sheets over the text (0029) --------------------------- */

typedef struct { const char *keys, *does; } sheet_row_t;

/* The keyboard's own column of spec 5's table, built rows only. */
static const sheet_row_t k_keys_pc[] = {
    { "Ctrl+Left / Right",       "word back / forward" },
    { "Ctrl+Up / Down",          "paragraph back / forward" },
    { "Home / End",              "line start / end" },
    { "Ctrl+Home / End",         "document start / end" },
    { "PageUp / PageDown",       "a screen up / down" },
    { "Ctrl+Backspace / Delete", "delete word before / after" },
    { "Ctrl+L, Ctrl+Shift+K",    "delete the line" },
    { "Shift+moves; Ctrl+A",     "select; select all" },
    { "Ctrl+C / X / V",          "copy / cut / paste" },
    { "Ctrl+Z; Ctrl+Y",          "undo; redo" },
    { "Tab / Shift+Tab",         "indent / unindent" },
    { "Ctrl+F; F3 / Shift+F3",   "find; next / previous" },
    { "Ctrl+S; Ctrl+Enter",      "save now; email notes" },
    { "F7",                      "check spelling" },
    { "Ctrl+= / Ctrl+-",         "text larger / smaller" },
    { "Ctrl+,; Ctrl+O",          "settings; notes" },
    { "F1 / F11 / F12",          "keys / bare page / stats" },
};
static const sheet_row_t k_keys_apple[] = {
    { "Option+Left / Right",     "word back / forward" },
    { "Option+Up / Down",        "paragraph back / forward" },
    { "Command+Left / Right",    "line start / end" },
    { "Command+Up / Down",       "document start / end" },
    { "PageUp / PageDown",       "a screen up / down" },
    { "Option+Delete",           "delete the word before" },
    { "Command+Delete",          "delete to the line's start" },
    { "Shift+moves; Command+A",  "select; select all" },
    { "Command+C / X / V",       "copy / cut / paste" },
    { "Command+Z / Shift+Z",     "undo / redo" },
    { "Tab / Shift+Tab",         "indent / unindent" },
    { "Command+F; Command+G",    "find; next (Shift: back)" },
    { "Command+S; Command+Enter", "save now; email notes" },
    { "Command+; or F7",         "check spelling" },
    { "Command+= / Command+-",   "text larger / smaller" },
    { "Command+,; Command+O",    "settings; notes" },
    { "F1 / F11 / F12",          "keys / bare page / stats" },
};
#define SHEET_SCALE     (2)

/* Rows drawn over the text: keys left, what they do right -- or, the
 * narrow way up, the second under the first -- until a key or a tap. */
static void sheet(const char *title, const sheet_row_t *row, int n)
{
    const int W = gfx_w(), H = gfx_h();
    const int lh = GFX_GLYPH_H(SHEET_SCALE) + 10;
    /* 0039: a row on one line wherever both halves fit -- the narrow
     * way up too -- and keys over what they do only where they do not. */
    int mk = 0, md = 0;
    for (int i = 0; i < n; i++) {
        const int k = gfx_text_w(row[i].keys, SHEET_SCALE), d = gfx_text_w(row[i].does, SHEET_SCALE);
        if (k > mk) mk = k;
        if (d > md) md = d;
    }
    const bool narrow = mk + 24 + md > W - 2 * MARGIN_X;
    const int col2 = narrow ? MARGIN_X + 32 : W >= 1000 ? W * 45 / 100 : MARGIN_X + mk + 24;
    gfx_fill_rect(0, 0, W, H, COL_HEAD);
    int y = MARGIN_Y;
    gfx_draw_text(MARGIN_X, y, title, SHEET_SCALE + 1, W - 2 * MARGIN_X, COL_TEXT);
    y += GFX_GLYPH_H(SHEET_SCALE + 1) + 24;
    for (int i = 0; i < n && y + lh < H - lh; i++) {
        gfx_draw_text(MARGIN_X, y, row[i].keys, SHEET_SCALE, narrow ? W - 2 * MARGIN_X : col2 - MARGIN_X - 16, COL_TEXT);
        if (narrow) y += lh;
        gfx_draw_text(col2, y, row[i].does, SHEET_SCALE, W - MARGIN_X - col2, COL_DIM);
        y += lh + (narrow ? 8 : 0);
    }
    gfx_draw_text(MARGIN_X, H - MARGIN_Y - GFX_GLYPH_H(SHEET_SCALE), "Any key to close.",
                  SHEET_SCALE, W - 2 * MARGIN_X, COL_DIM);
    gfx_blit(0, H);

    touch_swallow();
    for (;;) {
        kbd_event_t ev;
        if (kbd_get(&ev, pdMS_TO_TICKS(30)) && ev.down) break;
        int tx, ty;
        if (touch_get(&tx, &ty)) { touch_swallow(); break; }
    }
}

static void keys_sheet(bool apple)
{
    if (apple) sheet("Keys (Apple keyboard)", k_keys_apple, (int)(sizeof k_keys_apple / sizeof k_keys_apple[0]));
    else       sheet("Keys", k_keys_pc, (int)(sizeof k_keys_pc / sizeof k_keys_pc[0]));
}

/* F12: what the page holds. Characters are UTF-8 code points; a
 * paragraph is a run of lines between blank ones. */
static void stats_sheet(void)
{
    uint32_t chars = 0, nonsp = 0, paras = 0, line = 1, col = 0;
    bool blank = true;              /* the line so far has only spaces */
    bool in_para = false;
    for (uint32_t i = 0; i < s_len; i++) {
        const unsigned char c = (unsigned char)s_text[i];
        if ((c & 0xC0) == 0x80) continue;
        chars++;
        if (c == '\n') {
            if (blank) in_para = false;
            blank = true;
            continue;
        }
        if (c != ' ' && c != '\t') {
            nonsp++;
            if (blank && !in_para) { paras++; in_para = true; }
            blank = false;
        }
    }
    for (uint32_t i = 0; i < s_cur && i < s_len; i++) {
        if (s_text[i] == '\n') { line++; col = 0; }
        else if (((unsigned char)s_text[i] & 0xC0) != 0x80) col++;
    }
    const uint32_t words = word_count();
    static char v[9][FILES_NAME_MAX + 8];      /* 0044: a name as long as file mode makes */
    snprintf(v[0], sizeof v[0], "%s", s_name[0] ? s_name : "(none yet)");
    snprintf(v[1], sizeof v[1], "%lu", (unsigned long)words);
    snprintf(v[2], sizeof v[2], "%lu", (unsigned long)chars);
    snprintf(v[3], sizeof v[3], "%lu", (unsigned long)nonsp);
    snprintf(v[4], sizeof v[4], "%lu", (unsigned long)paras);
    snprintf(v[5], sizeof v[5], "%lu (%d on screen)", (unsigned long)(s_lines > 0 ? s_lines : 1), s_rows);
    snprintf(v[6], sizeof v[6], "line %lu, column %lu", (unsigned long)line, (unsigned long)col + 1);
    snprintf(v[7], sizeof v[7], "%lu of %u KB", (unsigned long)((s_len + 1023) / 1024), (unsigned)(TEXT_MAX / 1024));
    snprintf(v[8], sizeof v[8], "about %lu min", (unsigned long)((words + 229) / 230));
    const sheet_row_t rows[] = {
        { "Document",      v[0] }, { "Words",            v[1] },
        { "Characters",    v[2] }, { "Without spaces",   v[3] },
        { "Paragraphs",    v[4] }, { "Lines, wrapped",   v[5] },
        { "Cursor",        v[6] }, { "Size",             v[7] },
        { "Reading time",  v[8] },
    };
    sheet("Page stats", rows, (int)(sizeof rows / sizeof rows[0]));
}

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
    s_cols_head = (W - 2 * MARGIN_X) / GFX_HEAVY_W(EDITOR_SCALE);
    s_line_h = GFX_GLYPH_H(EDITOR_SCALE) + 4 * EDITOR_SCALE;
    s_status_y = H - MARGIN_Y / 2 - GFX_GLYPH_H(FOOT_SCALE);
    s_rows   = ((s_foot_on ? s_status_y : H - MARGIN_Y / 2) - MARGIN_Y - 8) / s_line_h;
    ESP_LOGI(TAG, "%d columns, %d rows, scale %d", s_cols, s_rows, EDITOR_SCALE);
}

esp_err_t editor_init(void)
{
    s_text    = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    s_ls      = heap_caps_malloc((TEXT_MAX + 2) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_le      = heap_caps_malloc((TEXT_MAX + 2) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_base    = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    s_mds     = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);   /* 0031; drawn plain without */
    s_clip    = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);   /* 0033; no copy or paste without */
    s_uops    = heap_caps_malloc(UNDO_OPS * sizeof *s_uops, MALLOC_CAP_SPIRAM);   /* 0039; no undo without */
    s_uarena  = heap_caps_malloc(UNDO_ARENA, MALLOC_CAP_SPIRAM);
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
    settings_on_send(send_now);         /* 0038: the settings page's button */
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
            if (ev.down && !(ev.mods & ~KBD_MOD_SHIFT) &&
                (ev.usage == KBD_F1 || ev.usage == KBD_F11 || ev.usage == KBD_F12)) {
                held = 0;               /* 0029: the F keys, unmodified */
                if (ev.usage == KBD_F11) s_bare = !s_bare;
                else if (ev.usage == KBD_F1) { layout(); keys_sheet(ev.apple); touch_down = false; }
                else { layout(); stats_sheet(); touch_down = false; }
                dirty = true;
            } else if (ev.down && ev.usage == KBD_S && ev.cmd) {
                held = 0;               /* 0036: save now -- the log, then the export */
                save_tick(true);
                export_tick(true);
                dirty = true;
            } else if (ev.down && ev.cmd && ev.usage == KBD_F) {
                held = 0;               /* 0039: find */
                find_view();
                touch_down = false;
                dirty = true;
            } else if (ev.down && ((ev.usage == KBD_F3 && !(ev.mods & ~KBD_MOD_SHIFT)) ||
                                   (ev.usage == KBD_G && ev.cmd && ev.apple))) {
                held = 0;               /* 0039: the next, or the one before */
                if (s_find_len) find_step(!(ev.mods & KBD_MOD_SHIFT));
                else { find_view(); touch_down = false; }
                dirty = true;
            } else if (ev.down && ev.cmd && (ev.usage == KBD_ENTER || ev.usage == KBD_KP_ENTER)) {
                held = 0;               /* 0038: send now, Cmd+Enter (spec 5) */
                send_now();
                dirty = true;
            } else if (ev.down && ev.cmd && ev.usage == KBD_O) {
                held = 0;               /* 0044: the notes */
                open_files(ev.apple);
                touch_down = false;
                dirty = true;
            } else if (ev.down && ev.usage == KBD_COMMA && ev.cmd) {
                open_settings = true;       /* spec 7: from the keyboard too */
            } else if (ev.down && (ev.usage == KBD_F7 || (ev.usage == KBD_SEMICOLON && ev.cmd && ev.apple))) {
                open_spell = true;          /* spec 5: Word's F7, the Mac's Cmd+; */
            } else if (ev.down) {
                if (!s_sending && s_send_line[0]) { s_send_line[0] = '\0'; dirty = true; }  /* 0038 */
                dirty = key(&ev) || dirty;
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
        if (!t_now && touch_down && (!s_bare || !kbd_present()) && settings_gear_hit(touch_x, touch_y)) open_settings = true;
        touch_down = t_now;

        if (open_settings) {
            held = 0;
            save_tick(true);            /* the card is current before we leave */
            export_tick(true);
            settings_page();
            measure();                  /* it may have turned the screen */
            touch_down = false;
            dirty = true;
        }

        if (open_spell) {
            held = 0;
            s_anchor = NO_ANCHOR;       /* the check moves the cursor */
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
        if (!held && export_tick(false)) dirty = true;
        if (send_tick()) dirty = true;

        if (dirty) redraw();
    }
}
