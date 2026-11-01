# SPDX-License-Identifier: MIT

idf_component_register(
    SRCS "main.c" "display.c"
    INCLUDE_DIRS "."
    REQUIRES feckless_drivers feckless_graphics_handler
)
