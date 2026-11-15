# jinxed_box: what Jinxed's .bin answers a boxed editor with, on top of
# feckless_box_calls (u-jinxed-0003, UMODE.md). Nothing requires it yet,
# so under MINIMAL_BUILD it is not built.
#
# SPDX-License-Identifier: MIT

idf_component_register(
    SRCS "src/jbox_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES feckless_umode feckless_box_calls
    PRIV_REQUIRES feckless_storage_handler esp_hw_support heap
)
