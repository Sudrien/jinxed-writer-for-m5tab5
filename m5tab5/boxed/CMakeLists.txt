# SPDX-License-Identifier: MIT
#
# boxed (u-jinxed-0018): the writer with its editor in a feckless box. A
# project of its own, as boxlog is (u-jinxed-0005), with its own manifest,
# lock and sdkconfig, so that nothing here touches the writer's firmware:
# a box needs the bootloader's region protection off, which the writer
# keeps. See README.md.
#
#     idf.py set-target esp32p4
#     idf.py build flash monitor
#
# The .bin is main/host.c and the writer's main/ less editor.c and main.c;
# elf/ builds the ELF it embeds, the editor, from the same main/.
cmake_minimum_required(VERSION 3.16)

get_filename_component(_m5 "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# The writer's configuration, then the box's lines; its partition table
# by full path (the defaults name it relative to a project, not this one).
file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.boxed.defaults"
     "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${_m5}/partitions.csv\"\n")
set(SDKCONFIG_DEFAULTS "${_m5}/sdkconfig.defaults"
                       "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
                       "${CMAKE_BINARY_DIR}/sdkconfig.boxed.defaults")

# jinxed_box, the table and Jinxed's own calls; jinxed_boxed_bindeps, whose
# manifest names what only the .bin links (the network, mail, the box's
# host side), so that the ELF's build, which reads main's manifest too,
# does not resolve and compile them.
list(APPEND EXTRA_COMPONENT_DIRS "${_m5}/components"
                                 "${CMAKE_CURRENT_LIST_DIR}/jinxed_boxed_bindeps")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(jinxed_boxed)

# The settings page's Libraries, as the writer's (0002): this build's
# lock and components, which is the .bin's. The ELF's own (feckless_box_libc,
# the two ABI sections) are not in it.
idf_component_get_property(main_lib main COMPONENT_LIB)
feckless_libversions(${main_lib} NAME "Jinxed Writer" LICENSE MIT)
