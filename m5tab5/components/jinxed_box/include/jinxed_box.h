/*
 * jinxed_box.h -- what Jinxed's own .bin answers a boxed editor with
 * (u-jinxed-0003, UMODE.md).
 *
 * feckless_box_calls_v1() answers stdio for reading only. The log is
 * written on every keystroke, so this table adds the writes doclog.c and
 * spell.c make -- fwrite, fflush, fsync, fileno, ferror, stat, mkdir,
 * rename, unlink, the storage handler's brief hold and hidden mark -- and
 * esp_random, and answers every FILE call itself (fopen, fread, fgets,
 * fseek, ftell, fclose too), so that one table owns every FILE the app
 * holds. Everything else is feckless_box_calls_v1()'s, unchanged.
 *
 *   uint32_t n;
 *   const feckless_umode_call_t *calls = jinxed_box_calls(&n);
 *   feckless_umode_load(image, size, calls, n, box_size, stack, &app);
 *   ...
 *   feckless_umode_unload(app);
 *   jinxed_box_end();
 *   feckless_box_calls_end();
 *
 * A boxed editor may touch the card under JINXED_BOX_ROOT and nowhere
 * else: every path is copied out of the box, then refused (EACCES) unless
 * it is that directory or below it with no ".." segment. The email
 * account, Wi-Fi and the rest stay in NVS and on the host's side.
 *
 * Every name here is already in the feckless ABI (the storage section and
 * exports.txt), so the ELF's ABI check takes them; answers.txt beside this
 * header lists them for the box check.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "feckless_umode_calls.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JINXED_BOX_ROOT
#define JINXED_BOX_ROOT "/sd/jinxed"
#endif

/* Jinxed's calls, then feckless_box_calls_v1()'s without the names
 * Jinxed answers. Built once; the same array every call; NULL (and *n 0)
 * if it could not be allocated. */
const feckless_umode_call_t *jinxed_box_calls(uint32_t *n);

/* The app has ended: close the files it left open and give back the
 * brief holds it did not release. After feckless_umode_unload(), beside
 * feckless_box_calls_end(). */
void jinxed_box_end(void);

/* The open-file limit, and the longest path taken from the app. */
#define JINXED_BOX_FILES    8
#define JINXED_BOX_PATH_MAX 256

#ifdef __cplusplus
}
#endif
