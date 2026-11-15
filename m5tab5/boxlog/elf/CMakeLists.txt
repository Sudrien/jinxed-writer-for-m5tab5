# SPDX-License-Identifier: MIT
#
# boxlog's ELF (u-jinxed-0005): ../main built as the boxed probe, with the
# .bin's own sdkconfig, so that both sides of the box are configured
# alike -- FatFs above all, which the storage section checks. Built by
# ../main/CMakeLists.txt; on its own:
#
#     cmake -S elf -B build/elf -DIDF_TARGET=esp32p4 \
#           -DJINXED_BOXLOG_HOST_SDKCONFIG=$PWD/sdkconfig && make -C build/elf elf
#
# As Lothesome's elf/CMakeLists.txt, less its version and libraries lines.
cmake_minimum_required(VERSION 3.16)

get_filename_component(_m5 "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_partitions_line "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${_m5}/partitions.csv\"")

if(JINXED_BOXLOG_HOST_SDKCONFIG AND SDKCONFIG)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${JINXED_BOXLOG_HOST_SDKCONFIG}")
    file(READ "${JINXED_BOXLOG_HOST_SDKCONFIG}" _cfg)
    string(REGEX REPLACE "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"[^\"\n]*\""
           "${_partitions_line}" _cfg "${_cfg}")
    file(WRITE "${SDKCONFIG}.host" "${_cfg}")
    configure_file("${SDKCONFIG}.host" "${SDKCONFIG}" COPYONLY)
endif()

file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.elf.defaults" "${_partitions_line}\n")
set(SDKCONFIG_DEFAULTS "${_m5}/sdkconfig.defaults"
                       "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.defaults"
                       "${CMAKE_BINARY_DIR}/sdkconfig.elf.defaults")

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../main")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(JINXED_BOXLOG_ELF 1)
# The .bin's lock: the ELF's feckless is the .bin's commit, not a fresh
# resolve of the branch.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/../dependencies.lock")

project(jinxed_boxlog_probe)

# Fails the build on any import a box does not answer (boxcheck).
feckless_abi_box_elf(jinxed_boxlog_probe)
