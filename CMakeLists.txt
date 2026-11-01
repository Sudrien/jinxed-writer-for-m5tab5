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
