# SPDX-License-Identifier: MIT
#
# boxlog's one component (u-jinxed-0005), built twice:
#
# - by elf/, as the boxed ELF: the probe with doclog.c and md.c from the
#   writer's own main/, unchanged, DOCLOG_DIR the box's. The feckless
#   components named are headers; their calls are imports, which
#   feckless_box_calls answers through jinxed_box's guard. feckless_box_libc
#   and feckless_abi_storage come from ../elf/elfdeps' manifest, which
#   only that build has (u-jinxed-0007).
# - by the project, as the .bin: host.c, the same probe natively with
#   DOCLOG_DIR the host's, and the ELF embedded.
set(_jm "${CMAKE_CURRENT_LIST_DIR}/../../main")
idf_build_get_property(_boxlog_elf JINXED_BOXLOG_ELF)

if(_boxlog_elf)
    idf_component_register(
        SRCS "boxlog.c" "${_jm}/doclog.c" "${_jm}/md.c"
        INCLUDE_DIRS "." "${_jm}"
        REQUIRES feckless_abi feckless_abi_storage feckless_storage_handler
                 esp_timer esp_hw_support heap
                 lwip                    # for elf_loader's esp_elf_symbol.c; as Defeatist's boxwin (u-defeatist-0003)
                 feckless_box_libc       # linked into the ELF
                 jinxed_boxlog_elfdeps   # u-jinxed-0007: where the two above come from
    )
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOXLOG_ELF=1
                               "DOCLOG_DIR=\"/sd/jinxed/boxlog/box\"")
    return()
endif()

idf_component_register(
    SRCS "host.c" "boxlog.c" "${_jm}/doclog.c" "${_jm}/md.c"
    INCLUDE_DIRS "." "${_jm}"
    PRIV_REQUIRES jinxed_box feckless_umode feckless_box_calls
                  feckless_drivers feckless_storage_handler
                  esp_timer esp_hw_support heap freertos
                  lwip      # u-jinxed-0006: for elf_loader, as in the ELF's list above
)

# u-jinxed-0006: lwip is here for a file that is not this project's.
# feckless_umode requires elf_loader, whose esp_elf_symbol.c includes
# <sys/socket.h> and <arpa/inet.h> without requiring lwip, where they live.
# The writer's build has lwip anyway (its main names it for Wi-Fi); this
# project is trimmed to main and what it requires, so the .bin's first
# build stopped at that file. 0005 gave the ELF's build lwip (after
# Defeatist's boxwin, u-defeatist-0003) and missed that the .bin links
# elf_loader too. Nothing here opens a socket.
target_compile_definitions(${COMPONENT_LIB} PRIVATE "DOCLOG_DIR=\"/sd/jinxed/boxlog/host\"")

# The ELF, built by elf/ into this build and embedded, as Lothesome's
# main/CMakeLists.txt does it.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(ExternalProject)
    idf_build_get_property(_idf_path IDF_PATH)
    idf_build_get_property(_target IDF_TARGET)
    idf_build_get_property(_build_dir BUILD_DIR)
    idf_build_get_property(_sdkconfig SDKCONFIG)
    idf_build_get_property(_project_dir PROJECT_DIR)
    set(_elf_dir "${_build_dir}/elf")
    set(_elf "${_elf_dir}/jinxed_boxlog_probe.app.elf")
    externalproject_add(jinxed_boxlog_elf
        SOURCE_DIR "${_project_dir}/elf"
        BINARY_DIR "${_elf_dir}"
        CMAKE_ARGS -DIDF_PATH=${_idf_path}
                   -DIDF_TARGET=${_target}
                   -DSDKCONFIG=${_elf_dir}/sdkconfig
                   -DJINXED_BOXLOG_HOST_SDKCONFIG=${_sdkconfig}
        CMAKE_GENERATOR "Unix Makefiles"
        BUILD_COMMAND make elf
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${_elf}"
        INSTALL_COMMAND ""
    )
    add_dependencies(${COMPONENT_LIB} jinxed_boxlog_elf)
    elf_embed_aligned_binary(${COMPONENT_LIB} "${_elf}" BINARY)
endif()
