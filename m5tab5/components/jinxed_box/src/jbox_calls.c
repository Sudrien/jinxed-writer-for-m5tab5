/*
 * jbox_calls.c -- Jinxed's own calls for its boxed editor (u-jinxed-0013).
 *
 * The editor stays the editor; the keyboard, the sender, power, settings
 * and the display's settings stay on the host (UMODE.md, 0013). These are
 * the editor's calls into them, answered here beside feckless_box_calls'
 * table, and named in ../jinxed_calls.txt for the ELF's build
 * (u-feckless-0054's OWN_CALLS).
 *
 * Most keep their own names and signatures, so editor.c calls them as it
 * does now: every pointer is checked against the box for its whole length,
 * every string the host keeps or reads is copied into the host first, and
 * results are written back only into the room the app gave. Three cannot
 * cross as they are, and have names of their own, which a shim in the ELF
 * turns back into the editor's calls:
 *
 *   jinxed_settings_author(buf, size)  settings_author() returns a host
 *                                      pointer; this copies the name out.
 *   jinxed_send_doc(name, range, text, len, foot, log_path, log_len)
 *                                      sender_send_doc() takes a function
 *                                      to copy the log, which would be the
 *                                      box's; the host reads the log
 *                                      itself, from the twin the editor
 *                                      names, held to JINXED_BOX_ROOT.
 *   jinxed_settings_page()             settings_page() calls the editor's
 *                                      send from inside its loop. Here the
 *                                      page returns 1 when "send now" is
 *                                      asked for, 0 when it is left; the
 *                                      shim sends and opens it again.
 *
 * The host's functions come through jinxed_box_host(), from the writer's
 * main: this component cannot require main, and a test gives stand-ins.
 * A call whose function was not given answers 0.
 *
 * SPDX-License-Identifier: MIT
 */
#include "jinxed_box.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "feckless_box_calls.h"

typedef feckless_umode_window_t win_t;
#define H(name) static bool name(win_t *w, const uint32_t a[7], uint32_t *ret)

static jinxed_host_t s_host;

void jinxed_box_host(const jinxed_host_t *h)
{
    if (h) s_host = *h;
    else memset(&s_host, 0, sizeof s_host);
}

static bool ok_ptr(const win_t *w, uint32_t p, uint32_t len)
{
    return len == 0 || feckless_umode_range_ok(w, (uintptr_t)p, len);
}

/* The app's string at p into out (max with its NUL); false if it is not
 * terminated within max and the box. */
static bool copy_str(const win_t *w, uint32_t p, char *out, uint32_t max)
{
    const int n = feckless_umode_strlen(w, (uintptr_t)p, max);
    if (n < 0) return false;
    memcpy(out, (const void *)(uintptr_t)p, (size_t)n);
    out[n] = '\0';
    return true;
}

/* A host string into the app's buffer [p, p + size): as much as fits,
 * always terminated; false if the buffer is not the box's. */
static bool put_str(const win_t *w, uint32_t p, uint32_t size, const char *s)
{
    if (size == 0 || !ok_ptr(w, p, size)) return false;
    const size_t n = strnlen(s, size - 1);
    memcpy((void *)(uintptr_t)p, s, n);
    ((char *)(uintptr_t)p)[n] = '\0';
    return true;
}

/* ---- the keyboard ------------------------------------------------------ */

/* (kbd_event_t *ev, ticks): waits, so unlocked; the event is the host's
 * until it is copied out whole. */
H(c_kbd_get)
{
    *ret = 0;
    if (!s_host.kbd_get || !ok_ptr(w, a[0], sizeof(kbd_event_t))) return true;
    kbd_event_t ev;
    if (s_host.kbd_get(&ev, a[1])) {
        memcpy((void *)(uintptr_t)a[0], &ev, sizeof ev);
        *ret = 1;
    }
    return true;
}
H(c_kbd_present) { (void)w; (void)a; *ret = s_host.kbd_present && s_host.kbd_present(); return true; }
H(c_kbd_ascii)
{
    (void)w;
    *ret = s_host.kbd_ascii ? (uint32_t)(uint8_t)s_host.kbd_ascii((uint8_t)a[0], (uint8_t)a[1], a[2] != 0) : 0;
    return true;
}

/* ---- the sender -------------------------------------------------------- */

H(c_sender_fetch_dict) { (void)w; (void)a; *ret = s_host.sender_fetch_dict && s_host.sender_fetch_dict(); return true; }
H(c_sender_fetched)    { (void)w; (void)a; *ret = s_host.sender_fetched && s_host.sender_fetched(); return true; }
H(c_sender_sent)       { (void)w; (void)a; *ret = s_host.sender_sent && s_host.sender_sent(); return true; }
H(c_sender_gen)        { (void)w; (void)a; *ret = s_host.sender_gen ? s_host.sender_gen() : 0; return true; }

#define STATUS_MAX 256
H(c_sender_status)
{
    *ret = 0;
    if (!s_host.sender_status || a[1] == 0 || !ok_ptr(w, a[0], a[1])) return true;
    char line[STATUS_MAX];
    const bool changed = s_host.sender_status(line, sizeof line);
    put_str(w, a[0], a[1], line);
    *ret = changed;
    return true;
}

/* jinxed_send_doc's log, read by the host from the twin the editor named:
 * the first s_log_len bytes, as doclog_copy_log() reads them. */
static char s_log_path[JINXED_BOX_PATH_MAX];
static uint32_t s_log_len;
static size_t log_from_card(void *buf, size_t cap, bool *too_big)
{
    *too_big = false;
    if (!s_log_path[0] || s_log_len == 0) return 0;
    if (s_log_len > cap) {
        *too_big = true;
        return 0;
    }
    FILE *f = fopen(s_log_path, "rb");
    if (!f) return 0;
    size_t got = fread(buf, 1, s_log_len, f);
    if (ferror(f) || got != s_log_len) got = 0;
    fclose(f);
    return got;
}

#define SEND_NAME_MAX  64
#define SEND_RANGE_MAX 40
#define SEND_FOOT_MAX  160          /* DOCLOG_FOOT_MAX */

/* (name, range, text, len, foot, log_path, log_len). The strings are
 * copied; the text is the box's, which the sender copies before it
 * returns; a log path outside JINXED_BOX_ROOT attaches no log. */
H(c_jinxed_send_doc)
{
    char name[SEND_NAME_MAX], range[SEND_RANGE_MAX], foot[SEND_FOOT_MAX];
    *ret = 0;
    if (!s_host.sender_send_doc) return true;
    if (!copy_str(w, a[0], name, sizeof name) || !copy_str(w, a[1], range, sizeof range) ||
        !ok_ptr(w, a[2], a[3]) || (a[4] && !copy_str(w, a[4], foot, sizeof foot))) {
        return true;
    }
    s_log_path[0] = '\0';
    s_log_len = 0;
    if (a[5] && copy_str(w, a[5], s_log_path, sizeof s_log_path) && jinxed_box_may_write(s_log_path)) {
        s_log_len = a[6];
    } else {
        s_log_path[0] = '\0';
    }
    *ret = s_host.sender_send_doc(name, range, (const char *)(uintptr_t)a[2], a[3],
                                  a[4] ? foot : NULL, log_from_card);
    s_log_path[0] = '\0';
    s_log_len = 0;
    return true;
}

/* ---- power ------------------------------------------------------------- */

H(c_power_idle_seconds) { (void)w; *ret = s_host.power_idle_seconds ? (uint32_t)s_host.power_idle_seconds((int)a[0]) : 0; return true; }

H(c_power_note_sent)
{
    char name[SEND_NAME_MAX];
    *ret = 0;
    if (s_host.power_note_sent && copy_str(w, a[0], name, sizeof name)) s_host.power_note_sent(name, a[1]);
    return true;
}
/* May not return: the board powers off. */
H(c_power_off_idle)
{
    char name[SEND_NAME_MAX];
    *ret = 0;
    if (s_host.power_off_idle && copy_str(w, a[0], name, sizeof name)) s_host.power_off_idle(name, a[1]);
    return true;
}
H(c_power_wake_line)
{
    *ret = 0;
    if (!s_host.power_wake_line || a[1] == 0 || !ok_ptr(w, a[0], a[1])) return true;
    char line[STATUS_MAX];
    const bool has = s_host.power_wake_line(line, sizeof line);
    if (has) put_str(w, a[0], a[1], line);
    *ret = has;
    return true;
}

/* ---- settings ---------------------------------------------------------- */

H(c_settings_draw_gear) { (void)w; (void)a; if (s_host.settings_draw_gear) s_host.settings_draw_gear(); *ret = 0; return true; }
H(c_settings_gear_w)    { (void)w; (void)a; *ret = s_host.settings_gear_w ? (uint32_t)s_host.settings_gear_w() : 0; return true; }
H(c_settings_gear_hit)
{
    (void)w;
    *ret = s_host.settings_gear_hit && s_host.settings_gear_hit((int)a[0], (int)a[1]);
    return true;
}
/* (buf, size): the attribution name, terminated, as much as fits; its
 * length, or 0 for none. */
H(c_jinxed_settings_author)
{
    *ret = 0;
    const char *n = s_host.settings_author ? s_host.settings_author() : NULL;
    if (!n || !put_str(w, a[0], a[1], n)) return true;
    *ret = (uint32_t)strlen((const char *)(uintptr_t)a[0]);
    return true;
}

/* The page's "send now", while it is up: recorded, and the page closes. */
static bool s_send_asked;
static bool page_send(void)
{
    s_send_asked = true;
    if (s_host.settings_close) s_host.settings_close();
    return true;
}
/* (): 1 if the page closed for "send now", 0 if it was left. Blocks for
 * as long as the page is up, so unlocked. */
H(c_jinxed_settings_page)
{
    (void)w;
    (void)a;
    *ret = 0;
    if (!s_host.settings_page) return true;
    s_send_asked = false;
    if (s_host.settings_on_send) s_host.settings_on_send(page_send);
    s_host.settings_page();
    *ret = s_send_asked;
    s_send_asked = false;
    return true;
}

/* ---- the display's settings ------------------------------------------- */

H(c_display_text_scale)     { (void)w; (void)a; *ret = s_host.display_text_scale ? (uint32_t)s_host.display_text_scale() : 0; return true; }
H(c_display_foot_scale)     { (void)w; (void)a; *ret = s_host.display_foot_scale ? (uint32_t)s_host.display_foot_scale() : 0; return true; }
H(c_display_set_text_scale) { (void)w; if (s_host.display_set_text_scale) s_host.display_set_text_scale((int)a[0]); *ret = 0; return true; }
H(c_display_markdown)       { (void)w; (void)a; *ret = s_host.display_markdown && s_host.display_markdown(); return true; }

/* ---- the table --------------------------------------------------------- */

static const uint32_t s_calls_v1 = 1;       /* jinxed_calls_v1 */

#define FN(n)  { #n, c_##n, NULL, 0, 0 }
#define FNU(n) { #n, c_##n, NULL, 0, FECKLESS_UMODE_CALL_UNLOCKED }

/* Locked unless they wait. kbd_get waits on the queue and touches no state
 * here; jinxed_settings_page is the page, seconds to minutes, and its
 * statics are only its own; power_off_idle may not return. */
static const feckless_umode_call_t s_own[] = {
    { "jinxed_calls_v1", NULL, &s_calls_v1, sizeof s_calls_v1, 0 },
    FNU(kbd_get), FN(kbd_present), FN(kbd_ascii),
    FN(sender_fetch_dict), FN(sender_fetched), FN(sender_sent), FN(sender_gen), FN(sender_status),
    FN(jinxed_send_doc),
    FN(power_idle_seconds), FN(power_note_sent), FNU(power_off_idle), FN(power_wake_line),
    FN(settings_draw_gear), FN(settings_gear_w), FN(settings_gear_hit),
    FN(jinxed_settings_author), FNU(jinxed_settings_page),
    FN(display_text_scale), FN(display_foot_scale), FN(display_set_text_scale), FN(display_markdown),
};

const feckless_umode_call_t *jinxed_box_own_calls(uint32_t *n)
{
    *n = sizeof s_own / sizeof s_own[0];
    return s_own;
}
