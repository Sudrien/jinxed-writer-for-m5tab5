/* feckless_box_calls' stand-in, and the brief holds and the hidden mark
 * it has no need of. SPDX-License-Identifier: MIT */
#pragma once
#include_next "storage.h"
void storage_hold_brief(storage_id_t id);
void storage_release_brief(storage_id_t id);
void storage_mark_hidden(const char *path);
