# SPDX-License-Identifier: MIT
#
# The editor's ELF (u-jinxed-0018): ../main built as the boxed editor, with
# the .bin's own sdkconfig, so that both sides of the box are configured
# alike -- FatFs above all, which the storage section checks. Built by
# ../main/CMakeLists.txt; on its own:
#
#     cmake -S elf -B build/elf -DIDF_TARGET=esp32p4 \
#           -DJINXED_BOXED_HOST_SDKCONFIG=$PWD/sdkconfig && make -C build/elf elf
#
# As boxlog's elf/CMakeLists.txt, with the chooser linked in and Jinxed's
# own calls named.
cmake_minimum_required(VERSION 3.16)

get_filename_component(_m5 "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_partitions_line "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${_m5}/partitions.csv\"")

if(JINXED_BOXED_HOST_SDKCONFIG AND SDKCONFIG)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${JINXED_BOXED_HOST_SDKCONFIG}")
    file(READ "${JINXED_BOXED_HOST_SDKCONFIG}" _cfg)
    string(REGEX REPLACE "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"[^\"\n]*\""
           "${_partitions_line}" _cfg "${_cfg}")
    file(WRITE "${SDKCONFIG}.host" "${_cfg}")
    configure_file("${SDKCONFIG}.host" "${SDKCONFIG}" COPYONLY)
endif()

file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.elf.defaults" "${_partitions_line}\n")
set(SDKCONFIG_DEFAULTS "${_m5}/sdkconfig.defaults"
                       "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.defaults"
                       "${CMAKE_BINARY_DIR}/sdkconfig.elf.defaults")

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../main"
                         "${CMAKE_CURRENT_LIST_DIR}/jinxed_boxed_elfdeps")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(JINXED_BOXED_ELF 1)
# A lock of its own, elf/dependencies.lock, as boxlog's (u-jinxed-0007):
# the two builds' manifests differ. Both resolve feckless-libs' umode
# branch; if it moved between the two configures, the commits differ.

project(jinxed_editor)

# feckless_chooser is plain code that links into an app (its README); the
# rest of main's requirements are headers, their calls the box's.
# Jinxed's own calls (u-jinxed-0013, 0016), which the .bin's table answers
# beside feckless's and both checks then take as answered.
set(ELF_COMPONENTS feckless_chooser)
feckless_abi_box_elf(jinxed_editor OWN_CALLS "${_m5}/components/jinxed_box/jinxed_calls.txt")
