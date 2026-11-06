/*
 * settings.c -- the gear and the settings page. See settings.h.
 *
 * The page is laid out in content coordinates -- y from the top of the
 * first section -- and drawn shifted up by the scroll, between a fixed
 * header and the bottom edge. layout() is the one place positions are
 * decided; drawing and hit-testing both call it, so a button is always
 * tapped where it was drawn.
 *
 * Touch: a press that moves more than DRAG_PX is a drag and scrolls; one
 * that lifts without moving that far is a tap, delivered where it went
 * down.
 *
 * SPDX-License-Identifier: MIT
 */
#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch.h"

#include "display.h"
#include "kbd.h"
#include "libversions.h"
#include "mailcfg.h"
#include "mailform.h"
#include "sender.h"

static const char *TAG = "settings";

#define SCALE       (2)             /* body text: 24 px glyphs */
#define TITLE_SCALE (3)
#define PAD         (48)
#define HEAD_H      (88)
#define TEXT_H      GFX_GLYPH_H(SCALE)
#define LINE_H      (TEXT_H + 12)
#define BTN_H       (64)
#define SECTION_GAP (48)
#define KEY_STEP    (LINE_H * 2)
#define DRAG_PX     (12)
#define POLL_MS     (20)

/* The gear: drawn in the status line, right corner. Its sizes are for
 * the footer's text at scale 2, and grow with it (0022): GZ() scales one. */
#define GEAR_R      (11)
#define GEAR_HIT_W  (140)
#define GEAR_HIT_H  (90)
#define GZ(n)       ((n) * display_foot_scale() / 2)

static int gear_cx(void) { return gfx_w() - 40 / 2 - GZ(GEAR_R); }
static int gear_cy(void) { return gfx_h() - 16 / 2 - GFX_GLYPH_H(display_foot_scale()) / 2 - 8; }

int settings_gear_w(void) { return 40 + 2 * GZ(GEAR_R) + 18; }

void settings_draw_gear(void)
{
    /* A hub, eight teeth and a hole, as Avoidant draws it, smaller. The
     * fonts have no gear. */
    static const int dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    static const int dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
    const int cx = gear_cx(), cy = gear_cy();
    for (int i = 0; i < 8; i++) gfx_fill_circle(cx + GZ(dx[i]), cy + GZ(dy[i]), GZ(3), COL_DIM);
    gfx_fill_circle(cx, cy, GZ(8), COL_DIM);
    gfx_fill_circle(cx, cy, GZ(3), COL_BG);
}

bool settings_gear_hit(int x, int y)
{
    return x >= gfx_w() - GZ(GEAR_HIT_W) && y >= gfx_h() - GZ(GEAR_HIT_H);
}

/* ---- input ------------------------------------------------------------ */

/* What one poll of the keyboard and the panel came to. */
typedef enum { IN_NONE, IN_KEY, IN_TAP, IN_DRAG } in_kind_t;

typedef struct {
    in_kind_t kind;
    uint8_t   usage, mods;      /* IN_KEY */
    bool      cmd;              /* the shortcut modifier, as kbd.h has it (0027) */
    bool      apple;
    int       x, y;             /* IN_TAP: where it went down */
    int       dy;               /* IN_DRAG: moved since the last poll */
} input_t;

static struct {
    bool down, dragging;
    int  x0, y0, last_y;
} s_t;

static void input_reset(void)
{
    memset(&s_t, 0, sizeof(s_t));
    touch_swallow();
}

static input_t input_poll(void)
{
    input_t in = { .kind = IN_NONE };
    kbd_event_t ev;
    if (kbd_get(&ev, pdMS_TO_TICKS(POLL_MS))) {
        if (ev.down) { in.kind = IN_KEY; in.usage = ev.usage; in.mods = ev.mods; in.cmd = ev.cmd; in.apple = ev.apple; }
        return in;
    }
    int x, y;
    const bool now = touch_get(&x, &y);
    if (now && !s_t.down) {
        s_t.down = true; s_t.dragging = false;
        s_t.x0 = x; s_t.y0 = y; s_t.last_y = y;
    } else if (now && s_t.down) {
        if (!s_t.dragging && (y - s_t.y0 > DRAG_PX || s_t.y0 - y > DRAG_PX)) s_t.dragging = true;
        if (s_t.dragging && y != s_t.last_y) {
            in.kind = IN_DRAG; in.dy = y - s_t.last_y;
            s_t.last_y = y;
        }
    } else if (!now && s_t.down) {
        s_t.down = false;
        if (!s_t.dragging) { in.kind = IN_TAP; in.x = s_t.x0; in.y = s_t.y0; }
    }
    return in;
}

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* ---- drawing helpers -------------------------------------------------- */

static void button(int x, int y, int w, int h, const char *label, uint16_t bg)
{
    gfx_fill_rect(x, y, w, h, bg);
    const int tw = gfx_text_w(label, SCALE);
    gfx_draw_text(x + (w - tw) / 2, y + (h - TEXT_H) / 2, label, SCALE, w, COL_TEXT);
}

#define BACK_W  (200)
static int back_x(void) { return gfx_w() - PAD - BACK_W; }
#define BACK_Y  ((HEAD_H - BTN_H) / 2)

static void header(const char *title)
{
    gfx_fill_rect(0, 0, gfx_w(), HEAD_H, COL_HEAD);
    gfx_draw_text(PAD, (HEAD_H - GFX_GLYPH_H(TITLE_SCALE)) / 2, title, TITLE_SCALE,
                  gfx_w() - 2 * PAD - BACK_W, COL_TEXT);
    button(back_x(), BACK_Y, BACK_W, BTN_H, "Back", COL_BTN);
}

static bool back_hit(int x, int y)
{
    return y < HEAD_H && x >= back_x() - PAD / 2;
}

/* ---- the email form, full window -------------------------------------- */

/* The form until Save or Cancel. True if an account was saved. */
static bool email_form(void)
{
    const int W = gfx_w();
    mailform_t f = {
        .x = PAD * 3, .y = HEAD_H + 32, .w = W - PAD * 6,
        .row_h = 56, .scale = SCALE, .osk = false,
        .bg = COL_BG, .field = COL_FIELD, .key = COL_BTN, .text = COL_TEXT,
        .dim = COL_DIM, .edge = COL_EDGE, .go = COL_GO, .bad = COL_BAD, .on = COL_GO,
    };
    mailcfg_t c;
    mailcfg_load(NULL, &c);     /* complete or half-finished, either way */
    mailform_begin(&f, &c);
    mailcfg_wipe(&c);
    input_reset();

    bool saved = false, redraw = true;
    for (;;) {
        if (redraw) {
            gfx_fill_rect(0, 0, W, gfx_h(), COL_BG);
            gfx_fill_rect(0, 0, W, HEAD_H, COL_HEAD);
            gfx_draw_text(PAD, (HEAD_H - GFX_GLYPH_H(TITLE_SCALE)) / 2, "Email account",
                          TITLE_SCALE, W - 2 * PAD, COL_TEXT);
            mailform_draw(&f);
            gfx_blit(0, gfx_h());
            redraw = false;
        }
        const input_t in = input_poll();
        mailform_result_t r = MAILFORM_NONE;
        if (in.kind == IN_TAP) {
            r = mailform_tap(&f, in.x, in.y);
        } else if (in.kind == IN_KEY) {
            const bool shift = (in.mods & KBD_MOD_SHIFT) != 0;
            switch (in.usage) {
            case KBD_BACKSPACE: r = mailform_key(&f, MAILFORM_KEY_DEL); break;
            case KBD_TAB:       r = mailform_key(&f, shift ? MAILFORM_KEY_PREV : MAILFORM_KEY_NEXT); break;
            case KBD_DOWN:      r = mailform_key(&f, MAILFORM_KEY_NEXT); break;
            case KBD_UP:        r = mailform_key(&f, MAILFORM_KEY_PREV); break;
            case KBD_ENTER:
            case KBD_KP_ENTER:  r = mailform_key(&f, MAILFORM_KEY_ENTER); break;
            case KBD_ESCAPE:    r = mailform_key(&f, MAILFORM_KEY_ESC); break;
            default: {
                if (in.mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_GUI)) break;
                const char ch = kbd_ascii(in.usage, in.mods, false);
                if (ch >= 0x20 && ch < 0x7F) r = mailform_key(&f, ch);
                break;
            }
            }
        }
        if (r == MAILFORM_REDRAW) redraw = true;
        if (r == MAILFORM_CANCEL) break;
        if (r == MAILFORM_SAVE) {
            if (mailcfg_save(NULL, &f.c) == ESP_OK) { saved = true; break; }
            ESP_LOGW(TAG, "mailcfg_save failed");
            f.note = "Could not store the account in NVS.";
            redraw = true;
        }
    }
    mailform_done(&f);
    input_reset();
    return saved;
}

/* ---- the page --------------------------------------------------------- */

/* The four ways up, in the order the buttons are laid out: two rows of
 * two, so they fit the narrow way too. */
static const struct { int rot; const char *label; } k_rot[4] = {
    { GFX_ROT_90,  "Landscape" },
    { GFX_ROT_270, "Landscape, flipped" },
    { GFX_ROT_0,   "Portrait" },
    { GFX_ROT_180, "Portrait, flipped" },
};

/* The four themes (0021), laid out as the angles are. */
static const struct { int theme; const char *label; } k_theme[4] = {
    { DISPLAY_THEME_DARK,   "Dark" },
    { DISPLAY_THEME_LIGHT,  "Light" },
    { DISPLAY_THEME_GREEN,  "Green console" },
    { DISPLAY_THEME_ORANGE, "Orange console" },
};

/* The text sizes (0022): one narrow button each, in a row. */
#define SIZES   (DISPLAY_SCALE_MAX - DISPLAY_SCALE_MIN + 1)
static int size_w(void)
{
    const int w = (gfx_w() - 2 * PAD - (SIZES - 1) * (PAD / 2)) / SIZES;
    return w > 160 ? 160 : w;
}

/* Positions in content coordinates, from layout(). */
typedef struct {
    int edit_y, forget_y;       /* buttons; forget_y < 0 when not shown */
    int test_y;                 /* the test button; < 0 when not shown */
    int rot_y;                  /* the first row of the four angles */
    int theme_y;                /* the first row of the four themes */
    int size_y;                 /* the row of text sizes */
    int libs_y;                 /* first library row */
    int total_h;
} page_t;

static struct {
    bool       mail_ok;
    mailcfg_t  mail;            /* password wiped after load */
    const char *why;
    bool       forget_armed;
} s_p;

static void load_mail(void)
{
    s_p.mail_ok = mailcfg_load(NULL, &s_p.mail);
    memset(s_p.mail.pass, 0, sizeof(s_p.mail.pass));    /* never needed here */
    s_p.why = NULL;
    if (!s_p.mail_ok) {
        mailcfg_t probe = s_p.mail;
        probe.pass[0] = 'x';    /* judge the rest; the form says if it is missing */
        mailcfg_check(&probe, &s_p.why);
    }
}

/* Narrow -- the screen on end (0019): a library is two lines, its name
 * and then its checkout and licence, rather than three columns. */
static bool narrow(void) { return gfx_w() < 1000; }
static int lib_lh(void) { return (GFX_GLYPH_H(SCALE) + 10) * (narrow() ? 2 : 1); }

/* `s` in up to `max_lines` lines of `max_w`, broken between words by
 * gfx_wrap_line(); drawn from `y` when `draw`. The lines it took. */
static int wrapped(int x, int y, const char *s, int max_w, int max_lines, uint16_t c, bool draw)
{
    int lines = 0;
    while (*s && lines < max_lines) {
        const char *next = s;
        size_t n = gfx_wrap_line(s, SCALE, max_w, &next);
        char line[200];
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, s, n);
        line[n] = '\0';
        /* The last line allowed takes the rest, cut with an ellipsis. */
        if (lines == max_lines - 1 && *next) snprintf(line, sizeof(line), "%s", s);
        if (draw) gfx_draw_text(x, y + lines * LINE_H, line, SCALE, max_w, c);
        lines++;
        if (next == s) break;
        s = next;
    }
    return lines;
}

/*
 * Lay the page out from content y = 0 and, when `draw`, draw it shifted
 * up by `scroll` with the header's height above it.
 */
static page_t layout(bool draw, int scroll)
{
    page_t p = { .forget_y = -1, .test_y = -1 };
    const int W = gfx_w();
    const int off = HEAD_H - scroll;
    int y = 32;

#define TXT(yy, s, c) do { if (draw) gfx_draw_text(PAD, off + (yy), (s), SCALE, W - 2 * PAD, (c)); } while (0)
#define RULE(yy)      do { if (draw) gfx_fill_rect(PAD, off + (yy), W - 2 * PAD, 2, COL_RULE); } while (0)

    /* Email (SMTP) */
    TXT(y, "Email (SMTP)", COL_TEXT);
    y += LINE_H + 8;
    char line[160], dom[MAILCFG_ADDR_MAX + 2];
    if (s_p.mail_ok) {
        snprintf(line, sizeof(line), "%s, port %u, %s", s_p.mail.host,
                 (unsigned)mailcfg_port(&s_p.mail),
                 s_p.mail.security == MAILCFG_STARTTLS ? "STARTTLS" : "TLS");
        TXT(y, line, COL_DIM);
        y += LINE_H;
        mailcfg_domain(s_p.mail.to, dom, sizeof(dom));
        snprintf(line, sizeof(line), "Sends to %s", dom);     /* domain only, spec 6 */
        TXT(y, line, COL_DIM);
        y += LINE_H;
    } else {
        TXT(y, "Not set up: nothing will be sent.", COL_DIM);
        y += LINE_H;
        if (s_p.why && (s_p.mail.host[0] || s_p.mail.user[0] || s_p.mail.to[0])) {
            TXT(y, s_p.why, COL_DIM);
            y += LINE_H;
        }
    }
    y += 12;
    p.edit_y = y;
    const int bw = (W - 2 * PAD - PAD / 2) / 2;
    const int sw = size_w();
    if (draw) button(PAD, off + y, bw, BTN_H, s_p.mail_ok ? "Edit account" : "Set up", COL_BTN);
    if (s_p.mail_ok) {
        p.forget_y = y;
        if (draw) button(PAD + bw + PAD / 2, off + y, bw, BTN_H,
                         s_p.forget_armed ? "Tap again to forget" : "Forget",
                         s_p.forget_armed ? COL_BAD : COL_BTN);
    }
    y += BTN_H + 16;
    {
        /* The test: what it is doing, or how the last one went. Shown
         * whatever the account's state -- "no email configured" is a
         * result too (spec 6's messages). */
        char st[160];
        const bool busy = sender_status(st, sizeof st);
        p.test_y = y;
        if (draw) button(PAD, off + y, bw, BTN_H, busy ? "Sending..." : "Send test email",
                         busy ? COL_GO : COL_BTN);
        y += BTN_H + 16;
        /* Under the button, the page's width and two lines of it: a
         * failure sentence is long, and the page may be on end (0019). */
        if (st[0]) y += wrapped(PAD, off + y, st, W - 2 * PAD, 2, COL_DIM, draw) * LINE_H + 8;
    }
    TXT(y, "Kept on this device, never on the card.", COL_DIM);
    y += LINE_H + SECTION_GAP;

    /* Display (0019) */
    RULE(y - SECTION_GAP / 2);
    TXT(y, "Display", COL_TEXT);
    y += LINE_H + 8;
    p.rot_y = y;
    for (int i = 0; i < 4; i++) {
        const int bx = PAD + (i % 2) * (bw + PAD / 2);
        const int by = y + (i / 2) * (BTN_H + 16);
        if (draw) button(bx, off + by, bw, BTN_H, k_rot[i].label,
                         k_rot[i].rot == display_rotation() ? COL_GO : COL_BTN);
    }
    y += 2 * (BTN_H + 16);
    TXT(y, "Turns at once, and is kept on this device.", COL_DIM);
    y += LINE_H + 16;
    p.theme_y = y;
    for (int i = 0; i < 4; i++) {
        const int bx = PAD + (i % 2) * (bw + PAD / 2);
        const int by = y + (i / 2) * (BTN_H + 16);
        if (draw) button(bx, off + by, bw, BTN_H, k_theme[i].label,
                         k_theme[i].theme == display_theme() ? COL_GO : COL_BTN);
    }
    y += 2 * (BTN_H + 16);
    TXT(y, "Colours, kept on this device.", COL_DIM);
    y += LINE_H + 16;
    /* 0022: the editor's text size, one button a size. This page stays
     * at its own. */
    p.size_y = y;
    for (int i = 0; i < SIZES; i++) {
        char l[2] = { (char)('0' + DISPLAY_SCALE_MIN + i), '\0' };
        if (draw) button(PAD + i * (sw + PAD / 2), off + y, sw, BTN_H, l,
                         DISPLAY_SCALE_MIN + i == display_text_scale() ? COL_GO : COL_BTN);
    }
    y += BTN_H + 16;
    TXT(y, kbd_apple() ? "Text size in the editor; Cmd+= and Cmd+- there."
                       : "Text size in the editor; Ctrl+= and Ctrl+- there.", COL_DIM);
    y += LINE_H + SECTION_GAP;

    /* Libraries */
    RULE(y - SECTION_GAP / 2);
    TXT(y, "Libraries", COL_TEXT);
    y += LINE_H + 8;
    p.libs_y = y;
    const int lh = lib_lh();
    const bool nar = narrow();
    /* Wide: three columns. Narrow: the name, then under it the checkout
     * and the licence. */
    const int cx1 = PAD;
    const int cx2 = nar ? PAD + 24 : W * 45 / 100;
    const int cx3 = nar ? W * 65 / 100 : W * 75 / 100;
    const int row2 = nar ? GFX_GLYPH_H(SCALE) + 10 : 0;
    for (int i = 0; i < LIBVERSIONS_COUNT; i++, y += lh) {
        const int sy = off + y;
        if (!draw || sy + lh < HEAD_H || sy > gfx_h()) continue;
        gfx_draw_text(cx1, sy, k_libversions[i].module, SCALE, nar ? W - 2 * PAD : cx2 - cx1 - 16, COL_TEXT);
        gfx_draw_text(cx2, sy + row2, k_libversions[i].checkout, SCALE, cx3 - cx2 - 16, COL_DIM);
        gfx_draw_text(cx3, sy + row2, k_libversions[i].licence, SCALE, W - PAD - cx3, COL_DIM);
    }
    y += 32;
    p.total_h = y;
#undef TXT
#undef RULE
    return p;
}

static int max_scroll(const page_t *p)
{
    const int view = gfx_h() - HEAD_H;
    return p->total_h > view ? p->total_h - view : 0;
}

static void draw_page(int scroll, const page_t *p)
{
    const int W = gfx_w(), H = gfx_h();
    gfx_fill_rect(0, 0, W, H, COL_BG);
    layout(true, scroll);
    /* The header last, over anything scrolled up under it. */
    header("Settings");
    /* A thin bar at the right edge: where in the page this is. */
    const int ms = max_scroll(p);
    if (ms > 0) {
        const int view = H - HEAD_H;
        const int bh = view * view / p->total_h;
        const int by = HEAD_H + (view - bh) * scroll / ms;
        gfx_fill_rect(W - 8, by, 4, bh, COL_RULE);
    }
    gfx_blit(0, H);
}

void settings_page(void)
{
    int scroll = 0;
    load_mail();
    s_p.forget_armed = false;
    input_reset();
    page_t p = layout(false, 0);
    bool redraw = true;
    uint32_t seen_gen = sender_gen();

    for (;;) {
        if (redraw) {
            p = layout(false, scroll);
            if (scroll > max_scroll(&p)) scroll = max_scroll(&p);
            draw_page(scroll, &p);
            redraw = false;
        }
        const input_t in = input_poll();
        const int view = gfx_h() - HEAD_H;
        int want = scroll;
        /* The send task moved on: show it. */
        if (sender_gen() != seen_gen) { seen_gen = sender_gen(); redraw = true; }

        if (in.kind == IN_KEY) {
            const bool gui = in.cmd;   /* Command on an Apple keyboard, else Ctrl (0027) */
            switch (in.usage) {
            case KBD_ESCAPE:   goto out;
            case KBD_COMMA:    if (gui) goto out; break;
            /* The top and bottom: Command+Up / Down on a Mac; on a PC,
             * Ctrl+Home / End, as Home / End already are (0028). */
            case KBD_UP:       want = gui && in.apple ? 0 : scroll - KEY_STEP; break;
            case KBD_DOWN:     want = gui && in.apple ? max_scroll(&p) : scroll + KEY_STEP; break;
            case KBD_PAGEUP:   want = scroll - (view - LINE_H); break;
            case KBD_PAGEDOWN: want = scroll + (view - LINE_H); break;
            case KBD_HOME:     want = 0; break;
            case KBD_END:      want = max_scroll(&p); break;
            case KBD_ENTER:
            case KBD_KP_ENTER:
                if (email_form()) ESP_LOGI(TAG, "email account saved");
                load_mail();
                redraw = true;
                break;
            case KBD_T:         /* Ctrl+T: send a test (spec 5) */
                if (gui) sender_test();
                break;
            case KBD_R:         /* Ctrl+R: a quarter turn clockwise */
                if (gui) {
                    display_set_rotation((display_rotation() + 1) % 4);
                    want = 0;
                    redraw = true;
                }
                break;
            default: break;
            }
        } else if (in.kind == IN_DRAG) {
            want = scroll - in.dy;
        } else if (in.kind == IN_TAP) {
            if (back_hit(in.x, in.y)) goto out;
            const bool was_armed = s_p.forget_armed;
            s_p.forget_armed = false;
            if (in.y >= HEAD_H) {
                const int cy = in.y - HEAD_H + scroll;  /* content y */
                const int bw = (gfx_w() - 2 * PAD - PAD / 2) / 2;
                if (p.test_y >= 0 && in_rect(in.x, cy, PAD, p.test_y, bw, BTN_H)) {
                    sender_test();      /* refused while one runs; the label says so */
                } else if (in_rect(in.x, cy, PAD, p.edit_y, bw, BTN_H)) {
                    if (email_form()) ESP_LOGI(TAG, "email account saved");
                    load_mail();
                } else if (cy >= p.rot_y && cy < p.rot_y + 2 * (BTN_H + 16)) {
                    const int col = in.x >= PAD + bw + PAD / 2 ? 1 : 0;
                    const int rowi = (cy - p.rot_y) / (BTN_H + 16);
                    const int i = rowi * 2 + col;
                    const int bx = PAD + col * (bw + PAD / 2);
                    const int by = p.rot_y + rowi * (BTN_H + 16);
                    if (in_rect(in.x, cy, bx, by, bw, BTN_H) && k_rot[i].rot != display_rotation()) {
                        display_set_rotation(k_rot[i].rot);
                        want = 0;       /* the page is another shape now */
                    }
                } else if (cy >= p.size_y && cy < p.size_y + BTN_H) {
                    for (int i = 0; i < SIZES; i++) {
                        if (in_rect(in.x, cy, PAD + i * (size_w() + PAD / 2), p.size_y, size_w(), BTN_H))
                            display_set_text_scale(DISPLAY_SCALE_MIN + i);
                    }
                } else if (cy >= p.theme_y && cy < p.theme_y + 2 * (BTN_H + 16)) {
                    const int col = in.x >= PAD + bw + PAD / 2 ? 1 : 0;
                    const int rowi = (cy - p.theme_y) / (BTN_H + 16);
                    const int i = rowi * 2 + col;
                    const int bx = PAD + col * (bw + PAD / 2);
                    const int by = p.theme_y + rowi * (BTN_H + 16);
                    if (in_rect(in.x, cy, bx, by, bw, BTN_H)) display_set_theme(k_theme[i].theme);
                } else if (p.forget_y >= 0 &&
                           in_rect(in.x, cy, PAD + bw + PAD / 2, p.forget_y, bw, BTN_H)) {
                    if (was_armed) {
                        mailcfg_clear(NULL);
                        ESP_LOGI(TAG, "email account forgotten");
                        load_mail();
                    } else {
                        s_p.forget_armed = true;
                    }
                }
            }
            redraw = true;
        }

        if (want < 0) want = 0;
        if (want > max_scroll(&p)) want = max_scroll(&p);
        if (want != scroll) { scroll = want; redraw = true; }
    }
out:
    mailcfg_wipe(&s_p.mail);
    input_reset();
}
