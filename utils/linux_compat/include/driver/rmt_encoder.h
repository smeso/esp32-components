/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal stand-in for the esp_driver_rmt <driver/rmt_encoder.h> header, used
 * only for the FreeRTOS Linux simulator build. See driver/rmt_types.h.
 */

#pragma once

#include <esp_err.h>

#include "driver/rmt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rmt_new_bytes_encoder(const rmt_bytes_encoder_config_t *config,
                                rmt_encoder_handle_t *ret_encoder);
esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder);

#ifdef __cplusplus
}
#endif
