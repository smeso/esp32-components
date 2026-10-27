/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal stand-in for the esp_driver_i2s <driver/i2s_common.h> header, used
 * only for the FreeRTOS Linux simulator build. See driver/i2s_types.h.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#include "driver/i2s_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2s_channel_write(i2s_chan_handle_t handle,
                            const void *src,
                            size_t size,
                            size_t *bytes_written,
                            uint32_t timeout_ms);
esp_err_t i2s_channel_read(i2s_chan_handle_t handle,
                           void *dest,
                           size_t size,
                           size_t *bytes_read,
                           uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
