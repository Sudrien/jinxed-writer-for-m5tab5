# SPDX-License-Identifier: MIT
#
# boxed's one component (u-jinxed-0018), built twice, as boxlog's:
#
# - by elf/, as the editor's ELF: the writer's editor.c, files.c, doclog.c,
#   md.c, spell.c and fname.c, unchanged, with shim.c for the four things
#   that cannot cross under their own names, and feckless_storage_handler's
#   storage_list.c, which a boxed app links into itself since it calls the
#   app's filter (u-feckless-0047). The feckless components named are
#   headers; their calls are imports, which feckless_box_calls answers, and
#   the host modules' calls are Jinxed's own (jinxed_box).
# - by the project, as the .bin: host.c in place of the writer's main.c,
#   and the rest of the writer's main/ less editor.c -- doclog.c for the
#   dark wake's send, spell.c for the sender's check of a download, files.c
#   for the settings page's mode -- and the ELF embedded.
set(_jm "${CMAKE_CURRENT_LIST_DIR}/../../main")
idf_build_get_property(_boxed_elf JINXED_BOXED_ELF)

if(_boxed_elf)
    set(_srcs "shim.c" "${_jm}/editor.c" "${_jm}/files.c" "${_jm}/doclog.c"
              "${_jm}/md.c" "${_jm}/spell.c" "${_jm}/fname.c")
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        idf_component_get_property(_sh feckless_storage_handler COMPONENT_DIR)
        list(APPEND _srcs "${_sh}/src/storage_list.c")
        # u-jinxed-0019: the chooser here too, not as an ELF_COMPONENTS
        # library linked after this one. Only the chooser calls
        # storage_list_*, so linked separately it came after main's archive
        # had been scanned, storage_list.o was never pulled in, and the five
        # names were left as imports (a shared object may have undefined
        # names), which boxcheck refused. In one archive ld rescans until
        # nothing more is pulled: files.o pulls the chooser, the chooser
        # storage_list.o.
        idf_component_get_property(_ch feckless_chooser COMPONENT_DIR)
        list(APPEND _srcs "${_ch}/src/feckless_chooser.c")
    endif()
    idf_component_register(
        SRCS ${_srcs}
        INCLUDE_DIRS "." "${_jm}"
        REQUIRES feckless_abi feckless_abi_storage feckless_abi_nvs
                 feckless_drivers feckless_graphics_handler feckless_storage_handler
                 feckless_nvs_handler feckless_chooser fatfs
                 esp_timer esp_hw_support esp_app_format heap
                 lwip                    # for elf_loader's esp_elf_symbol.c, as boxlog's (u-jinxed-0006)
                 feckless_box_libc       # linked into the ELF
                 jinxed_boxed_elfdeps    # where the three ELF-only ones come from
    )
    # u-jinxed-0028: no -fno-builtin-stpcpy. The stpcpy GCC makes of
    # editor.c's strcat() chain comes from feckless_box_libc
    # (u-feckless-0058).
    return()
endif()

idf_component_register(
    SRCS "host.c" "${_jm}/display.c" "${_jm}/kbd.c" "${_jm}/hidrpt.c" "${_jm}/settings.c"
         "${_jm}/sender.c" "${_jm}/power.c" "${_jm}/doclog.c" "${_jm}/spell.c"
         "${_jm}/md.c" "${_jm}/files.c" "${_jm}/fname.c"
    INCLUDE_DIRS "." "${_jm}"
    PRIV_REQUIRES jinxed_box feckless_umode feckless_box_calls
                  feckless_drivers feckless_graphics_handler
                  feckless_storage_handler feckless_mail
                  feckless_nvs_handler feckless_libversions
                  feckless_network_handler feckless_chooser
                  usb esp_hw_support esp_timer esp_app_format heap freertos lwip
                  jinxed_boxed_bindeps   # where the .bin-only ones come from
)

# The ELF, built by elf/ into this build and embedded, as boxlog's.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(ExternalProject)
    idf_build_get_property(_idf_path IDF_PATH)
    idf_build_get_property(_target IDF_TARGET)
    idf_build_get_property(_build_dir BUILD_DIR)
    idf_build_get_property(_sdkconfig SDKCONFIG)
    idf_build_get_property(_project_dir PROJECT_DIR)
    set(_elf_dir "${_build_dir}/elf")
    set(_elf "${_elf_dir}/jinxed_editor.app.elf")
    externalproject_add(jinxed_editor_elf
        SOURCE_DIR "${_project_dir}/elf"
        BINARY_DIR "${_elf_dir}"
        CMAKE_ARGS -DIDF_PATH=${_idf_path}
                   -DIDF_TARGET=${_target}
                   -DSDKCONFIG=${_elf_dir}/sdkconfig
                   -DJINXED_BOXED_HOST_SDKCONFIG=${_sdkconfig}
        CMAKE_GENERATOR "Unix Makefiles"
        BUILD_COMMAND make elf
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${_elf}"
        INSTALL_COMMAND ""
    )
    add_dependencies(${COMPONENT_LIB} jinxed_editor_elf)
    elf_embed_aligned_binary(${COMPONENT_LIB} "${_elf}" BINARY)
endif()
