/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mock_rmt.hpp"

#include <esp_err.h>

namespace mock_bus {

namespace {

// Opaque handle values that pass the driver's null checks but are never
// dereferenced: the RMT driver entry points are redirected to the mocks below.
rmt_channel_handle_t channel_handle = reinterpret_cast<rmt_channel_handle_t>(0x1);
rmt_encoder_handle_t encoder_handle = reinterpret_cast<rmt_encoder_handle_t>(0x2);

bool fail_new_tx_channel = false;
bool fail_new_bytes_encoder = false;
bool fail_enable = false;
bool fail_disable = false;
bool fail_del_encoder = false;
bool fail_del_channel = false;
bool fail_transmit = false;
bool fail_wait_all_done = false;
size_t last_payload_size = 0;
const void *last_payload_ptr = nullptr;
std::vector<uint8_t> last_payload;
unsigned disable_count = 0;
unsigned enable_count = 0;

}   // namespace

void
mock_rmt_reset()
{
    fail_new_tx_channel = false;
    fail_new_bytes_encoder = false;
    fail_enable = false;
    fail_disable = false;
    fail_del_encoder = false;
    fail_del_channel = false;
    fail_transmit = false;
    fail_wait_all_done = false;
    last_payload_size = 0;
    last_payload_ptr = nullptr;
    last_payload.clear();
    disable_count = 0;
    enable_count = 0;
}

void
mock_rmt_fail_new_tx_channel(bool fail)
{
    fail_new_tx_channel = fail;
}

void
mock_rmt_fail_new_bytes_encoder(bool fail)
{
    fail_new_bytes_encoder = fail;
}

void
mock_rmt_fail_enable(bool fail)
{
    fail_enable = fail;
}

void
mock_rmt_fail_disable(bool fail)
{
    fail_disable = fail;
}

void
mock_rmt_fail_del_encoder(bool fail)
{
    fail_del_encoder = fail;
}

void
mock_rmt_fail_del_channel(bool fail)
{
    fail_del_channel = fail;
}

void
mock_rmt_fail_transmit(bool fail)
{
    fail_transmit = fail;
}

void
mock_rmt_fail_wait_all_done(bool fail)
{
    fail_wait_all_done = fail;
}

size_t
mock_rmt_last_payload_size()
{
    return last_payload_size;
}

const std::vector<uint8_t> &
mock_rmt_last_payload()
{
    return last_payload;
}

const void *
mock_rmt_last_payload_ptr()
{
    return last_payload_ptr;
}

unsigned
mock_rmt_disable_count()
{
    return disable_count;
}

unsigned
mock_rmt_enable_count()
{
    return enable_count;
}

rmt_channel_handle_t
mock_rmt_channel_handle()
{
    return channel_handle;
}

rmt_encoder_handle_t
mock_rmt_encoder_handle()
{
    return encoder_handle;
}

}   // namespace mock_bus

// ---------------------------------------------------------------------------
// Mocked ESP-IDF RMT driver entry points. The test component is linked into
// the firmware before the real esp_driver_rmt library, so the linker --wrap
// options in this component's CMakeLists.txt redirect every reference to
// these functions to the __wrap_* implementations, letting the tests drive
// their RMT-based drivers without real hardware.
// ---------------------------------------------------------------------------

extern "C" {

esp_err_t
__wrap_rmt_new_tx_channel(const rmt_tx_channel_config_t *config,
                          rmt_channel_handle_t *ret_chan)
{
    (void) config;
    if (mock_bus::fail_new_tx_channel)
        return ESP_ERR_NO_MEM;
    *ret_chan = mock_bus::mock_rmt_channel_handle();
    return ESP_OK;
}

esp_err_t
__wrap_rmt_new_bytes_encoder(const rmt_bytes_encoder_config_t *config,
                             rmt_encoder_handle_t *ret_encoder)
{
    (void) config;
    if (mock_bus::fail_new_bytes_encoder)
        return ESP_ERR_NO_MEM;
    *ret_encoder = mock_bus::mock_rmt_encoder_handle();
    return ESP_OK;
}

esp_err_t
__wrap_rmt_enable(rmt_channel_handle_t channel)
{
    (void) channel;
    ++mock_bus::enable_count;
    if (mock_bus::fail_enable)
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t
__wrap_rmt_disable(rmt_channel_handle_t channel)
{
    (void) channel;
    ++mock_bus::disable_count;
    if (mock_bus::fail_disable)
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t
__wrap_rmt_del_encoder(rmt_encoder_handle_t encoder)
{
    (void) encoder;
    if (mock_bus::fail_del_encoder)
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t
__wrap_rmt_del_channel(rmt_channel_handle_t channel)
{
    (void) channel;
    if (mock_bus::fail_del_channel)
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t
__wrap_rmt_transmit(rmt_channel_handle_t channel,
                    rmt_encoder_handle_t encoder,
                    const void *payload,
                    size_t payload_bytes,
                    const rmt_transmit_config_t *config)
{
    (void) channel;
    (void) encoder;
    (void) config;
    if (mock_bus::fail_transmit)
        return ESP_ERR_INVALID_STATE;
    mock_bus::last_payload_ptr = payload;
    mock_bus::last_payload.assign(static_cast<const uint8_t *>(payload),
                                  static_cast<const uint8_t *>(payload) + payload_bytes);
    mock_bus::last_payload_size = payload_bytes;
    return ESP_OK;
}

esp_err_t
__wrap_rmt_tx_wait_all_done(rmt_channel_handle_t channel, int timeout_ms)
{
    (void) channel;
    (void) timeout_ms;
    if (mock_bus::fail_wait_all_done)
        return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

}   // extern "C"
