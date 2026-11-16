#!/bin/sh
# Build and run the doclog, spell, hidrpt, md, fname, kbdq, drawq and redraw host tests under ASan/UBSan. Needs a
# C compiler. A word list as the first argument (symspell-c99's
# dictionaries/dictionary.txt) also builds and probes a full dictionary.
# SPDX-License-Identifier: MIT
set -e
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d)
mkdir -p "$t/inc"
cat > "$t/inc/esp_err.h" <<'H'
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
H
cc -g -O1 -Wall -Wextra -fsanitize=address,undefined -DDOCLOG_HOST -D'DOCLOG_DIR="sd"' \
   -I "$t/inc" -I "$here/../main" -o "$t/dlt" "$here/doclog_host_test.c" "$here/../main/doclog.c"
cc -g -O1 -Wall -Wextra -fsanitize=address,undefined -DSPELL_HOST -D'SPELL_DIR="sd/spell"' \
   -I "$here/../main" -o "$t/slt" "$here/spell_host_test.c" "$here/../main/spell.c"
cc -g -O1 -Wall -Wextra -fsanitize=address,undefined \
   -I "$here/../main" -o "$t/mdt" "$here/md_host_test.c" "$here/../main/md.c"
cc -g -O1 -Wall -Wextra -fsanitize=address,undefined \
   -I "$here/../main" -o "$t/hrt" "$here/hidrpt_host_test.c" "$here/../main/hidrpt.c"
cc -g -O1 -Wall -Wextra -fsanitize=address,undefined \
   -I "$here/../main" -o "$t/fnt" "$here/fname_host_test.c" "$here/../main/fname.c"
cc -g -O1 -Wall -Wextra -fsanitize=address,undefined \
   -I "$here/../main" -o "$t/kqt" "$here/kbdq_host_test.c"
cc -g -O1 -Wall -Wextra -fsanitize=address,undefined \
   -I "$here/../main" -o "$t/dqt" "$here/drawq_host_test.c"
# u-jinxed-0024: the redraw test compiles editor.c over feckless-libs' own
# gfx.c and its host shim, so it needs a checkout of feckless-libs (the
# umode branch): FECKLESS=<path>, or one beside this repository.
feckless=${FECKLESS:-$here/../../../feckless-libs-for-m5tab5}
g=$feckless/components/feckless_graphics_handler
if [ -f "$g/src/gfx.c" ]; then
    cc -std=gnu11 -g -O1 -D_GNU_SOURCE -DMALLOC_CAP_DEFAULT=0 -Wall -Wextra -Wno-sign-compare \
       -fsanitize=address,undefined -I "$here/redraw_fake" -I "$g/test" -I "$g/test/fake" \
       -I "$g/include" -I "$g/fonts/ark12" -I "$g/fonts/arabixel" \
       -I "$feckless/components/feckless_drivers/include" \
       -I "$feckless/components/feckless_storage_handler/include" -I "$here/../main" \
       -o "$t/rdt" "$here/redraw_host_test.c" "$g/src/gfx.c" "$g/fonts/ark12/ark12.c" \
       "$g/fonts/arabixel/arabixel_tab.c" "$g/fonts/arabixel/arabixel_shape.c" "$here/../main/md.c" -lm
    rdt=yes
else
    echo "redraw: skipped -- no feckless-libs at $feckless (FECKLESS=<path>)"
fi
list=${1:+$(cd "$(dirname "$1")" && pwd)/$(basename "$1")}
(cd "$t" && ./dlt 2>/dev/null)
(cd "$t" && ./hrt)
(cd "$t" && ./mdt)
(cd "$t" && ./fnt)
(cd "$t" && ./kqt && ./kqt old)
(cd "$t" && ./dqt)
[ -n "$rdt" ] && (cd "$t" && ./rdt)
(cd "$t" && rm -rf sd && ./slt $list 2>/dev/null)
rm -rf "$t"
