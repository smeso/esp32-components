/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>

namespace mock_bus {

// Reset all RMT mock state: handles, transmit history and failure flags.
void mock_rmt_reset();

// Failure injection for the mocked RMT driver entry points.
void mock_rmt_fail_new_tx_channel(bool fail);
void mock_rmt_fail_new_bytes_encoder(bool fail);
void mock_rmt_fail_enable(bool fail);
void mock_rmt_fail_disable(bool fail);
void mock_rmt_fail_del_encoder(bool fail);
void mock_rmt_fail_del_channel(bool fail);
void mock_rmt_fail_transmit(bool fail);
void mock_rmt_fail_wait_all_done(bool fail);

// Payload handed to the last rmt_transmit() call.
[[nodiscard]] size_t mock_rmt_last_payload_size();
[[nodiscard]] const std::vector<uint8_t> &mock_rmt_last_payload();
[[nodiscard]] const void *mock_rmt_last_payload_ptr();

// Number of rmt_disable() / rmt_enable() calls seen so far. Used to check
// that a failed transmission aborts the channel.
[[nodiscard]] unsigned mock_rmt_disable_count();
[[nodiscard]] unsigned mock_rmt_enable_count();

// Handles handed out by the mocked channel/encoder constructors.
[[nodiscard]] rmt_channel_handle_t mock_rmt_channel_handle();
[[nodiscard]] rmt_encoder_handle_t mock_rmt_encoder_handle();

}   // namespace mock_bus
