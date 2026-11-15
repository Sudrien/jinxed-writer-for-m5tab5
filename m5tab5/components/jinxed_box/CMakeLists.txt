# jinxed_box: the table Jinxed's .bin loads a boxed editor with,
# feckless_box_calls' with writes held to /sd/jinxed by feckless's write
# filter (u-jinxed-0003, 0004, 0012, UMODE.md). The writer's firmware does
# not require it; boxlog's .bin does.
#
# SPDX-License-Identifier: MIT

idf_component_register(
    SRCS "src/jbox_storage.c" "src/jbox_calls.c"
    INCLUDE_DIRS "include"
                 "../../main"      # u-jinxed-0013: the writer's headers (kbd.h), for types
    REQUIRES feckless_umode feckless_box_calls
    PRIV_REQUIRES heap
)
