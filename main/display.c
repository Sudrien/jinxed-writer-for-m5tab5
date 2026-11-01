/*
 * display.c -- the panel, landscape, dim.
 *
 * SPDX-License-Identifier: MIT
 */
#include "display.h"

#include "esp_check.h"
#include "esp_lcd_types.h"
#include "lcd.h"

static const char *TAG = "display";

esp_err_t display_init(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(lcd_init(&panel), TAG, "lcd");
    ESP_RETURN_ON_ERROR(gfx_init(panel, LCD_H_RES, LCD_V_RES), TAG, "gfx");
    gfx_set_rotation(DISPLAY_ROTATION);

    /* Paint before light: the backlight stays at zero until there is a
     * picture, so nothing flashes at power-on. */
    gfx_fill_rect(0, 0, gfx_w(), gfx_h(), COL_BG);
    gfx_blit(0, gfx_h());
    return lcd_backlight_set(DISPLAY_BACKLIGHT);
}
