#!/bin/sh
# Build and run the doclog host test under ASan/UBSan. Needs a C compiler.
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
cd "$t" && ./dlt 2>/dev/null
rm -rf "$t"
