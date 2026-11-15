/* SPDX-License-Identifier: MIT */
#pragma once
#include_next "esp_heap_caps.h"
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
