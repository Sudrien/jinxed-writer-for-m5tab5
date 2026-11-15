# SPDX-License-Identifier: MIT
#
# boxlog (u-jinxed-0005): the log replayed natively and in a feckless box,
# and the two compared. A project of its own, with its own manifest, lock
# and sdkconfig, so that nothing here touches the writer's firmware: a box
# needs the bootloader's region protection off, which the writer keeps.
# See README.md.
#
#     idf.py set-target esp32p4
#     idf.py build flash monitor
#
# The .bin is main/host.c; elf/ builds the ELF it embeds from the same
# main/, as Lothesome's umode branch does.
cmake_minimum_required(VERSION 3.16)

get_filename_component(_m5 "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# The writer's configuration, then the box's lines; its partition table
# by full path (the defaults name it relative to a project, not this one).
file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.boxlog.defaults"
     "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${_m5}/partitions.csv\"\n")
set(SDKCONFIG_DEFAULTS "${_m5}/sdkconfig.defaults"
                       "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
                       "${CMAKE_BINARY_DIR}/sdkconfig.boxlog.defaults")

# jinxed_box, the guard (u-jinxed-0004).
list(APPEND EXTRA_COMPONENT_DIRS "${_m5}/components")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(jinxed_boxlog)
