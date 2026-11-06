# SPDX-License-Identifier: MIT

idf_component_register(
    SRCS "main.c" "display.c" "kbd.c" "editor.c" "doclog.c" "settings.c" "sender.c" "spell.c"
    INCLUDE_DIRS "."
    REQUIRES feckless_drivers feckless_graphics_handler
             feckless_storage_handler feckless_mail
             feckless_nvs_handler feckless_libversions
             feckless_network_handler usb esp_hw_support esp_timer esp_app_format
)
