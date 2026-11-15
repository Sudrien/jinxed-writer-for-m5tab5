# jinxed_box: the table Jinxed's .bin loads a boxed editor with,
# feckless_box_calls' with writes held to /sd/jinxed (u-jinxed-0003, 0004,
# UMODE.md). Nothing requires it yet, so under MINIMAL_BUILD it is not
# built.
#
# SPDX-License-Identifier: MIT

idf_component_register(
    SRCS "src/jbox_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES feckless_umode feckless_box_calls
    PRIV_REQUIRES fatfs heap
)
