/*
 * files.c -- see files.h (0044).
 *
 * The page, top to bottom: a title line, feckless_chooser over the
 * notes, and a line for the keys -- or, while one is asked, the
 * question, or the field a name is typed in.
 *
 * SPDX-License-Identifier: MIT
 */
#include "files.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "feckless_chooser.h"
#include "gfx.h"
#include "nvs_handler.h"
#include "touch.h"

#include "display.h"
#include "doclog.h"
#include "fname.h"
#include "kbd.h"

static const char *TAG = "files";

#define NVS_NS      "jinxed"
#define NVS_MODE    "mode"
#define NVS_FILE    "file"

#define PAD         (40)
#define POLL_MS     (20)
#define KIND_NOTE   (0)
#define KIND_CONF   (1)     /* name.conflict.ext: dim */

/* ---- the mode and the last note ------------------------------------------ */

static int s_mode = -1;

int files_mode(void)
{
    if (s_mode < 0) {
        uint8_t m = FILES_DAILY;
        if (nvsh_get_u8(NVS_NS, NVS_MODE, &m) != ESP_OK || m > FILES_FILE) m = FILES_DAILY;
        s_mode = m;
    }
    return s_mode;
}

void files_set_mode(int mode)
{
    s_mode = mode == FILES_FILE ? FILES_FILE : FILES_DAILY;
    if (nvsh_set_u8(NVS_NS, NVS_MODE, (uint8_t)s_mode) != ESP_OK)
        ESP_LOGW(TAG, "mode not kept; it holds until power-off");
}

bool files_last(char *name, size_t n)
{
    size_t len = n;
    if (nvsh_get_str(NVS_NS, NVS_FILE, name, &len) != ESP_OK || !fname_is_note(name)) {
        if (n) name[0] = '\0';
        return false;
    }
    return true;
}

void files_set_last(const char *name)
{
    if (!name || !name[0]) return;
    char was[FILES_NAME_MAX + 1];
    if (files_last(was, sizeof was) && !strcmp(was, name)) return;     /* no write for nothing */
    if (nvsh_set_str(NVS_NS, NVS_FILE, name) != ESP_OK) ESP_LOGW(TAG, "last note not kept");
}

/* ---- the page's sizes ----------------------------------------------------- */

/* The editor's text size, but no larger than 3: a list is read at a glance. */
static int scale(void)
{
    const int s = display_text_scale();
    return s > 3 ? 3 : s;
}
static int text_h(void) { return GFX_GLYPH_H(scale()); }
static int row_h(void) { return text_h() + 6 * scale(); }
static int head_h(void) { return row_h() + 8; }
static int foot_h(void) { return row_h() + 8; }
static int foot_y(void) { return gfx_h() - foot_h(); }

/* ---- the chooser's view of the notes -------------------------------------- */

static int notes(const char *dir, const storage_ent_t *e, void *ctx)
{
    (void)dir;
    (void)ctx;
    /* folders are the trash and the dictionary's, not notes */
    if (e->is_dir || !fname_is_note(e->name)) return STORAGE_LIST_SKIP;
    return strstr(e->name, ".conflict.") ? KIND_CONF : KIND_NOTE;
}

static uint16_t colour(const storage_ent_t *e, void *ctx)
{
    (void)ctx;
    return e->kind == KIND_CONF ? COL_DIM : 0;
}

/* Static: it holds a path and the list (feckless_chooser.h). */
static chooser_t s_ch;

static void setup(bool apple)
{
    s_ch = (chooser_t){
        .x = 0, .y = head_h(), .w = gfx_w(), .h = foot_y() - head_h(),
        .row_h = row_h(), .scale = scale(),
        .bg = COL_BG, .alt = COL_BG, .sel = COL_FIELD, .text = COL_TEXT, .dim = COL_DIM,
        .folder = COL_TEXT, .rule = COL_RULE,
        .root = DOCLOG_DIR,
        .empty = apple ? "No notes here yet. Command+N makes one." : "No notes here yet. Ctrl+N makes one.", .sort = STORAGE_SORT_NEWEST,
        .filter = notes, .colour = colour,
        .show_size = true, .show_time = true,
    };
}

/* ---- input ------------------------------------------------------------------ */

typedef struct {
    bool tap, key;
    int x, y;
    kbd_event_t ev;
} in_t;

static bool s_down;
static int s_tx, s_ty;

/* A key press, a tap (down and up), or nothing within POLL_MS. */
static in_t poll(void)
{
    in_t in = { 0 };
    if (kbd_get(&in.ev, pdMS_TO_TICKS(POLL_MS))) {
        in.key = in.ev.down;
        return in;
    }
    int x, y;
    const bool now = touch_get(&x, &y);
    if (now && !s_down) { s_tx = x; s_ty = y; }
    if (!now && s_down) { in.tap = true; in.x = s_tx; in.y = s_ty; }
    s_down = now;
    return in;
}

/* ---- drawing ---------------------------------------------------------------- */

static void title(const char *cur)
{
    const int W = gfx_w(), ty = (head_h() - text_h()) / 2;
    gfx_fill_rect(0, 0, W, head_h(), COL_HEAD);
    gfx_draw_text(PAD, ty, "Notes", scale(), W / 3, COL_TEXT);
    if (cur && cur[0]) {
        char s[FILES_NAME_MAX + 16];
        snprintf(s, sizeof s, "open: %s", cur);
        const int w = gfx_text_w(s, scale());
        const int max = W / 2;
        gfx_draw_text(W - PAD - (w < max ? w : max), ty, s, scale(), max, COL_DIM);
    }
}

/* The bottom line: a note in `bad` colour when there is one; else the
 * selected note's name (0046), what the keys act on, and the keys. */
static void foot(const char *note, bool apple)
{
    const int W = gfx_w(), y = foot_y(), ty = y + (foot_h() - text_h()) / 2;
    gfx_fill_rect(0, y, W, foot_h(), COL_HEAD);
    if (note) {
        gfx_draw_text(PAD, ty, note, scale(), W - 2 * PAD, COL_BAD);
        return;
    }
    const char *keys = apple
        ? "Enter: open   Cmd+N: new   Cmd+R: rename   Cmd+Delete: trash   Esc: back"
        : "Enter: open   Ctrl+N: new   Ctrl+R: rename   Delete: trash   Esc: back";
    const storage_ent_t *sel = chooser_sel(&s_ch);
    int x = PAD;
    if (sel) {
        /* the name first, never more than half the line */
        const int nw = gfx_text_w(sel->name, scale()), max = (W - 2 * PAD) / 2;
        gfx_draw_text(x, ty, sel->name, scale(), max, COL_TEXT);
        x += (nw < max ? nw : max) + 3 * GFX_GLYPH_W(scale());
    }
    if (x < W - PAD) gfx_draw_text(x, ty, keys, scale(), W - PAD - x, COL_DIM);
}

static void draw(const char *cur, const char *note, bool apple)
{
    title(cur);
    chooser_draw(&s_ch);
    foot(note, apple);
    gfx_blit(0, gfx_h());
}

/* ---- a name, typed in the bottom line --------------------------------------- */

/*
 * The field until Enter or Esc, with `start` in it. True with the name
 * fixed (fname_fix) in `out`; its complaint shown until it is right.
 */
static bool ask_name(const char *prompt, const char *start, char *out, size_t n,
                     const char *cur)
{
    char t[FILES_NAME_MAX + 1];
    snprintf(t, sizeof t, "%s", start ? start : "");
    int len = (int)strlen(t), at = len;
    bool caps = false;
    const char *why = NULL;
    for (;;) {
        title(cur);
        chooser_draw(&s_ch);
        const int W = gfx_w(), y = foot_y(), ty = y + (foot_h() - text_h()) / 2;
        gfx_fill_rect(0, y, W, foot_h(), COL_HEAD);
        char p[64];
        snprintf(p, sizeof p, "%s ", why ? why : prompt);
        const int pw = gfx_text_w(p, scale());
        gfx_draw_text(PAD, ty, p, scale(), W / 2, why ? COL_BAD : COL_DIM);
        const int fx = PAD + (pw < W / 2 ? pw : W / 2), fw = W - fx - PAD;
        gfx_fill_rect(fx - 6, y + 4, fw + 12, foot_h() - 8, COL_FIELD);
        gfx_draw_text(fx, ty, t, scale(), fw, COL_TEXT);
        const int cx = fx + at * GFX_GLYPH_W(scale());
        if (cx < fx + fw) gfx_fill_rect(cx, ty - 2, 2 * scale(), text_h() + 4, COL_CURSOR);
        gfx_blit(0, gfx_h());

        in_t in;
        do in = poll(); while (!in.key && !in.tap);
        if (in.tap) {
            if (in.y < y) return false;     /* a tap off the field: cancel */
            continue;
        }
        const kbd_event_t *e = &in.ev;
        switch (e->usage) {
        case KBD_ESCAPE:    return false;
        case KBD_ENTER:
        case KBD_KP_ENTER:
            t[len] = '\0';
            why = fname_fix(t, out, n);
            if (!why) return true;
            continue;
        case KBD_CAPSLOCK:  caps = !caps; continue;
        case KBD_LEFT:      if (at > 0) at--; continue;
        case KBD_RIGHT:     if (at < len) at++; continue;
        case KBD_HOME:      at = 0; continue;
        case KBD_END:       at = len; continue;
        case KBD_BACKSPACE:
            if (at > 0) { memmove(t + at - 1, t + at, (size_t)(len - at + 1)); at--; len--; }
            why = NULL;
            continue;
        case KBD_DELETE:
            if (at < len) { memmove(t + at, t + at + 1, (size_t)(len - at)); len--; }
            why = NULL;
            continue;
        default: break;
        }
        if (e->mods & (KBD_MOD_CTRL | KBD_MOD_GUI | KBD_MOD_ALT)) continue;
        const char c = kbd_ascii(e->usage, e->mods, caps);
        if (c < ' ' || c > '~' || len >= FILES_NAME_MAX) continue;
        memmove(t + at + 1, t + at, (size_t)(len - at + 1));
        t[at++] = c;
        len++;
        why = NULL;
    }
}

/* Y or Enter yes, anything else no. */
static bool ask_yes(const char *q, const char *cur)
{
    title(cur);
    chooser_draw(&s_ch);
    const int W = gfx_w(), y = foot_y();
    gfx_fill_rect(0, y, W, foot_h(), COL_HEAD);
    gfx_draw_text(PAD, y + (foot_h() - text_h()) / 2, q, scale(), W - 2 * PAD, COL_TEXT);
    gfx_blit(0, gfx_h());
    in_t in;
    do in = poll(); while (!in.key && !in.tap);
    return in.key && (in.ev.usage == KBD_Y || in.ev.usage == KBD_ENTER || in.ev.usage == KBD_KP_ENTER);
}

/* A new note's name, asked over the list: true with it in `cur`. Its
 * complaint, if the name is taken, in `*note`. */
static bool new_name(char *cur, size_t n, const char **note)
{
    char name[FILES_NAME_MAX + 1];
    if (!ask_name("New note:", "", name, sizeof name, cur)) return false;
    if (doclog_exists(name)) { *note = "There is already a note by that name."; return false; }
    snprintf(cur, n, "%s", name);
    return true;               /* doclog_open() makes it */
}

/* ---- the page ----------------------------------------------------------------- */

static const char *err_text(esp_err_t e)
{
    switch (e) {
    case ESP_ERR_INVALID_STATE: return "There is already a note by that name, or no card.";
    case ESP_ERR_NOT_FOUND:     return "That note is not on the card any more.";
    case ESP_ERR_INVALID_ARG:   return "That name will not do.";
    default:                    return "The card would not do it.";
    }
}

/* The chooser's key for an editor key, or 0 for none. */
static int chooser_key_for(const kbd_event_t *e)
{
    switch (e->usage) {
    case KBD_UP:        return CHOOSER_KEY_UP;
    case KBD_DOWN:      return CHOOSER_KEY_DOWN;
    case KBD_PAGEUP:    return CHOOSER_KEY_PGUP;
    case KBD_PAGEDOWN:  return CHOOSER_KEY_PGDN;
    case KBD_HOME:      return CHOOSER_KEY_HOME;
    case KBD_END:       return CHOOSER_KEY_END;
    case KBD_ENTER:
    case KBD_KP_ENTER:  return CHOOSER_KEY_ENTER;
    default: break;
    }
    if (e->mods & (KBD_MOD_CTRL | KBD_MOD_GUI | KBD_MOD_ALT)) return 0;
    const char c = kbd_ascii(e->usage, e->mods, false);
    return c > ' ' && c <= '~' ? c : 0;
}

files_result_t files_page(char *cur, size_t n, bool apple, bool new_note)
{
    setup(apple);
    chooser_begin(&s_ch, NULL);
    if (cur[0]) chooser_select(&s_ch, cur);
    const char *note = NULL;
    files_result_t res = FILES_STAY;
    s_down = false;
    touch_swallow();

    if (new_note) {             /* 0045: Cmd+N from the editor */
        if (new_name(cur, n, &note)) res = FILES_OPEN;
        if (res == FILES_OPEN || !note) goto out;
    }

    for (;;) {
        draw(cur, note, apple);
        in_t in;
        chooser_result_t r = CHOOSER_NONE;
        do {
            in = poll();
            if (!in.key && !in.tap) r = chooser_poll(&s_ch);
        } while (!in.key && !in.tap && r == CHOOSER_NONE);
        if (r != CHOOSER_NONE) continue;    /* a card came or went */
        note = NULL;

        if (in.tap) {
            if (in.y < head_h()) break;     /* the title line: back */
            r = chooser_tap(&s_ch, in.x, in.y);
        } else {
            const kbd_event_t *e = &in.ev;
            const storage_ent_t *sel = chooser_sel(&s_ch);
            if (e->usage == KBD_ESCAPE || (e->cmd && e->usage == KBD_O)) break;
            if (e->cmd && e->usage == KBD_N) {
                if (!new_name(cur, n, &note)) continue;
                res = FILES_OPEN;
                break;
            }
            if (sel && e->cmd && e->usage == KBD_R) {   /* 0046: no F2; F keys are the editor's */
                char from[FILES_NAME_MAX + 1], to[FILES_NAME_MAX + 1];
                snprintf(from, sizeof from, "%s", sel->name);
                if (!ask_name("Rename to:", from, to, sizeof to, cur) || !strcmp(from, to)) continue;
                const esp_err_t er = doclog_rename(from, to);
                if (er != ESP_OK) { note = err_text(er); continue; }
                if (!strcmp(cur, from)) { snprintf(cur, n, "%s", to); files_set_last(to); }
                chooser_reload(&s_ch);
                chooser_select(&s_ch, to);
                continue;
            }
            if (sel && (e->usage == KBD_DELETE || (e->cmd && e->usage == KBD_BACKSPACE))) {
                char q[FILES_NAME_MAX + 48], name[FILES_NAME_MAX + 1];
                snprintf(name, sizeof name, "%s", sel->name);
                snprintf(q, sizeof q, "Move %s to the trash? Y: yes, any other key: no", name);
                if (!ask_yes(q, cur)) continue;
                const esp_err_t er = doclog_trash(name);
                if (er != ESP_OK) { note = err_text(er); continue; }
                if (!strcmp(cur, name)) cur[0] = '\0';
                chooser_reload(&s_ch);
                continue;
            }
            const int k = chooser_key_for(e);
            if (!k) continue;
            r = chooser_key(&s_ch, k);
        }
        if (r == CHOOSER_OPEN) {
            const storage_ent_t *sel = chooser_sel(&s_ch);
            if (sel && strlen(sel->name) < n && strlen(sel->name) <= FILES_NAME_MAX) {
                snprintf(cur, n, "%s", sel->name);
                res = FILES_OPEN;
                break;
            }
            note = "That name is too long to open here.";
        }
        if (r == CHOOSER_CANCEL) break;
    }
out:
    chooser_done(&s_ch);
    s_down = false;
    touch_swallow();
    return res;
}
