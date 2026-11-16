/*
 * redraw_host_test.c -- redraw_changed() leaves the panel as redraw()
 * would (u-jinxed-0024).
 *
 * main/editor.c is compiled whole (#included, for its statics), with
 * feckless-libs' real gfx.c under it, against that component's own host
 * shim for ESP-IDF, and stand-ins for the editor's other calls (the
 * card, the sender, the keyboard's ASCII). The panel is an image every
 * esp_lcd_panel_draw_bitmap() is copied into, the screen turned 90
 * degrees as the writer runs it.
 *
 * A script of keys goes through the editor's own key(): typing, Enter, a
 * line long enough to wrap, Backspace across the wrap, markdown (a
 * heading, bold, code, a strike, a bold run across a wrap whose marker is
 * deleted and typed again on the row above), arrows, a selection with Shift, a page
 * of Enters so the text scrolls, caps lock, a send's status line, the
 * theme, the text size, markdown off, the F1 sheet drawn over the
 * screen, select all and delete. After each
 * key, redraw_changed() draws as the loop would, and the panel is
 * copied; then a whole redraw() draws over it, and the panel must not
 * change. The signatures are then put back as redraw_changed() left
 * them, so the next key's partial redraw starts from its own state, not
 * a fresh one: drift between keys shows too.
 *
 * Usage: redraw_host_test <feckless-libs>; see test/run.sh.
 *
 * SPDX-License-Identifier: MIT
 */
/* u-jinxed-0025: a clock the loop's phase drives. The shim's own
 * xTaskGetTickCount() is 0 and its vTaskDelay() nothing. */
#include "shim.h"
static TickType_t test_now;
#define xTaskGetTickCount() (test_now)
#define vTaskDelay(t) ((void)(test_now += (TickType_t)(t)))

#include "../main/editor.c"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- the panel ------------------------------------------------------- */

#define PW 720
#define PH 1280
static uint16_t s_panel[PW * PH];
static unsigned long s_blit_px;

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x0, int y0, int x1, int y1, const void *data)
{
    (void)p;
    const uint16_t *d = data;
    const int w = x1 - x0;
    for (int y = y0; y < y1; y++) memcpy(&s_panel[(size_t)y * PW + x0], &d[(size_t)(y - y0) * w], (size_t)w * 2);
    s_blit_px += (unsigned long)w * (unsigned long)(y1 - y0);
    /* u-jinxed-0025: a blit costs what it did on a board, 60 ms a screen. */
    test_now += (TickType_t)((unsigned long)w * (unsigned long)(y1 - y0) * 60ul / ((unsigned long)PW * PH));
    return ESP_OK;
}
esp_err_t esp_lcd_dpi_panel_register_event_callbacks(esp_lcd_panel_handle_t p,
                                                     const esp_lcd_dpi_panel_event_callbacks_t *c, void *a)
{ (void)p; (void)c; (void)a; return ESP_OK; }
esp_err_t esp_lcd_dpi_panel_get_frame_buffer(esp_lcd_panel_handle_t p, uint32_t n, void **fb)
{ (void)p; (void)n; (void)fb; return ESP_FAIL; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "ESP_ERR"; }
void *heap_caps_malloc(size_t sz, uint32_t caps) { (void)caps; return calloc(1, sz); }
void *heap_caps_calloc(size_t n, size_t sz, uint32_t caps) { (void)caps; return calloc(n, sz); }
void *heap_caps_aligned_alloc(size_t al, size_t sz, uint32_t caps)
{ (void)caps; return aligned_alloc(al, (sz + al - 1) / al * al); }
void heap_caps_free(void *p) { free(p); }

/* ---- the theme and settings ------------------------------------------ */

static const display_theme_t k_dark = {
    .bg = 0x1082, .text = 0x9CD2, .dim = 0x528A, .cursor = 0x738E,
    .head = 0x18E4, .rule = 0x2966, .btn = 0x31A6, .field = 0x18C3, .edge = 0x4208, .go = 0x2A87, .bad = 0xA2CA,
};
static const display_theme_t k_light = {
    .bg = 0xEF7C, .text = 0x2965, .dim = 0x7BCE, .cursor = 0x4A69,
    .head = 0xDEFA, .rule = 0xC638, .btn = 0xD699, .field = 0xF7BE, .edge = 0xA514, .go = 0xA675, .bad = 0xB206,
};
const display_theme_t *display_col = &k_dark;
static int  s_t_scale = 3;
static bool s_t_md = true;
int  display_text_scale(void) { return s_t_scale; }
int  display_foot_scale(void) { return s_t_scale - 1 < 2 ? 2 : s_t_scale - 1; }
void display_set_text_scale(int sc) { s_t_scale = sc < 2 ? 2 : sc > 5 ? 5 : sc; }
bool display_markdown(void) { return s_t_md; }

/* ---- the rest of the board, quiet ------------------------------------- */

bool battery_charging(void) { return false; }
bool battery_external(void) { return true; }
esp_err_t battery_init(i2c_master_bus_handle_t bus) { (void)bus; return ESP_OK; }
int  battery_mv(void) { return 8300; }
int  battery_pct(void) { return 87; }
esp_err_t battery_start(void) { return ESP_OK; }
esp_err_t doclog_append(const char *o, uint32_t ol, const char *n, uint32_t nl, int64_t t, bool tok,
                        doclog_src_t src, const char *a)
{ (void)o; (void)ol; (void)n; (void)nl; (void)t; (void)tok; (void)src; (void)a; return ESP_OK; }
void doclog_close(void) {}
size_t doclog_copy_log(void *b, size_t c, bool *tb) { (void)b; (void)c; *tb = false; return 0; }
esp_err_t doclog_export(const char *t, uint32_t l, const char *a, int64_t n, bool nok, const char *b)
{ (void)t; (void)l; (void)a; (void)n; (void)nok; (void)b; return ESP_OK; }
uint32_t doclog_footer(char *f, const char *a, int64_t n, bool nok, const char *b)
{ (void)a; (void)n; (void)nok; (void)b; f[0] = '\0'; return 0; }
esp_err_t doclog_init(uint32_t cap) { (void)cap; return ESP_OK; }
bool doclog_is_open(void) { return true; }
esp_err_t doclog_open(const char *nm, char *t, uint32_t *l, int64_t n, bool nok)
{ (void)nm; (void)t; (void)n; (void)nok; *l = 0; return ESP_OK; }
uint32_t doclog_seq(void) { return 1; }
esp_err_t doclog_verify(void) { return ESP_OK; }
const esp_app_desc_t *esp_app_get_description(void) { return NULL; }
int64_t esp_timer_get_time(void) { return 0; }
bool files_last(char *n, size_t sz) { (void)n; (void)sz; return false; }
int  files_mode(void) { return 0; }
files_result_t files_page(char *c, size_t n, bool a, bool nn) { (void)c; (void)n; (void)a; (void)nn; files_result_t r; memset(&r, 0, sizeof r); return r; }
void files_set_last(const char *n) { (void)n; }
/* A sheet waits for a key: one is there when the script says so. */
static bool s_key_waiting;

/* u-jinxed-0025: the loop's phase. Presses and releases at given ticks;
 * kbd_get() waits for the next one as the queue would, moving the clock,
 * and ends the run (longjmp) once the script is done and the loop has
 * had a while to finish. */
typedef struct { TickType_t at; uint8_t usage; bool down; } scripted_t;
static scripted_t s_script[512];
static int s_script_n, s_script_i;
static TickType_t s_script_end;
static jmp_buf s_loop_done;

bool kbd_get(kbd_event_t *ev, uint32_t t)
{
    if (s_script_n) {
        if (s_script_i < s_script_n && s_script[s_script_i].at <= test_now + t) {
            if (s_script[s_script_i].at > test_now) test_now = s_script[s_script_i].at;
            *ev = (kbd_event_t){ .usage = s_script[s_script_i].usage, .down = s_script[s_script_i].down };
            s_script_i++;
            return true;
        }
        test_now += t;
        if (s_script_i == s_script_n && test_now > s_script_end) longjmp(s_loop_done, 1);
        return false;
    }
    if (!s_key_waiting) return false;
    s_key_waiting = false;
    *ev = (kbd_event_t){ .usage = KBD_ESCAPE, .down = true };
    return true;
}
bool kbd_present(void) { return true; }
esp_err_t lcd_backlight_set(int p) { (void)p; return ESP_OK; }
esp_err_t lcd_backlight_set_counts(uint32_t d) { (void)d; return ESP_OK; }
int  power_idle_seconds(int k) { (void)k; return 0; }
void power_note_sent(const char *n, uint32_t s) { (void)n; (void)s; }
void power_off_idle(const char *n, uint32_t s) { (void)n; (void)s; }
bool power_wake_line(char *l, size_t s) { (void)l; (void)s; return false; }
bool rtc8130_read(int64_t *e) { *e = 0; return false; }
bool sender_fetch_dict(void) { return false; }
bool sender_fetched(void) { return false; }
uint32_t sender_gen(void) { return 0; }
bool sender_send_doc(const char *n, const char *r, const char *t, uint32_t l, const char *f,
                     size_t (*lg)(void *, size_t, bool *))
{ (void)n; (void)r; (void)t; (void)l; (void)f; (void)lg; return true; }
bool sender_sent(void) { return true; }
bool sender_status(char *l, size_t s) { (void)l; (void)s; return false; }
const char *settings_author(void) { return NULL; }
void settings_draw_gear(void) { gfx_fill_rect(gfx_w() - 40, gfx_h() - 30, 24, 24, COL_DIM); }
bool settings_gear_hit(int x, int y) { (void)x; (void)y; return false; }
int  settings_gear_w(void) { return 50; }
void settings_on_send(bool (*s)(void)) { (void)s; }
void settings_page(void) {}
bool spell_add(const char *w, size_t n) { (void)w; (void)n; return false; }
void spell_case_like(const char *w, size_t n, char *s) { (void)w; (void)n; (void)s; }
void spell_ignore(const char *w, size_t n) { (void)w; (void)n; }
bool spell_known(const char *w, size_t n) { (void)w; (void)n; return true; }
bool spell_load(void) { return false; }
bool spell_next_word(const char *t, uint32_t l, uint32_t f, uint32_t *a, uint32_t *b)
{ (void)t; (void)l; (void)f; (void)a; (void)b; return false; }
int  spell_suggest(const char *w, size_t n, spell_word_t *o, int m) { (void)w; (void)n; (void)o; (void)m; return 0; }
bool storage_present(storage_id_t id) { (void)id; return true; }
uint32_t storage_generation(void) { return 1; }
i2c_master_bus_handle_t tab5io_bus(void) { return NULL; }
bool touch_get(int *x, int *y) { (void)x; (void)y; return false; }
bool touch_present(void) { return false; }
void touch_swallow(void) {}

/* US layout, enough for the script. */
char kbd_ascii(uint8_t u, uint8_t mods, bool caps)
{
    const bool sh = (mods & KBD_MOD_SHIFT) != 0;
    if (u >= 0x04 && u <= 0x1D) { const char c = (char)('a' + u - 0x04); return (sh != caps) ? (char)(c - 32) : c; }
    if (u >= 0x1E && u <= 0x27) return sh ? "!@#$%^&*()"[u - 0x1E] : "1234567890"[u - 0x1E];
    switch (u) {
    case 0x2C: return ' ';
    case 0x2D: return sh ? '_' : '-';
    case 0x2E: return sh ? '+' : '=';
    case 0x35: return sh ? '~' : '`';
    case 0x36: return sh ? '<' : ',';
    case 0x37: return sh ? '>' : '.';
    default:   return 0;
    }
}

/* ---- the script ------------------------------------------------------ */

static int checks, failures;
static unsigned long partial_px, full_px;
static int keys;

static uint16_t s_snap[PW * PH];

/* What the loop does after a key: redraw_changed(). Then the whole
 * redraw() must leave the panel and the shadow as they were; the
 * signatures go back as redraw_changed() left them. */
static void check_step(const char *what)
{
    s_blit_px = 0;
    redraw_changed();
    partial_px += s_blit_px;
    memcpy(s_snap, s_panel, sizeof s_panel);
    const bool ok = s_sig_ok;
    const uint32_t fr = s_sig_frame, st = s_sig_status;
    uint32_t rows[SIG_ROWS_MAX];
    memcpy(rows, s_sig_row, sizeof rows);
    const int top = s_top;

    s_blit_px = 0;
    redraw();
    full_px += s_blit_px;
    int diff = 0;
    for (size_t i = 0; i < (size_t)PW * PH; i++) diff += s_snap[i] != s_panel[i];
    checks++;
    if (diff) {
        failures++;
        printf("FAIL after %s (key %d): %d panel pixels differ from a whole redraw\n", what, keys, diff);
    }
    s_sig_ok = ok;
    s_sig_frame = fr;
    s_sig_status = st;
    memcpy(s_sig_row, rows, sizeof rows);
    s_top = top;
}

static void press(uint8_t usage, uint8_t mods, const char *what)
{
    kbd_event_t ev = { .usage = usage, .mods = mods, .down = true, .apple = false,
                       .cmd = (mods & KBD_MOD_CTRL) != 0 };
    key(&ev);
    keys++;
    check_step(what);
}

static uint8_t usage_of(char c, uint8_t *mods)
{
    *mods = 0;
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + c - 'a');
    if (c >= 'A' && c <= 'Z') { *mods = KBD_MOD_LSHIFT; return (uint8_t)(0x04 + c - 'A'); }
    if (c >= '1' && c <= '9') return (uint8_t)(0x1E + c - '1');
    switch (c) {
    case '0': return 0x27;
    case ' ': return 0x2C;
    case '\n': return KBD_ENTER;
    case '-': return 0x2D;
    case '.': return 0x37;
    case ',': return 0x36;
    case '`': return 0x35;
    case '~': *mods = KBD_MOD_LSHIFT; return 0x35;
    case '*': *mods = KBD_MOD_LSHIFT; return 0x25;
    case '#': *mods = KBD_MOD_LSHIFT; return 0x20;
    case '_': *mods = KBD_MOD_LSHIFT; return 0x2D;
    default:  return 0x2C;
    }
}

static void type(const char *s)
{
    for (; *s; s++) {
        uint8_t m;
        const uint8_t u = usage_of(*s, &m);
        char what[32];
        snprintf(what, sizeof what, "typing '%c'", *s == '\n' ? '/' : *s);
        press(u, m, what);
    }
}

/* ---- u-jinxed-0025: the loop itself ------------------------------------ */

static void add(TickType_t at, uint8_t usage, bool down)
{
    s_script[s_script_n++] = (scripted_t){ at, usage, down };
}

/* editor_run() from an empty page over the script, until it ends. */
static void run_loop(const char *what)
{
    s_len = 0;
    s_cur = 0;
    s_anchor = NO_ANCHOR;
    s_sig_ok = false;
    s_script_i = 0;
    s_script_end = s_script[s_script_n - 1].at + 2000;
    if (!setjmp(s_loop_done)) editor_run();
    s_script_n = 0;
    (void)what;
}

static int count_char(char c)
{
    int n = 0;
    for (uint32_t i = 0; i < s_len; i++) n += s_text[i] == c;
    return n;
}

static void loop_phase(void)
{
    int f = 0;
    char got[128];

    /* Typed at 8 keys a second, each held 70 ms: every key once. */
    {
        const char *t = "hello world";
        TickType_t at = test_now + 100;
        for (const char *c = t; *c; c++, at += 125) {
            uint8_t m;
            const uint8_t u = usage_of(*c, &m);
            add(at, u, true);
            add(at + 70, u, false);
        }
        run_loop("typed");
        snprintf(got, sizeof got, "%.*s", (int)s_len, s_text);
        if (strcmp(got, t)) { printf("FAIL loop: typed \"%s\", the page has \"%s\"\n", t, got); f++; }
    }
    /* Rolled over at 15 keys a second: the next pressed before the last
     * is released. */
    {
        const char *t = "quick brown fox";
        TickType_t at = test_now + 100;
        for (const char *c = t; *c; c++, at += 66) {
            uint8_t m;
            const uint8_t u = usage_of(*c, &m);
            add(at, u, true);
            add(at + 100, u, false);
        }
        /* releases are in the script out of order: sort by time */
        for (int i = 1; i < s_script_n; i++)
            for (int j = i; j > 0 && s_script[j].at < s_script[j - 1].at; j--) {
                const scripted_t x = s_script[j]; s_script[j] = s_script[j - 1]; s_script[j - 1] = x;
            }
        run_loop("rolled over");
        snprintf(got, sizeof got, "%.*s", (int)s_len, s_text);
        if (strcmp(got, t)) { printf("FAIL loop: rolled over \"%s\", the page has \"%s\"\n", t, got); f++; }
    }
    /* Held 1.2 s: it repeats, from 400 ms, every 35 ms or a redraw. */
    {
        add(test_now + 100, KBD_K, true);
        add(test_now + 1300, KBD_K, false);
        run_loop("held");
        const int k = count_char('k');
        if (k < 10 || k > 1 + (1200 - 400) / 35 + 1) {
            printf("FAIL loop: k held 1.2 s gave %d k\n", k);
            f++;
        }
        printf("redraw loop: %s; k held 1.2 s gave %d\n", f ? "FAILED" : "typed and rolled over exactly", k);
    }
    checks += 3;
    failures += f;
}

int main(void)
{
    if (gfx_init(NULL, PW, PH) != ESP_OK) { printf("gfx_init failed\n"); return 1; }
    gfx_set_rotation(GFX_ROT_90);
    s_text = heap_caps_malloc(TEXT_MAX, 0);
    s_ls = heap_caps_malloc((TEXT_MAX + 2) * sizeof(uint32_t), 0);
    s_le = heap_caps_malloc((TEXT_MAX + 2) * sizeof(uint32_t), 0);
    s_base = heap_caps_malloc(TEXT_MAX, 0);
    s_mds = heap_caps_malloc(TEXT_MAX, 0);
    s_clip = heap_caps_malloc(TEXT_MAX, 0);
    s_uops = heap_caps_malloc(UNDO_OPS * sizeof *s_uops, 0);
    s_uarena = heap_caps_malloc(UNDO_ARENA, 0);
    s_row_buf = heap_caps_malloc(1024, 0);
    snprintf(s_name, sizeof s_name, "notes.md");
    measure();
    redraw();                                   /* the first screen, whole */

    type("Hello world");
    type("\n# A heading\nsome **bold** and `code` and ~~struck~~ text");
    type("\nA line long enough that the editor has to wrap it onto the next row of the screen at fifty seven columns.");
    for (int i = 0; i < 12; i++) press(KBD_BACKSPACE, 0, "Backspace across the wrap");
    for (int i = 0; i < 3; i++) press(KBD_UP, 0, "Up");
    for (int i = 0; i < 4; i++) press(KBD_LEFT, 0, "Left");
    press(KBD_DOWN, 0, "Down");
    for (int i = 0; i < 6; i++) press(KBD_LEFT, KBD_MOD_LSHIFT, "Shift+Left");
    press(KBD_UP, KBD_MOD_LSHIFT, "Shift+Up");
    press(KBD_RIGHT, 0, "Right, ends the selection");
    press(KBD_END, KBD_MOD_LCTRL, "Ctrl+End");
    for (int i = 0; i < 20; i++) press(KBD_ENTER, 0, "Enter, scrolling");
    type("at the bottom");
    press(KBD_CAPSLOCK, 0, "caps lock");
    type("loud");
    press(KBD_CAPSLOCK, 0, "caps lock off");
    snprintf(s_send_line, sizeof s_send_line, "sending: 1 of 2 parts");
    check_step("a send's status line");
    s_send_line[0] = '\0';
    check_step("the status line cleared");
    display_col = &k_light;
    check_step("the theme");
    press(KBD_EQUAL, KBD_MOD_LCTRL, "Ctrl+= (text size)");
    type(" bigger");
    press(KBD_MINUS, KBD_MOD_LCTRL, "Ctrl+- (text size)");
    s_t_md = false;
    check_step("markdown off");
    press(KBD_HOME, KBD_MOD_LCTRL, "Ctrl+Home");
    type("**now plain**");
    s_t_md = true;
    check_step("markdown on");
    for (int i = 0; i < 25; i++) press(KBD_DOWN, 0, "Down, scrolling");
    press(KBD_PAGEUP, 0, "Page Up");
    /* A style that crosses a wrap: the second row's bytes do not change
     * when the first row's opening marker goes, but its styles do. */
    press(KBD_END, KBD_MOD_LCTRL, "Ctrl+End");
    type("\n**bold that runs on past the end of this row and onto the next row of the screen**");
    press(KBD_HOME, 0, "Home");
    for (int i = 0; i < 70; i++) press(KBD_LEFT, 0, "Left, to the wrapped line's start");
    press(KBD_HOME, 0, "Home");
    press(KBD_DELETE, 0, "Delete the first *, unbolding the next row");
    press(KBD_DELETE, 0, "Delete the second *");
    type("**");
    s_key_waiting = true;
    keys_sheet(false);                          /* F1: draws over, waits for a key */
    check_step("after the F1 sheet");
    press(KBD_A, KBD_MOD_LCTRL, "select all");
    press(KBD_BACKSPACE, 0, "delete all");
    type("again");

    printf("redraw: %d keys, %d checks, %d failures; blitted %.1f%% of what a whole redraw a key would\n",
           keys, checks, failures, full_px ? 100.0 * (double)partial_px / (double)full_px : 0.0);

    loop_phase();
    return failures ? 1 : 0;
}
