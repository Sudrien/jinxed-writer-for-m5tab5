/*
 * display.c -- the panel, dim, whichever way up.
 *
 * SPDX-License-Identifier: MIT
 */
#include "display.h"

#include "esp_check.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "lcd.h"
#include "nvs_handler.h"
#include "touch.h"

static const char *TAG = "display";

/* Jinxed's own settings: its namespace, apart from the email account's
 * and the saved networks'. */
#define NVS_NS      "jinxed"
#define NVS_ROT     "rotation"
#define NVS_THEME   "theme"
#define NVS_SCALE   "scale"

static int s_rot = DISPLAY_ROTATION_DEFAULT;

/* In DISPLAY_THEME_* order. Dark is the colours this had before 0021.
 * The consoles are phosphor on near-black: P1 green and P3 amber. */
static const display_theme_t k_themes[DISPLAY_THEMES] = {
    [DISPLAY_THEME_DARK] = {
        .bg = RGB(0x10, 0x10, 0x12), .text = RGB(0x9A, 0x9A, 0x90),
        .dim = RGB(0x50, 0x50, 0x4C), .cursor = RGB(0x70, 0x70, 0x68),
        .head = RGB(0x1C, 0x1C, 0x20), .rule = RGB(0x2C, 0x2C, 0x30),
        .btn = RGB(0x30, 0x30, 0x36), .field = RGB(0x1A, 0x1A, 0x1E),
        .edge = RGB(0x40, 0x40, 0x46), .go = RGB(0x2E, 0x50, 0x3A),
        .bad = RGB(0xA0, 0x58, 0x50),
    },
    [DISPLAY_THEME_LIGHT] = {
        .bg = RGB(0xEE, 0xEC, 0xE4), .text = RGB(0x2C, 0x2C, 0x28),
        .dim = RGB(0x7C, 0x7A, 0x72), .cursor = RGB(0x4C, 0x4C, 0x46),
        .head = RGB(0xDE, 0xDC, 0xD4), .rule = RGB(0xC8, 0xC6, 0xBE),
        .btn = RGB(0xD2, 0xD0, 0xC8), .field = RGB(0xF8, 0xF6, 0xF0),
        .edge = RGB(0xA4, 0xA2, 0x9A), .go = RGB(0xA4, 0xCC, 0xAE),
        .bad = RGB(0xB0, 0x40, 0x34),
    },
    [DISPLAY_THEME_GREEN] = {
        .bg = RGB(0x04, 0x0A, 0x04), .text = RGB(0x40, 0xE0, 0x60),
        .dim = RGB(0x20, 0x78, 0x30), .cursor = RGB(0x30, 0xB0, 0x48),
        .head = RGB(0x08, 0x14, 0x0A), .rule = RGB(0x12, 0x32, 0x18),
        .btn = RGB(0x10, 0x2C, 0x14), .field = RGB(0x06, 0x10, 0x08),
        .edge = RGB(0x1C, 0x50, 0x26), .go = RGB(0x1E, 0x62, 0x2C),
        .bad = RGB(0xC8, 0x64, 0x30),
    },
    [DISPLAY_THEME_ORANGE] = {
        .bg = RGB(0x0C, 0x07, 0x02), .text = RGB(0xFF, 0xA8, 0x28),
        .dim = RGB(0x84, 0x56, 0x14), .cursor = RGB(0xC0, 0x7C, 0x1C),
        .head = RGB(0x16, 0x0E, 0x04), .rule = RGB(0x34, 0x20, 0x08),
        .btn = RGB(0x30, 0x1E, 0x08), .field = RGB(0x12, 0x0B, 0x04),
        .edge = RGB(0x5C, 0x3A, 0x10), .go = RGB(0x66, 0x4A, 0x10),
        .bad = RGB(0xD8, 0x48, 0x30),
    },
};

static int s_theme = DISPLAY_THEME_DARK;
const display_theme_t *display_col = &k_themes[DISPLAY_THEME_DARK];

int display_theme(void) { return s_theme; }

static int s_scale = DISPLAY_SCALE_DEFAULT;

int display_text_scale(void) { return s_scale; }

/* A step under the text, as large as the status line still fits across
 * the screen this way up: "no keyboard", "caps" at 15 glyphs and "not
 * saved" at 21 (editor.c), room for "99999 words" after a gap, the
 * margins, and the gear, whose width grows with it (settings.c). */
int display_foot_scale(void)
{
    int f = s_scale - 1;
    while (f > DISPLAY_SCALE_MIN &&
           2 * 40 + (30 + 2 + 11) * GFX_GLYPH_W(f) + 58 + 11 * f > gfx_w()) f--;
    return f < DISPLAY_SCALE_MIN ? DISPLAY_SCALE_MIN : f;
}

void display_set_text_scale(int scale)
{
    if (scale < DISPLAY_SCALE_MIN) scale = DISPLAY_SCALE_MIN;
    if (scale > DISPLAY_SCALE_MAX) scale = DISPLAY_SCALE_MAX;
    if (scale == s_scale) return;
    s_scale = scale;
    if (nvsh_set_u8(NVS_NS, NVS_SCALE, (uint8_t)scale) != ESP_OK) {
        ESP_LOGW(TAG, "text size not kept; it holds until power-off");
    }
}

void display_set_theme(int t)
{
    if (t < 0 || t >= DISPLAY_THEMES) return;
    s_theme = t;
    display_col = &k_themes[t];
    if (nvsh_set_u8(NVS_NS, NVS_THEME, (uint8_t)t) != ESP_OK) {
        ESP_LOGW(TAG, "theme not kept; it holds until power-off");
    }
}

int display_rotation(void) { return s_rot; }

void display_set_rotation(int q)
{
    if (q < GFX_ROT_0 || q > GFX_ROT_270) return;
    s_rot = q;
    gfx_set_rotation(q);
    touch_set_rotation(q);
    touch_swallow();
    if (nvsh_set_u8(NVS_NS, NVS_ROT, (uint8_t)q) != ESP_OK) {
        ESP_LOGW(TAG, "rotation not kept; it holds until power-off");
    }
}

esp_err_t display_init(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(lcd_init(&panel), TAG, "lcd");
    ESP_RETURN_ON_ERROR(gfx_init(panel, LCD_H_RES, LCD_V_RES), TAG, "gfx");
    /* The kept angle, from the first frame: NVS is up before this. */
    uint8_t q;
    if (nvsh_get_u8(NVS_NS, NVS_ROT, &q) == ESP_OK && q <= GFX_ROT_270) s_rot = q;
    gfx_set_rotation(s_rot);
    if (nvsh_get_u8(NVS_NS, NVS_THEME, &q) == ESP_OK && q < DISPLAY_THEMES) {
        s_theme = q;
        display_col = &k_themes[q];
    }
    if (nvsh_get_u8(NVS_NS, NVS_SCALE, &q) == ESP_OK &&
        q >= DISPLAY_SCALE_MIN && q <= DISPLAY_SCALE_MAX) s_scale = q;

    /* Paint before light: the backlight stays at zero until there is a
     * picture, so nothing flashes at power-on. */
    gfx_fill_rect(0, 0, gfx_w(), gfx_h(), COL_BG);
    gfx_blit(0, gfx_h());
    return lcd_backlight_set(DISPLAY_BACKLIGHT);
}
