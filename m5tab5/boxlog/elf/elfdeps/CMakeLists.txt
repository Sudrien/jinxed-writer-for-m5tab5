# SPDX-License-Identifier: MIT
#
# jinxed_boxlog_elfdeps (u-jinxed-0007): no code. Its manifest brings in
# what only boxlog's ELF links -- feckless_box_libc, an app's libc, and
# feckless_abi_storage, the storage section -- so that main's manifest,
# which both builds read, does not. The component manager makes every
# dependency in a component's manifest a requirement of that component;
# here that component is only in the ELF's build.
idf_component_register(
    REQUIRES feckless_box_libc feckless_abi_storage
)
