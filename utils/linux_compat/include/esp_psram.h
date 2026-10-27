/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal stand-in for the esp_psram <esp_psram.h> header, used only for the
 * FreeRTOS Linux simulator build. The linux target has no PSRAM, so the mocks
 * and the allocator tests always take their no-PSRAM path at compile time and
 * never call these functions.
 */

#pragma once

#include <stdbool.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

bool esp_psram_is_initialized(void);

#ifdef __cplusplus
}
#endif
