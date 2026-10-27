/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mock_bus {

struct i2c_write_record
{
    uint8_t reg;
    uint8_t val;
};

// Reset all mock state: register map, write history and failure flags.
void mock_i2c_reset();

// Direct access to the emulated I2C register map.
void mock_i2c_set_reg(uint8_t reg, uint8_t val);

// Like mock_i2c_set_reg(), but writes through the I2C mock never change the
// value again (e.g. to emulate a hardware strapping or a stuck bit).
void mock_i2c_set_sticky_reg(uint8_t reg, uint8_t val);

[[nodiscard]] uint8_t mock_i2c_get_reg(uint8_t reg);

// Ordered history of every register write performed through the I2C mock.
[[nodiscard]] const std::vector<i2c_write_record> &mock_i2c_writes();
[[nodiscard]] bool mock_i2c_wrote(uint8_t reg, uint8_t val);

// Counters for the I2C bus add/remove device calls.
[[nodiscard]] unsigned mock_i2c_add_device_count();
[[nodiscard]] unsigned mock_i2c_rm_device_count();

// Device address passed to the last i2c_master_bus_add_device() call.
[[nodiscard]] uint8_t mock_i2c_last_device_address();

// Failure injection: make the mocked driver calls return an error.
void mock_i2c_fail_writes(bool fail);
void mock_i2c_fail_reads(bool fail);
void mock_i2c_fail_add_device(bool fail);
void mock_i2c_fail_rm_device(bool fail);

// Fail the \p n i2c_master_transmit() calls after the first \p skip calls,
// then succeed again (useful to test error recovery in the middle of a
// write sequence).
void mock_i2c_fail_writes_times(unsigned n, unsigned skip = 0);

// I2S write mock: last chunk handed to i2s_channel_write().
void mock_i2s_fail_writes(bool fail);
// One-shot partial I2S write failure: the next i2s_channel_write() reports
// that the first \p bytes were transferred and then fails.
void mock_i2s_fail_writes_partial(size_t bytes);
[[nodiscard]] size_t mock_i2s_last_size();
[[nodiscard]] const std::vector<uint8_t> &mock_i2s_last_data();

// I2S read mock: data returned by i2s_channel_read().
void mock_i2s_set_read_data(const void *data, size_t size);
void mock_i2s_fail_reads(bool fail);

}   // namespace mock_bus
