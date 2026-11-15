/* SPDX-License-Identifier: MIT
 *
 * Host test for jbox_storage.c (u-jinxed-0004, 0012): Jinxed's write
 * filter, installed in feckless_box_calls (u-feckless-0055). doclog.c's
 * sequence under ./jbox_root goes through; every write named outside it is
 * refused by feckless on the filter's word, with nothing made; every read
 * is let through. Over real files, through feckless_umode's ext hook,
 * against feckless_box_calls' own stand-ins. feckless's own test covers the
 * copy and the race.
 *
 *   make -C components/jinxed_box/test FECKLESS=<feckless-libs at umode>
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "feckless_box_calls.h"
#include "ff.h"
#include "jinxed_box.h"

/* feckless_box_calls' stand-ins record these. */
extern uintptr_t box_lo, box_hi;
extern int fat_open, io_open;
extern BYTE chmod_attr;
extern char hidden_path[300];

static int failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failed++; } } while (0)

#define MEM (256 * 1024)
#define M1 ((uint32_t)-1)
static uint8_t *mem;
static feckless_box_t box;
static feckless_umode_window_t w;
static feckless_umode_calls_t tab;
static const feckless_umode_call_t *calls_tab;
static uint32_t ncalls;
static int *eno;

static int nr_index(const char *name)
{
    for (uint32_t i = 0; i < ncalls; i++)
        if (!strcmp(calls_tab[i].name, name)) return (int)i;
    return -1;
}
static uint32_t call(const char *name, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    const int i = nr_index(name);
    if (i < 0) { printf("FAIL no call %s\n", name); failed++; return 0; }
    w.ctx.x[10] = a0; w.ctx.x[11] = a1; w.ctx.x[12] = a2; w.ctx.x[13] = a3;
    uint32_t ret = 0xdeadbeef;
    CHECK(feckless_umode_calls_ext(&w, FECKLESS_SYS_EXT_BASE + (uint32_t)i, &w.ctx.x[10], &ret));
    return ret;
}
static uint32_t P(const void *p) { return (uint32_t)(uintptr_t)p; }
/* Seven arguments: a0..a6. */
static uint32_t call7(const char *name, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                       uint32_t a4, uint32_t a5, uint32_t a6)
{
    w.ctx.x[14] = a4; w.ctx.x[15] = a5; w.ctx.x[16] = a6;
    return call(name, a0, a1, a2, a3);
}
static void *box_alloc(uint32_t n) { void *p = (void *)(uintptr_t)feckless_box_alloc(&box, n); memset(p, 0, n); return p; }
static char *put_str(const char *s) { char *p = box_alloc((uint32_t)strlen(s) + 1); strcpy(p, s); return p; }
static bool exists(const char *p) { return access(p, F_OK) == 0; }

/* u-jinxed-0012: feckless's own table, the filter installed with it. */
static const char *s_answers;                  /* jinxed_calls.txt */

static void test_table(void)
{
    uint32_t fn = 0, on = 0, n2 = 0;
    const feckless_umode_call_t *f = feckless_box_calls_v1(&fn);
    const feckless_umode_call_t *o = jinxed_box_own_calls(&on);
    CHECK(jinxed_box_calls(&n2) == calls_tab && n2 == ncalls);     /* built once */
    CHECK(ncalls == fn + on);
    for (uint32_t i = 0; i < fn; i++) {                           /* feckless's, as it is */
        CHECK(!strcmp(calls_tab[i].name, f[i].name) && calls_tab[i].fn == f[i].fn &&
              calls_tab[i].data == f[i].data && calls_tab[i].flags == f[i].flags);
    }
    for (uint32_t i = 0; i < on; i++) {                           /* then Jinxed's */
        CHECK(!strcmp(calls_tab[fn + i].name, o[i].name) && calls_tab[fn + i].fn == o[i].fn);
        for (uint32_t j = 0; j < fn; j++) {
            if (!strcmp(o[i].name, f[j].name)) { printf("FAIL own call %s is feckless's\n", o[i].name); failed++; }
        }
    }
    /* jinxed_calls_v1: data, version 1, first */
    CHECK(!strcmp(o[0].name, "jinxed_calls_v1") && o[0].fn == NULL && o[0].data_len == 4 &&
          *(const uint32_t *)o[0].data == 1);
    /* jinxed_calls.txt: the symbol, and exactly the other own names */
    FILE *a = fopen(s_answers, "r");
    CHECK(a != NULL);
    char line[128];
    int names = 0, sym = 0;
    while (a && fgets(line, sizeof line, a)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "@symbol ", 8)) { sym += !strcmp(line + 8, o[0].name); continue; }
        if (!line[0] || line[0] == '#') continue;
        names++;
        bool found = false;
        for (uint32_t i = 1; i < on; i++) found |= !strcmp(line, o[i].name) && o[i].fn != NULL;
        if (!found) { printf("FAIL jinxed_calls.txt names %s, not an own call\n", line); failed++; }
    }
    if (a) fclose(a);
    CHECK(sym == 1 && names == (int)on - 1);
}

/* ---- u-jinxed-0013: the own calls, against stand-ins ------------------ */
static kbd_event_t h_ev = { .usage = 0x04, .mods = 0x02, .down = true, .apple = false, .cmd = true };
static bool h_kbd_has = true;
static uint32_t h_ticks;
static bool h_kbd_get(kbd_event_t *ev, uint32_t ticks) { h_ticks = ticks; if (h_kbd_has) *ev = h_ev; return h_kbd_has; }
static bool h_kbd_present(void) { return true; }
static uint8_t h_asc_u, h_asc_m; static bool h_asc_c;
static char h_kbd_ascii(uint8_t u, uint8_t m, bool c) { h_asc_u = u; h_asc_m = m; h_asc_c = c; return 'A'; }
static bool h_true(void) { return true; }
static uint32_t h_gen(void) { return 77; }
static bool h_status(char *line, size_t size) { snprintf(line, size, "sending: 3 of 5 parts done"); return true; }
/* As the sender does: copied during the call. Whether each pointer was the
 * box's is kept, since the copies are the handler's and gone after. */
static bool h_sent;
static char h_send_name[64], h_send_range[40], h_send_foot[160];
static bool h_name_boxed, h_range_boxed, h_foot_boxed, h_foot_null;
static const char *h_send_text;
static uint32_t h_send_len;
static bool in_box(const void *p);
static size_t h_log_got; static bool h_log_big;
static char h_logbuf[64];
static bool h_send_doc(const char *name, const char *range, const char *text, uint32_t len,
                       const char *foot, size_t (*log)(void *buf, size_t cap, bool *too_big))
{
    h_sent = true;
    snprintf(h_send_name, sizeof h_send_name, "%s", name);
    snprintf(h_send_range, sizeof h_send_range, "%s", range);
    snprintf(h_send_foot, sizeof h_send_foot, "%s", foot ? foot : "");
    h_name_boxed = in_box(name); h_range_boxed = in_box(range); h_foot_boxed = foot && in_box(foot);
    h_foot_null = foot == NULL;
    h_send_text = text; h_send_len = len;
    h_log_got = log ? log(h_logbuf, sizeof h_logbuf, &h_log_big) : 0;
    return true;
}
static int h_idle(int kind) { return 100 + kind; }
static char h_noted[64]; static uint32_t h_noted_seq;
static void h_note_sent(const char *name, uint32_t seq) { snprintf(h_noted, sizeof h_noted, "%s", name); h_noted_seq = seq; }
static bool h_wake(char *line, size_t size) { snprintf(line, size, "woke at 07:00"); return true; }
static int h_gear_drawn;
static void h_draw_gear(void) { h_gear_drawn++; }
static int h_gear_w(void) { return 48; }
static int h_hit_x, h_hit_y;
static bool h_gear_hit(int x, int y) { h_hit_x = x; h_hit_y = y; return x > 600; }
static const char *h_author_name = "Ada Lovelace, Countess of Lovelace";
static const char *h_author(void) { return h_author_name; }
static bool (*h_send_cb)(void);
static void h_on_send(bool (*send)(void)) { h_send_cb = send; }
static bool h_page_presses;                 /* the page presses "send now" */
static int h_closed;
static void h_close(void) { h_closed++; }
static void h_page(void) { if (h_page_presses && h_send_cb) h_send_cb(); }
static int h_scale = 2;
static int h_text_scale(void) { return h_scale; }
static int h_foot_scale(void) { return 1; }
static void h_set_text_scale(int s) { h_scale = s; }
static bool h_md(void) { return true; }
typedef struct { uint16_t bg, text, dim; } h_theme_t;    /* any layout: copied whole */
static const h_theme_t h_themes[2] = { { 1, 2, 3 }, { 4, 5, 6 } };
static const h_theme_t *h_col = &h_themes[0];

static const jinxed_host_t s_fake_host = {
    .kbd_get = h_kbd_get, .kbd_present = h_kbd_present, .kbd_ascii = h_kbd_ascii,
    .sender_fetch_dict = h_true, .sender_fetched = h_true, .sender_sent = h_true,
    .sender_gen = h_gen, .sender_status = h_status, .sender_send_doc = h_send_doc,
    .power_idle_seconds = h_idle, .power_note_sent = h_note_sent, .power_off_idle = h_note_sent,
    .power_wake_line = h_wake,
    .settings_draw_gear = h_draw_gear, .settings_gear_w = h_gear_w, .settings_gear_hit = h_gear_hit,
    .settings_author = h_author, .settings_on_send = h_on_send, .settings_page = h_page,
    .settings_close = h_close,
    .display_text_scale = h_text_scale, .display_foot_scale = h_foot_scale,
    .display_set_text_scale = h_set_text_scale, .display_markdown = h_md,
    .display_col = (const void *const *)&h_col, .display_col_size = sizeof(h_theme_t),
};

static bool in_box(const void *p) { return (uintptr_t)p >= w.lo && (uintptr_t)p < w.hi; }

static void test_own_calls(void)
{
    jinxed_box_host(&s_fake_host);

    /* the keyboard */
    kbd_event_t *ev = box_alloc(sizeof *ev);
    CHECK(call("kbd_get", P(ev), 50, 0, 0) == 1 && h_ticks == 50);
    CHECK(ev->usage == 0x04 && ev->mods == 0x02 && ev->down && ev->cmd && !ev->apple);
    CHECK(call("kbd_get", P(&h_ev), 50, 0, 0) == 0);             /* not the box's */
    CHECK(call("kbd_get", P((char *)mem + MEM - 2), 50, 0, 0) == 0);
    h_kbd_has = false;
    memset(ev, 0xEE, sizeof *ev);
    CHECK(call("kbd_get", P(ev), 0, 0, 0) == 0 && ev->usage == 0xEE);   /* nothing written */
    h_kbd_has = true;
    CHECK(call("kbd_present", 0, 0, 0, 0) == 1);
    CHECK(call("kbd_ascii", 0x104, 0x12, 1, 0) == 'A' && h_asc_u == 0x04 && h_asc_m == 0x12 && h_asc_c);

    /* the sender */
    CHECK(call("sender_fetch_dict", 0, 0, 0, 0) == 1 && call("sender_fetched", 0, 0, 0, 0) == 1);
    CHECK(call("sender_sent", 0, 0, 0, 0) == 1 && call("sender_gen", 0, 0, 0, 0) == 77);
    char *st = box_alloc(64);
    CHECK(call("sender_status", P(st), 64, 0, 0) == 1 && !strcmp(st, "sending: 3 of 5 parts done"));
    CHECK(call("sender_status", P(st), 8, 0, 0) == 1 && !strcmp(st, "sending"));   /* fits, terminated */
    CHECK(call("sender_status", P(st), 0, 0, 0) == 0);
    CHECK(call("sender_status", P((char *)mem + MEM - 4), 64, 0, 0) == 0);

    /* jinxed_send_doc: the strings copied, the text the box's, the log read from the card */
    FILE *lf = fopen("jbox_root/.note.md.h0", "wb");
    fputs("JINXLOG-RECORDS-AND-MORE", lf);
    fclose(lf);
    char *name = put_str("note.md"), *range = put_str("seq 1-9"), *foot = put_str("-- Ada");
    char *text = put_str("# some words\n");
    char *lp = put_str("jbox_root/.note.md.h0");
    CHECK(call7("jinxed_send_doc", P(name), P(range), P(text), 13, P(foot), P(lp), 7) == 1);
    CHECK(!h_name_boxed && !strcmp(h_send_name, "note.md"));       /* the host's copies */
    CHECK(!h_range_boxed && !strcmp(h_send_range, "seq 1-9"));
    CHECK(!h_foot_boxed && !h_foot_null && !strcmp(h_send_foot, "-- Ada"));
    CHECK(h_send_text == text && h_send_len == 13);
    CHECK(h_log_got == 7 && !h_log_big && !memcmp(h_logbuf, "JINXLOG", 7));
    CHECK(call7("jinxed_send_doc", P(name), P(range), P(text), 13, 0, P(lp), 100) == 1);
    CHECK(h_foot_null && h_log_got == 0 && h_log_big);   /* past the host's buffer: too big */
    char *outside = put_str("elsewhere/kept.txt");
    CHECK(call7("jinxed_send_doc", P(name), P(range), P(text), 13, P(foot), P(outside), 5) == 1);
    CHECK(h_log_got == 0 && !h_log_big);                          /* outside the root: no log */
    CHECK(call7("jinxed_send_doc", P(name), P(range), P(text), 13, P(foot), 0, 5) == 1 && h_log_got == 0);
    h_sent = false;
    CHECK(call7("jinxed_send_doc", P("host"), P(range), P(text), 13, P(foot), P(lp), 7) == 0 && !h_sent);
    CHECK(call7("jinxed_send_doc", P(name), P(range), P(text), MEM, P(foot), P(lp), 7) == 0 && !h_sent);
    char *long_name = box_alloc(100);
    memset(long_name, 'n', 99);
    CHECK(call7("jinxed_send_doc", P(long_name), P(range), P(text), 13, P(foot), P(lp), 7) == 0 && !h_sent);

    /* power */
    CHECK(call("power_idle_seconds", 2, 0, 0, 0) == 102);
    CHECK(call("power_note_sent", P(name), 9, 0, 0) == 0 && !strcmp(h_noted, "note.md") && h_noted_seq == 9);
    h_noted[0] = '\0';
    CHECK(call("power_note_sent", P("host"), 9, 0, 0) == 0 && h_noted[0] == '\0');
    CHECK(call("power_off_idle", P(name), 11, 0, 0) == 0 && h_noted_seq == 11);
    CHECK(call("power_wake_line", P(st), 64, 0, 0) == 1 && !strcmp(st, "woke at 07:00"));

    /* settings */
    CHECK(call("settings_draw_gear", 0, 0, 0, 0) == 0 && h_gear_drawn == 1);
    CHECK(call("settings_gear_w", 0, 0, 0, 0) == 48);
    CHECK(call("settings_gear_hit", 700, 20, 0, 0) == 1 && h_hit_x == 700 && h_hit_y == 20);
    CHECK(call("settings_gear_hit", (uint32_t)-5, 20, 0, 0) == 0 && h_hit_x == -5);
    char *au = box_alloc(48);
    CHECK(call("jinxed_settings_author", P(au), 48, 0, 0) == strlen(h_author_name) && !strcmp(au, h_author_name));
    CHECK(call("jinxed_settings_author", P(au), 4, 0, 0) == 3 && !strcmp(au, "Ada"));
    CHECK(call("jinxed_settings_author", P((char *)mem + MEM - 2), 48, 0, 0) == 0);
    h_author_name = NULL;
    CHECK(call("jinxed_settings_author", P(au), 48, 0, 0) == 0);
    h_page_presses = false;
    h_closed = 0;
    CHECK(call("jinxed_settings_page", 0, 0, 0, 0) == 0 && h_closed == 0);   /* left */
    h_page_presses = true;
    CHECK(call("jinxed_settings_page", 0, 0, 0, 0) == 1 && h_closed == 1);   /* send now: closed */
    h_page_presses = false;
    CHECK(call("jinxed_settings_page", 0, 0, 0, 0) == 0);                    /* not remembered */

    /* the display's settings */
    CHECK(call("display_text_scale", 0, 0, 0, 0) == 2 && call("display_foot_scale", 0, 0, 0, 0) == 1);
    CHECK(call("display_set_text_scale", 3, 0, 0, 0) == 0 && h_scale == 3);
    CHECK(call("display_markdown", 0, 0, 0, 0) == 1);

    /* u-jinxed-0016: the theme, by value, as display_col points now */
    h_theme_t *th = box_alloc(sizeof *th);
    CHECK(call("jinxed_display_col", P(th), sizeof *th, 0, 0) == 1 && th->bg == 1 && th->dim == 3);
    h_col = &h_themes[1];                                         /* the page chose another */
    CHECK(call("jinxed_display_col", P(th), sizeof *th, 0, 0) == 1 && th->bg == 4 && th->dim == 6);
    memset(th, 0xEE, sizeof *th);
    CHECK(call("jinxed_display_col", P(th), sizeof *th - 2, 0, 0) == 0 && th->bg == 0xEEEE);   /* another layout */
    CHECK(call("jinxed_display_col", P(th), sizeof *th + 2, 0, 0) == 0 && th->bg == 0xEEEE);
    CHECK(call("jinxed_display_col", P(&h_themes[0]), sizeof *th, 0, 0) == 0);           /* not the box's */
    CHECK(call("jinxed_display_col", P((char *)mem + MEM - 2), sizeof *th, 0, 0) == 0);

    /* NVS: "jinxed" only */
    char *ns_j = put_str("jinxed"), *ns_m = put_str("mail"), *key = put_str("last");
    CHECK(call("nvsh_set_u8", P(ns_j), P(key), 5, 0) == 0);
    CHECK(call("nvsh_set_u8", P(ns_m), P(key), 5, 0) != 0);

    /* no host functions: every own call answers 0 */
    jinxed_box_host(NULL);
    CHECK(call("kbd_get", P(ev), 0, 0, 0) == 0 && call("sender_gen", 0, 0, 0, 0) == 0);
    CHECK(call("jinxed_settings_page", 0, 0, 0, 0) == 0 && call("display_markdown", 0, 0, 0, 0) == 0);
    CHECK(call("jinxed_display_col", P(th), sizeof *th, 0, 0) == 0 && th->bg == 0xEEEE);
    CHECK(call7("jinxed_send_doc", P(name), P(range), P(text), 13, P(foot), P(lp), 7) == 0);
}

/* The filter alone. */
static void test_filter_rule(void)
{
    static const char *const yes[] = {
        "jbox_root", "jbox_root/", "jbox_root/a.md", "jbox_root//a", "jbox_root/..notes.md",
        "jbox_root/a/b/.c.h0", "jbox_root/.../x",
    };
    static const char *const no[] = {
        "", "/", "jbox_roo", "jbox_rootx", "jbox_rootx/a", "/jbox_root/a", "../jbox_root/a",
        "jbox_root/..", "jbox_root/../x", "jbox_root/a/..", "jbox_root//..//x", "0:/jinxed/a",
        "elsewhere/new",
    };
    for (size_t i = 0; i < sizeof yes / sizeof *yes; i++) {
        if (!jinxed_box_may_write(yes[i])) { printf("FAIL refused %s\n", yes[i]); failed++; }
    }
    for (size_t i = 0; i < sizeof no / sizeof *no; i++) {
        if (jinxed_box_may_write(no[i])) { printf("FAIL allowed %s\n", no[i]); failed++; }
    }
}

/* doclog.c, inside the root: every call reaches feckless. */
static void test_inside(void)
{
    char *dir = put_str("jbox_root");
    char *twin = put_str("jbox_root/.note.md.h0");
    char *tmp = put_str("jbox_root/note.md.tmp");
    char *exp = put_str("jbox_root/note.md");
    char *wb = put_str("wb"), *rpb = put_str("r+b"), *ab = put_str("ab"), *rb = put_str("rb");
    struct stat *st = box_alloc(sizeof *st);

    CHECK(call("mkdir", P(dir), 0777, 0, 0) == 0 && exists("jbox_root"));
    *eno = 0;
    CHECK(call("mkdir", P(dir), 0777, 0, 0) == M1 && *eno == EEXIST);       /* feckless's errno */

    uint8_t *rec = box_alloc(9000);
    uint32_t x = 1;
    for (int i = 0; i < 9000; i++) rec[i] = (uint8_t)((x = x * 1103515245u + 12345u) >> 16);
    const uint32_t f = call("fopen", P(twin), P(wb), 0, 0);
    CHECK(f != 0);
    CHECK(call("fwrite", P(rec), 1, 9000, f) == 9000 && call("ferror", f, 0, 0, 0) == 0);
    CHECK(call("fflush", f, 0, 0, 0) == 0 && call("fsync", call("fileno", f, 0, 0, 0), 0, 0, 0) == 0);
    CHECK(call("fclose", f, 0, 0, 0) == 0);
    hidden_path[0] = '\0';
    call("storage_mark_hidden", P(twin), 0, 0, 0);
    CHECK(!strcmp(hidden_path, "jbox_root/.note.md.h0"));
    const uint32_t g = call("fopen", P(twin), P(rpb), 0, 0);
    CHECK(g != 0 && call("fseek", g, 100, SEEK_SET, 0) == 0 && call("fwrite", P(rec), 1, 10, g) == 10);
    CHECK(call("fclose", g, 0, 0, 0) == 0);
    const uint32_t h = call("fopen", P(twin), P(ab), 0, 0);
    CHECK(h != 0 && call("fwrite", P(rec), 1, 6, h) == 6 && call("fclose", h, 0, 0, 0) == 0);
    CHECK(call("stat", P(twin), P(st), 0, 0) == 0 && st->st_size == 9006);

    uint32_t e = call("fopen", P(exp), P(wb), 0, 0);
    CHECK(call("fwrite", P(rec), 1, 3, e) == 3 && call("fclose", e, 0, 0, 0) == 0);
    e = call("fopen", P(tmp), P(wb), 0, 0);
    CHECK(call("fwrite", P(rec), 1, 7, e) == 7 && call("fclose", e, 0, 0, 0) == 0);
    CHECK(call("unlink", P(exp), 0, 0, 0) == 0);
    CHECK(call("rename", P(tmp), P(exp), 0, 0) == 0);
    CHECK(call("stat", P(exp), P(st), 0, 0) == 0 && st->st_size == 7);
    struct utimbuf *ut = box_alloc(sizeof *ut);
    ut->actime = ut->modtime = 1000000000;
    CHECK(call("utime", P(exp), P(ut), 0, 0) == 0);
    CHECK(call("stat", P(exp), P(st), 0, 0) == 0 && st->st_mtime == 1000000000);
    const uint32_t r = call("fopen", P(exp), P(rb), 0, 0);
    CHECK(r != 0 && call("fclose", r, 0, 0, 0) == 0);

    char *trash = put_str("jbox_root/trash");
    CHECK(call("mkdir", P(trash), 0777, 0, 0) == 0);
    char *in_trash = put_str("jbox_root/trash/note.md");
    CHECK(call("rename", P(exp), P(in_trash), 0, 0) == 0 && exists("jbox_root/trash/note.md"));
    CHECK(call("remove", P(in_trash), 0, 0, 0) == 0);
    CHECK(call("rmdir", P(trash), 0, 0, 0) == 0 && !exists("jbox_root/trash"));
    /* ".." inside a longer name is a name */
    char *dots = put_str("jbox_root/..notes.md");
    const uint32_t d = call("fopen", P(dots), P(wb), 0, 0);
    CHECK(d != 0 && call("fclose", d, 0, 0, 0) == 0);
    /* storage_io's files, for writing, inside */
    char *io = put_str("jbox_root/io.bin");
    const uint32_t s = call("storage_io_open", P(io), P(wb), 0, 0);
    CHECK(s != 0 && io_open == 1 && call("storage_io_close", s, 0, 0, 0) == 0 && io_open == 0);
}

/* Outside the root: every write refused, nothing made; reads let through. */
static void test_outside(void)
{
    if (mkdir("elsewhere", 0700) != 0) {}
    FILE *o = fopen("elsewhere/kept.txt", "w");
    fputs("kept\n", o);
    fclose(o);

    static const char *const outside[] = {
        "elsewhere/new", "jbox_rootx/a", "jbox_roo", "jbox_root/../elsewhere/new",
        "jbox_root/a/..", "jbox_root//..//elsewhere/new", "../jbox_root/a", "/jbox_root/a", "",
    };
    static const char *const writes[] = { "w", "wb", "a", "ab", "r+", "r+b", "rb+", "w+", "x" };
    struct stat *st = box_alloc(sizeof *st);
    struct utimbuf *ut = box_alloc(sizeof *ut);
    char *kept = put_str("elsewhere/kept.txt");
    char *inside = put_str("jbox_root/note.md");
    for (size_t i = 0; i < sizeof outside / sizeof *outside; i++) {
        char *p = put_str(outside[i]);
        for (size_t m = 0; m < sizeof writes / sizeof *writes; m++) {
            char *mode = put_str(writes[m]);
            *eno = 0;
            CHECK(call("fopen", P(p), P(mode), 0, 0) == 0 && *eno == EACCES);
            *eno = 0;
            CHECK(call("storage_io_open", P(p), P(mode), 0, 0) == 0 && *eno == EACCES);
        }
        *eno = 0;
        CHECK(call("mkdir", P(p), 0777, 0, 0) == M1 && *eno == EACCES);
        CHECK(call("rmdir", P(p), 0, 0, 0) == M1);
        CHECK(call("remove", P(p), 0, 0, 0) == M1);
        CHECK(call("unlink", P(p), 0, 0, 0) == M1);
        CHECK(call("utime", P(p), P(ut), 0, 0) == M1);
        *eno = 0;
        CHECK(call("rename", P(inside), P(p), 0, 0) == M1 && *eno == EACCES);
        CHECK(call("rename", P(p), P(inside), 0, 0) == M1);
        hidden_path[0] = '\0';
        call("storage_mark_hidden", P(p), 0, 0, 0);
        CHECK(hidden_path[0] == '\0');                              /* never reached storage */
    }
    CHECK(!exists("elsewhere/new") && !exists("jbox_rootx") && !exists("jbox_roo"));
    CHECK(io_open == 0);

    /* existing files outside: not removed, renamed, touched or truncated */
    CHECK(call("remove", P(kept), 0, 0, 0) == M1 && call("unlink", P(kept), 0, 0, 0) == M1);
    CHECK(call("rename", P(kept), P(inside), 0, 0) == M1);
    CHECK(call("fopen", P(kept), P(put_str("w")), 0, 0) == 0);
    struct stat hs;
    CHECK(stat("elsewhere/kept.txt", &hs) == 0 && hs.st_size == 5);

    /* reads anywhere */
    const uint32_t r = call("fopen", P(kept), P(put_str("r")), 0, 0);
    CHECK(r != 0);
    char *line = box_alloc(16);
    CHECK(call("fgets", P(line), 16, r, 0) == P(line) && !strcmp(line, "kept\n"));
    CHECK(call("fclose", r, 0, 0, 0) == 0);
    const uint32_t r2 = call("storage_io_open", P(kept), P(put_str("rb")), 0, 0);
    CHECK(r2 != 0 && call("storage_io_close", r2, 0, 0, 0) == 0);
    CHECK(call("stat", P(kept), P(st), 0, 0) == 0 && st->st_size == 5);
    const uint32_t dd = call("opendir", P(put_str("elsewhere")), 0, 0, 0);
    CHECK(dd != 0 && call("readdir", dd, 0, 0, 0) != 0 && call("closedir", dd, 0, 0, 0) == 0);

    /* FatFs: reading passes, any writing or creating mode is refused */
    FIL *fp = box_alloc(sizeof(FIL));
    CHECK(call("f_open", P(fp), P(kept), FA_READ, 0) == FR_OK && fat_open == 1);
    CHECK(call("f_close", P(fp), 0, 0, 0) == FR_OK && fat_open == 0);
    for (uint32_t bit = 0x02; bit <= 0x10; bit <<= 1) {
        CHECK(call("f_open", P(fp), P(kept), FA_READ | bit, 0) == FR_DENIED);
        /* On the board FatFs's paths are "0:/...", which the filter never
         * allows; here the stand-in takes this project's paths, so a
         * drive-form one stands for them. */
        CHECK(call("f_open", P(fp), P(put_str("0:/jinxed/note.md")), bit, 0) == FR_DENIED);
    }
    CHECK(call("f_open", P(fp), P(kept), 0x30, 0) == FR_DENIED);     /* FA_OPEN_APPEND */
    CHECK(fat_open == 0);
    chmod_attr = 0;
    CHECK(call("f_chmod", P(kept), 0x02, 0x02, 0) == FR_DENIED && chmod_attr == 0);

    /* paths that are not strings in the box: EFAULT, and nothing done */
    *eno = 0;
    CHECK(call("mkdir", P("jbox_root/host"), 0777, 0, 0) == M1 && *eno == EFAULT);
    char *edge = (char *)mem + MEM - 4;
    memcpy(edge, "jbox", 4);
    *eno = 0;
    CHECK(call("unlink", P(edge), 0, 0, 0) == M1 && *eno == EFAULT);
    char *lng = box_alloc(400);
    memcpy(lng, "jbox_root/", 10);
    memset(lng + 10, 'a', 300);
    *eno = 0;
    /* u-jinxed-0012: feckless takes paths to 1024 bytes, and this one is
     * under the root, so the filter allows it; the file system refuses a
     * 300-byte name, and the app gets the host's errno for that. */
    CHECK(call("fopen", P(lng), P(put_str("wb")), 0, 0) == 0 && *eno == ENAMETOOLONG);
    /* a mode that is not a string counts as writing: the path decides */
    *eno = 0;
    CHECK(call("fopen", P(put_str("elsewhere/new")), P(edge), 0, 0) == 0 && *eno == EFAULT);  /* feckless refuses the mode first */
    CHECK(!exists("elsewhere/new"));

    /* errno with tp outside the box is not written */
    int *outside_tp = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(outside_tp != MAP_FAILED);
    if (outside_tp != MAP_FAILED) {
        const uint32_t tp = w.ctx.x[4];
        *outside_tp = 77;
        w.ctx.x[4] = P(outside_tp);
        CHECK(call("unlink", P(kept), 0, 0, 0) == M1 && *outside_tp == 77);
        w.ctx.x[4] = tp;
        munmap(outside_tp, 4096);
    }
}

int main(int argc, char **argv)
{
    s_answers = argc > 1 ? argv[1] : "../../jinxed_calls.txt";
    mem = mmap(NULL, MEM, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (mem == MAP_FAILED) { printf("no memory below 4 GB\n"); return 1; }
    feckless_box_init(&box, (uintptr_t)mem, MEM, 4096);
    calls_tab = jinxed_box_calls(&ncalls);
    CHECK(calls_tab != NULL);
    if (!calls_tab) return 1;
    tab = (feckless_umode_calls_t){ calls_tab, ncalls, &box };
    w.lo = (uintptr_t)mem;
    w.hi = (uintptr_t)mem + MEM;
    w.ext_arg = &tab;
    box_lo = w.lo;
    box_hi = w.hi;
    eno = box_alloc(sizeof *eno);                                   /* the thread's errno block */
    w.ctx.x[4] = P(eno);
    test_table();
    test_filter_rule();
    test_inside();
    test_outside();
    test_own_calls();

    /* The app's end forgets the filter: a write outside goes through, and
     * jinxed_box_calls() puts it back for the next load. */
    feckless_box_calls_end();
    const uint32_t f = call("fopen", P(put_str("elsewhere/after")), P(put_str("w")), 0, 0);
    CHECK(f != 0 && call("fclose", f, 0, 0, 0) == 0 && exists("elsewhere/after"));
    uint32_t n3 = 0;
    jinxed_box_calls(&n3);
    *eno = 0;
    CHECK(call("fopen", P(put_str("elsewhere/again")), P(put_str("w")), 0, 0) == 0 && *eno == EACCES);
    feckless_box_calls_end();
    printf(failed ? "%d FAILED\n" : "jinxed_box: all passed\n", failed);
    return failed != 0;
}
