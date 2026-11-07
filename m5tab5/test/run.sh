#!/bin/sh
# Build and run the doclog, spell, hidrpt, md and fname host tests under ASan/UBSan. Needs a
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
list=${1:+$(cd "$(dirname "$1")" && pwd)/$(basename "$1")}
(cd "$t" && ./dlt 2>/dev/null)
(cd "$t" && ./hrt)
(cd "$t" && ./mdt)
(cd "$t" && ./fnt)
(cd "$t" && rm -rf sd && ./slt $list 2>/dev/null)
rm -rf "$t"
