# SPDX-License-Identifier: MIT
#
# jinxed_boxed_bindeps (u-jinxed-0018): no code. Its manifest brings in
# what only the .bin links -- the box's host side, mail, the network, the
# library table -- as elf/jinxed_boxed_elfdeps does for the ELF (boxlog's
# u-jinxed-0007, the other way round). The component manager makes each
# of these a requirement of this component; main's .bin build requires it,
# and names each of them by name as well.
idf_component_register()
