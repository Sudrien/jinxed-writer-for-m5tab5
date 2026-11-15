/*
 * jinxed_box.h -- what Jinxed's own .bin loads a boxed editor with
 * (u-jinxed-0003, 0004, 0012, UMODE.md).
 *
 * feckless_box_calls_v1()'s table, every name of it, with the editor's
 * writes held to JINXED_BOX_ROOT by feckless's write filter
 * (u-feckless-0055), which sees the host's copy of each path the app would
 * write. Reads go anywhere, as feckless has them.
 *
 *   uint32_t n;
 *   const feckless_umode_call_t *calls = jinxed_box_calls(&n);   // for each load
 *   feckless_umode_load(image, size, calls, n, box_size, stack, &app);
 *   ...
 *   feckless_umode_unload(app);
 *   feckless_box_calls_end();      // forgets the filter too
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "feckless_umode_calls.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JINXED_BOX_ROOT
#define JINXED_BOX_ROOT "/sd/jinxed"
#endif

/* Installs the filter and returns feckless's table. Call it before each
 * feckless_umode_load(), since feckless_box_calls_end() forgets the
 * filter with the app. */
const feckless_umode_call_t *jinxed_box_calls(uint32_t *n);

/* The filter: true if path is JINXED_BOX_ROOT or below it, with no ".."
 * segment. */
bool jinxed_box_may_write(const char *path);

#ifdef __cplusplus
}
#endif
