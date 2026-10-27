/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal stand-in for the esp_hw_support <esp_memory_utils.h> header, used
 * only for the FreeRTOS Linux simulator build. The linux target has no
 * external RAM, so the function is mocked at link time and always reports
 * false.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool esp_ptr_external_ram(const void *p);

#ifdef __cplusplus
}
#endif
