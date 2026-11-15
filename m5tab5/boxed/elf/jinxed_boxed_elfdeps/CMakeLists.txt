# SPDX-License-Identifier: MIT
#
# jinxed_boxed_elfdeps (u-jinxed-0018): no code, as boxlog's
# jinxed_boxlog_elfdeps (u-jinxed-0007). Its manifest brings in what only
# the editor's ELF links -- feckless_box_libc, an app's libc, and the
# storage and NVS sections -- so that main's manifest, which both builds
# read, does not. Named after its directory (u-jinxed-0008).
idf_component_register(
    REQUIRES feckless_box_libc feckless_abi_storage feckless_abi_nvs
)
