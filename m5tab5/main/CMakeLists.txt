# SPDX-License-Identifier: MIT

idf_component_register(
    SRCS "main.c" "display.c" "kbd.c" "editor.c" "doclog.c"
    INCLUDE_DIRS "."
    REQUIRES feckless_drivers feckless_graphics_handler
             feckless_storage_handler usb esp_hw_support
)
