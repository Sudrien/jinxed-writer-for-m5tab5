# jinxed-writer-for-m5tab5: a writerdeck for the M5Stack Tab5.
#
# Standalone ESP-IDF 6.1 firmware. The feckless components are linked
# directly (main/idf_component.yml), not through the ELF launcher's ABI.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only what main/ requires, and what that requires, is built.
idf_build_set_property(MINIMAL_BUILD ON)

project(jinxed_writer)

# libversions.h: the settings page's Libraries section, written at
# configure time from the build's own lock and components, so the list
# cannot drift from what was compiled.
idf_component_get_property(main_lib main COMPONENT_LIB)
feckless_libversions(${main_lib} NAME "Jinxed Writer" LICENSE MIT)
