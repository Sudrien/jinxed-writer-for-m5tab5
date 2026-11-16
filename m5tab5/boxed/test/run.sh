#!/bin/sh
# Host tests for the boxed build's own code (u-jinxed-0027): main/shim.c.
# The headers are feckless-libs': FECKLESS=<path>, or a checkout beside
# this repository. The writer's own host tests are ../../test/run.sh.
# SPDX-License-Identifier: MIT
set -e
here=$(cd "$(dirname "$0")" && pwd)
feckless=${FECKLESS:-$here/../../../../feckless-libs-for-m5tab5}
c=$feckless/components
g=$c/feckless_graphics_handler
if [ ! -f "$g/include/gfx.h" ]; then
    echo "shim: skipped -- no feckless-libs at $feckless (FECKLESS=<path>)"
    exit 0
fi
t=$(mktemp -d)
cc -std=gnu11 -g -O1 -D_GNU_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I "$here/../../test/redraw_fake" -I "$g/test" -I "$g/test/fake" -I "$g/include" \
   -I "$g/fonts/ark12" -I "$g/fonts/arabixel" -I "$c/feckless_abi/include" \
   -I "$c/feckless_abi_nvs/include" -I "$c/feckless_abi_storage/include" \
   -I "$c/feckless_box_libc/include" -I "$c/feckless_nvs_handler/include" \
   -I "$here/../../main" -I "$here/../main" \
   -o "$t/sht" "$here/shim_host_test.c"
(cd "$t" && ./sht)
rm -rf "$t"
