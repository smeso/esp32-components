/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mock_bus.hpp"

#include <algorithm>
#include <cstring>

#include <esp_err.h>

#include <driver/i2c_master.h>
#include <driver/i2s_common.h>

namespace mock_bus {

namespace {

uint8_t regs[256];
bool sticky_regs[256];
std::vector<i2c_write_record> write_history;
unsigned add_device_count = 0;
unsigned rm_device_count = 0;
uint8_t last_device_address = 0;
bool fail_writes = false;
bool fail_reads = false;
bool fail_add_device = false;
bool fail_rm_device = false;
unsigned fail_writes_remaining = 0;
unsigned fail_writes_skip = 0;
bool fail_i2s_write = false;
bool fail_i2s_read = false;
size_t fail_i2s_write_partial = 0;
size_t i2s_last_size = 0;
std::vector<uint8_t> i2s_last_data;
std::vector<uint8_t> i2s_read_data;

}   // namespace

// Expose the counter to the __wrap_* hooks, which live outside the anonymous
// namespace but inside mock_bus.
bool
should_fail_write()
{
    if (fail_writes_skip > 0) {
        --fail_writes_skip;
        return false;
    }
    if (fail_writes_remaining > 0) {
        --fail_writes_remaining;
        return true;
    }
    return false;
}

void
mock_i2c_reset()
{
    std::memset(regs, 0, sizeof(regs));
    std::memset(sticky_regs, 0, sizeof(sticky_regs));
    write_history.clear();
    add_device_count = 0;
    rm_device_count = 0;
    last_device_address = 0;
    fail_writes = false;
    fail_reads = false;
    fail_add_device = false;
    fail_rm_device = false;
    fail_writes_remaining = 0;
    fail_writes_skip = 0;
    fail_i2s_write = false;
    fail_i2s_read = false;
    fail_i2s_write_partial = 0;
    i2s_last_size = 0;
    i2s_last_data.clear();
    i2s_read_data.clear();
}

void
mock_i2c_set_reg(uint8_t reg, uint8_t val)
{
    regs[reg] = val;
}

void
mock_i2c_set_sticky_reg(uint8_t reg, uint8_t val)
{
    regs[reg] = val;
    sticky_regs[reg] = true;
}

uint8_t
mock_i2c_get_reg(uint8_t reg)
{
    return regs[reg];
}

const std::vector<i2c_write_record> &
mock_i2c_writes()
{
    return write_history;
}

bool
mock_i2c_wrote(uint8_t reg, uint8_t val)
{
    for (const auto &w : write_history) {
        if (w.reg == reg && w.val == val)
            return true;
    }
    return false;
}

unsigned
mock_i2c_add_device_count()
{
    return add_device_count;
}

unsigned
mock_i2c_rm_device_count()
{
    return rm_device_count;
}

uint8_t
mock_i2c_last_device_address()
{
    return last_device_address;
}

void
mock_i2c_fail_writes(bool fail)
{
    fail_writes = fail;
}

void
mock_i2c_fail_reads(bool fail)
{
    fail_reads = fail;
}

void
mock_i2c_fail_add_device(bool fail)
{
    fail_add_device = fail;
}

void
mock_i2c_fail_rm_device(bool fail)
{
    fail_rm_device = fail;
}

void
mock_i2c_fail_writes_times(unsigned n, unsigned skip)
{
    fail_writes = false;
    fail_writes_skip = skip;
    fail_writes_remaining = n;
}

void
mock_i2s_fail_writes(bool fail)
{
    fail_i2s_write = fail;
}

void
mock_i2s_fail_writes_partial(size_t bytes)
{
    fail_i2s_write_partial = bytes;
}

void
mock_i2s_set_read_data(const void *data, size_t size)
{
    i2s_read_data.assign(static_cast<const uint8_t *>(data),
                         static_cast<const uint8_t *>(data) + size);
}

void
mock_i2s_fail_reads(bool fail)
{
    fail_i2s_read = fail;
}

size_t
mock_i2s_last_size()
{
    return i2s_last_size;
}

const std::vector<uint8_t> &
mock_i2s_last_data()
{
    return i2s_last_data;
}

}   // namespace mock_bus

// ---------------------------------------------------------------------------
// Mocked ESP-IDF driver entry points.
//
// The test component is linked into the firmware before the real esp_driver_i2c
// and esp_driver_i2s libraries, so the linker --wrap options below redirect
// every reference to these functions to the __wrap_* implementations, letting
// the tests drive their device drivers without real hardware.
// ---------------------------------------------------------------------------

namespace mock_bus {

void
record_add_device(uint8_t address)
{
    last_device_address = address;
    ++add_device_count;
}

void
record_rm_device()
{
    ++rm_device_count;
}

void
record_write(uint8_t reg, uint8_t val)
{
    if (!sticky_regs[reg])
        regs[reg] = val;
    write_history.push_back({ reg, val });
}

void
record_i2s_data(const void *src, size_t size)
{
    i2s_last_size = size;
    i2s_last_data.assign(static_cast<const uint8_t *>(src),
                         static_cast<const uint8_t *>(src) + size);
}

}   // namespace mock_bus

extern "C" {

esp_err_t
__wrap_i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle,
                                 const i2c_device_config_t *dev_config,
                                 i2c_master_dev_handle_t *ret_handle)
{
    (void) bus_handle;
    if (mock_bus::fail_add_device)
        return ESP_ERR_NO_MEM;
    mock_bus::record_add_device(dev_config->device_address);
    *ret_handle = reinterpret_cast<i2c_master_dev_handle_t>(0x1234);
    return ESP_OK;
}

esp_err_t
__wrap_i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
    (void) handle;
    if (mock_bus::fail_rm_device)
        return ESP_ERR_INVALID_STATE;
    mock_bus::record_rm_device();
    return ESP_OK;
}

esp_err_t
__wrap_i2c_master_transmit(i2c_master_dev_handle_t i2c_dev,
                           const uint8_t *write_buffer,
                           size_t write_size,
                           int xfer_timeout_ms)
{
    (void) i2c_dev;
    (void) xfer_timeout_ms;
    if (mock_bus::should_fail_write())
        return ESP_ERR_TIMEOUT;
    if (mock_bus::fail_writes)
        return ESP_ERR_TIMEOUT;
    if (write_size < 2)
        return ESP_ERR_INVALID_SIZE;
    for (size_t i = 1; i < write_size; ++i) {
        mock_bus::record_write(write_buffer[0] + static_cast<uint8_t>(i - 1), write_buffer[i]);
    }
    return ESP_OK;
}

esp_err_t
__wrap_i2c_master_transmit_receive(i2c_master_dev_handle_t i2c_dev,
                                   const uint8_t *write_buffer,
                                   size_t write_size,
                                   uint8_t *read_buffer,
                                   size_t read_size,
                                   int xfer_timeout_ms)
{
    (void) i2c_dev;
    (void) xfer_timeout_ms;
    if (mock_bus::fail_reads)
        return ESP_ERR_TIMEOUT;
    if (write_size != 1 || read_size == 0)
        return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < read_size; ++i) {
        read_buffer[i] = mock_bus::mock_i2c_get_reg(
                static_cast<uint8_t>(write_buffer[0] + static_cast<uint8_t>(i)));
    }
    return ESP_OK;
}

esp_err_t
__wrap_i2s_channel_write(i2s_chan_handle_t handle,
                         const void *src,
                         size_t size,
                         size_t *bytes_written,
                         uint32_t timeout_ms)
{
    (void) handle;
    (void) timeout_ms;
    if (mock_bus::fail_i2s_write_partial != 0) {
        // Simulate a partial transfer that then fails: the first bytes make
        // it to the device, the rest do not.
        const size_t done = std::min(mock_bus::fail_i2s_write_partial, size);
        mock_bus::record_i2s_data(src, done);
        mock_bus::fail_i2s_write_partial = 0;
        if (bytes_written)
            *bytes_written = done;
        return ESP_ERR_INVALID_STATE;
    }
    if (mock_bus::fail_i2s_write) {
        if (bytes_written)
            *bytes_written = 0;
        return ESP_ERR_INVALID_STATE;
    }
    mock_bus::record_i2s_data(src, size);
    if (bytes_written)
        *bytes_written = size;
    return ESP_OK;
}

esp_err_t
__wrap_i2s_channel_read(i2s_chan_handle_t handle,
                        void *dest,
                        size_t size,
                        size_t *bytes_read,
                        uint32_t timeout_ms)
{
    (void) handle;
    (void) timeout_ms;
    if (mock_bus::fail_i2s_read) {
        if (bytes_read)
            *bytes_read = 0;
        return ESP_ERR_INVALID_STATE;
    }
    const size_t to_copy = std::min(size, mock_bus::i2s_read_data.size());
    std::memcpy(dest, mock_bus::i2s_read_data.data(), to_copy);
    mock_bus::i2s_read_data.erase(mock_bus::i2s_read_data.begin(),
                                  mock_bus::i2s_read_data.begin() + to_copy);
    if (bytes_read)
        *bytes_read = to_copy;
    return ESP_OK;
}

}   // extern "C"
