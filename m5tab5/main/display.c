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

static int s_rot = DISPLAY_ROTATION_DEFAULT;

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

    /* Paint before light: the backlight stays at zero until there is a
     * picture, so nothing flashes at power-on. */
    gfx_fill_rect(0, 0, gfx_w(), gfx_h(), COL_BG);
    gfx_blit(0, gfx_h());
    return lcd_backlight_set(DISPLAY_BACKLIGHT);
}
