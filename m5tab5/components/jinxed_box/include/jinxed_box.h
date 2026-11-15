/*
 * jinxed_box.h -- the table Jinxed's own .bin loads a boxed editor with
 * (u-jinxed-0003, 0004, UMODE.md).
 *
 * feckless_box_calls_v1()'s table, every name of it, with the calls that
 * can change the card by a path held to JINXED_BOX_ROOT: remove, unlink,
 * mkdir, rmdir, utime, rename, storage_mark_hidden, and fopen and
 * storage_io_open in a mode that writes; f_open and f_chmod refused when
 * they would write. Everything else, reads and every FILE call included,
 * is feckless's handler, unchanged (src/jbox_storage.c says how).
 *
 *   uint32_t n;
 *   const feckless_umode_call_t *calls = jinxed_box_calls(&n);
 *   feckless_umode_load(image, size, calls, n, box_size, stack, &app);
 *   ...
 *   feckless_umode_unload(app);
 *   feckless_box_calls_end();
 *
 * A storage call that writes by a path and that feckless adds later is
 * not guarded until it is added here.
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

/* Built once; the same array every call. NULL (and *n 0) if it could not
 * be allocated, or if feckless's table lacks a call this guards. */
const feckless_umode_call_t *jinxed_box_calls(uint32_t *n);

/* The longest path the guard reads from the app. Longer is EFAULT. */
#define JINXED_BOX_PATH_MAX 256

#ifdef __cplusplus
}
#endif
