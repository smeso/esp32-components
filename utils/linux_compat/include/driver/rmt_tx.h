/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal stand-in for the esp_driver_rmt <driver/rmt_tx.h> header, used
 * only for the FreeRTOS Linux simulator build. See driver/rmt_types.h.
 */

#pragma once

#include <esp_err.h>

#include "driver/rmt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *config,
                             rmt_channel_handle_t *ret_chan);
esp_err_t rmt_enable(rmt_channel_handle_t channel);
esp_err_t rmt_disable(rmt_channel_handle_t channel);
esp_err_t rmt_del_channel(rmt_channel_handle_t channel);
esp_err_t rmt_transmit(rmt_channel_handle_t channel,
                       rmt_encoder_handle_t encoder,
                       const void *payload,
                       size_t payload_bytes,
                       const rmt_transmit_config_t *config);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t channel, int timeout_ms);

#ifdef __cplusplus
}
#endif
