/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <driver/gpio.h>
#include <driver/i2c_slave.h>
#include <driver/rmt_types.h>
#include <driver/uart.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "multidevice_status.hpp"

// Shared helpers for the two-board test rig. The suite is chosen at build time:
// multidevice_a.cpp is the leader (it drives the buses) and multidevice_b.cpp is
// the follower. Both firmwares coordinate over UART1 (GPIO 3/4) with the small
// framed protocol below and use the status LED for operator feedback.
namespace multidevice {

//
// Pin map of the rig (see the connection table in the task description).
//
inline constexpr gpio_num_t kCtrlTx = GPIO_NUM_3;     // UART1, cross connected
inline constexpr gpio_num_t kCtrlRx = GPIO_NUM_4;     // UART1, cross connected
inline constexpr gpio_num_t kDataTx = GPIO_NUM_5;     // UART2, cross connected
inline constexpr gpio_num_t kDataRx = GPIO_NUM_6;     // UART2, cross connected
inline constexpr gpio_num_t kSpiSck = GPIO_NUM_9;     // SPI, direct
inline constexpr gpio_num_t kSpiMosi = GPIO_NUM_10;   // SPI, direct
inline constexpr gpio_num_t kSpiMiso = GPIO_NUM_11;   // SPI, direct
inline constexpr gpio_num_t kSpiCs = GPIO_NUM_12;     // SPI, direct
inline constexpr gpio_num_t kI2sBclk = GPIO_NUM_13;   // I2S0, direct
inline constexpr gpio_num_t kI2sWs = GPIO_NUM_14;     // I2S0, direct
inline constexpr gpio_num_t kI2sSd = GPIO_NUM_15;     // I2S0, direct
inline constexpr gpio_num_t kRmtPin = GPIO_NUM_16;    // RMT link, direct
inline constexpr gpio_num_t kSyncPin = GPIO_NUM_17;   // GPIO link, direct
inline constexpr gpio_num_t kPwmPin = GPIO_NUM_18;    // GPIO/PWM link, direct
inline constexpr gpio_num_t kI2cSda = GPIO_NUM_7;     // I2C, direct, external pull-up
inline constexpr gpio_num_t kI2cScl = GPIO_NUM_8;     // I2C, direct, external pull-up
inline constexpr uint16_t kI2cSlaveAddr = 0x42;       // 7-bit I2C slave address

#if defined(MD_ROLE_LEADER)
inline constexpr bool kLeader = true;
#else
inline constexpr bool kLeader = false;
#endif

//
// Framed control protocol over UART1. Every frame is
// [0xAA][type][arg0][arg1][crc] where crc = type ^ arg0 ^ arg1 ^ 0x5A.
//
inline constexpr uint8_t kSyncByte = 0xAA;

enum frame_type : uint8_t
{
    kHello = 0x01,
    kHelloAck = 0x02,
    kStep = 0x03,
    kReply = 0x04,
    kDataAck = 0x05,
};

inline uint8_t
frame_crc(uint8_t type, uint8_t a, uint8_t b)
{
    return static_cast<uint8_t>(type ^ a ^ b ^ 0x5A);
}

inline void
ctrl_install()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    uart_config_t cfg = {};
    cfg.baud_rate = 115200;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(
            UART_NUM_1, kCtrlTx, kCtrlRx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 2048, 2048, 0, nullptr, 0));
    // The rig has no external pull-up resistors: the peer's TX pin is left
    // high-Z until its firmware configures it, so the receive line would float
    // and the UART could sample a spurious start bit. Hold the idle level high
    // with the internal pull-up and drop anything captured while it floated.
    ESP_ERROR_CHECK(gpio_set_pull_mode(kCtrlRx, GPIO_PULLUP_ONLY));
    uart_flush_input(UART_NUM_1);
}

inline void
ctrl_write(const void *data, size_t len)
{
    uart_write_bytes(UART_NUM_1, data, len);
    uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(1000));
}

inline size_t
ctrl_read_exact(void *dst, size_t len, uint32_t timeout_ms)
{
    auto *out = static_cast<uint8_t *>(dst);
    size_t got = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (got < len) {
        const TickType_t now = xTaskGetTickCount();
        if (now >= deadline)
            break;
        const TickType_t wait = std::min<TickType_t>(deadline - now, pdMS_TO_TICKS(50));
        const int r = uart_read_bytes(UART_NUM_1, out + got, len - got, wait);
        if (r > 0)
            got += static_cast<size_t>(r);
    }
    return got;
}

inline void
ctrl_drain(uint32_t ms)
{
    std::array<uint8_t, 64> scratch{};
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (xTaskGetTickCount() < deadline)
        uart_read_bytes(UART_NUM_1, scratch.data(), scratch.size(), pdMS_TO_TICKS(20));
    uart_flush_input(UART_NUM_1);
}

inline void
send_frame(uint8_t type, uint8_t a, uint8_t b)
{
    const std::array<uint8_t, 5> frame{ kSyncByte, type, a, b, frame_crc(type, a, b) };
    ctrl_write(frame.data(), frame.size());
}

inline bool
recv_frame(uint8_t &type, uint8_t &a, uint8_t &b, uint32_t timeout_ms)
{
    uint8_t byte = 0;
    for (;;) {
        if (ctrl_read_exact(&byte, 1, timeout_ms) != 1)
            return false;
        if (byte == kSyncByte)
            break;
    }

    std::array<uint8_t, 4> rest{};
    if (ctrl_read_exact(rest.data(), rest.size(), timeout_ms) != rest.size())
        return false;
    if (frame_crc(rest[0], rest[1], rest[2]) != rest[3])
        return false;

    type = rest[0];
    a = rest[1];
    b = rest[2];
    return true;
}

// Initial rendezvous so both boards start the sequence together. The follower
// keeps sending HELLO until the leader answers; the leader then drains any
// HELLO still in flight.
inline bool
sync()
{
    static bool done = false;
    if (done)
        return true;
    ctrl_install();

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
    if (kLeader) {
        while (xTaskGetTickCount() < deadline) {
            uint8_t type = 0;
            uint8_t a = 0;
            uint8_t b = 0;
            if (recv_frame(type, a, b, 1000) && type == kHello) {
                send_frame(kHelloAck, 0, 0);
                ctrl_drain(300);
                done = true;
                return true;
            }
        }
        return false;
    }

    while (xTaskGetTickCount() < deadline) {
        send_frame(kHello, 0, 0);
        uint8_t type = 0;
        uint8_t a = 0;
        uint8_t b = 0;
        if (recv_frame(type, a, b, 300) && type == kHelloAck) {
            ctrl_drain(20);
            done = true;
            return true;
        }
    }
    return false;
}

//
// Rendezvous helpers. The leader sends a STEP frame carrying an argument and
// waits for the follower's REPLY; the follower waits for the STEP and answers
// with a value computed from the argument.
//
inline bool
lead_send(uint8_t step, uint8_t arg)
{
    if (!sync())
        return false;
    send_frame(kStep, step, arg);
    return true;
}

inline bool
lead_wait(uint8_t step, uint8_t &value, uint32_t timeout_ms = 10000)
{
    uint8_t type = 0;
    uint8_t a = 0;
    uint8_t b = 0;
    if (!recv_frame(type, a, b, timeout_ms))
        return false;
    if (type != kReply || a != step)
        return false;
    value = b;
    return true;
}

inline bool
follow_wait(uint8_t step, uint8_t &arg, uint32_t timeout_ms = 60000)
{
    if (!sync())
        return false;
    uint8_t type = 0;
    uint8_t a = 0;
    uint8_t b = 0;
    if (!recv_frame(type, a, b, timeout_ms))
        return false;
    if (type != kStep || a != step)
        return false;
    arg = b;
    return true;
}

inline void
follow_reply(uint8_t step, uint8_t value)
{
    send_frame(kReply, step, value);
}

//
// Small deterministic helpers shared by both firmwares.
//
inline uint8_t
crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) :
                                 static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

inline void
fill_pattern(uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t x = seed | 1u;
    for (size_t i = 0; i < len; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = static_cast<uint8_t>(x & 0xFF);
    }
}

inline bool
wait_flag(volatile bool &flag, uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!flag && xTaskGetTickCount() < deadline)
        vTaskDelay(pdMS_TO_TICKS(1));
    return flag;
}

//
// RMT receive buffer shared by the RMT and the PWM measurement tests.
//
struct rmt_capture
{
    static constexpr size_t kMaxSymbols = 64;

    std::array<rmt_symbol_word_t, kMaxSymbols> symbols{};
    volatile size_t count = 0;
    volatile bool done = false;

    static bool on_done(rmt_channel_handle_t /*chan*/,
                        const rmt_rx_done_event_data_t *edata,
                        void *ctx)
    {
        auto *self = static_cast<rmt_capture *>(ctx);
        const size_t n = std::min(edata->num_symbols, kMaxSymbols);
        std::memcpy(
                self->symbols.data(), edata->received_symbols, n * sizeof(rmt_symbol_word_t));
        self->count = n;
        self->done = true;
        return false;
    }

    void reset()
    {
        count = 0;
        done = false;
        symbols.fill({});
    }
};

//
// I2C slave state used by the I2C test. Both driver callbacks run from ISR
// context: the receive callback copies the captured bytes out and arms the
// read, while the request callback wakes the task so it can answer a read. A
// read holds SCL low from its address match until i2c_slave_write() releases
// the stretch, which cannot be done from the callback itself, so the task must
// already be blocked on `requested` when the read arrives.
//
struct i2c_capture
{
    static constexpr size_t kMaxBytes = 32;

    std::array<uint8_t, kMaxBytes> data{};
    volatile size_t length = 0;
    volatile bool done = false;
    // Set once a write has been received. The request callback only wakes the
    // task for a read that follows such a write, so that an address match of
    // the write itself (if the peripheral reports one) cannot make the task
    // answer before the read it is meant to serve.
    volatile bool read_armed = false;
    SemaphoreHandle_t requested = nullptr;

    static bool on_receive(i2c_slave_dev_handle_t /*slave*/,
                           const i2c_slave_rx_done_event_data_t *edata,
                           void *ctx)
    {
        auto *self = static_cast<i2c_capture *>(ctx);
        const size_t n = std::min<size_t>(edata->length, kMaxBytes);
        std::memcpy(self->data.data(), edata->buffer, n);
        self->length = n;
        self->done = true;
        self->read_armed = true;
        return false;
    }

    static bool on_request(i2c_slave_dev_handle_t /*slave*/,
                           const i2c_slave_request_event_data_t * /*edata*/,
                           void *ctx)
    {
        auto *self = static_cast<i2c_capture *>(ctx);
        if (!self->read_armed)
            return false;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(self->requested, &woken);
        return woken == pdTRUE;
    }

    void reset()
    {
        length = 0;
        done = false;
        read_armed = false;
        data.fill(0);
    }
};

// Total time spent at the given level across the captured symbols.
inline uint32_t
captured_level_time(const rmt_capture &cap, uint32_t level)
{
    uint32_t total = 0;
    for (size_t i = 0; i < cap.count; ++i) {
        const rmt_symbol_word_t &sym = cap.symbols[i];
        if (sym.level0 == level)
            total += sym.duration0;
        if (sym.level1 == level)
            total += sym.duration1;
    }
    return total;
}

// Index of the first symbol equal to `first` followed by the rest of `pattern`.
inline size_t
find_symbol_pattern(const rmt_capture &cap, std::span<const rmt_symbol_word_t> pattern)
{
    if (pattern.empty() || cap.count < pattern.size())
        return SIZE_MAX;
    for (size_t start = 0; start + pattern.size() <= cap.count; ++start) {
        bool match = true;
        for (size_t i = 0; i < pattern.size(); ++i) {
            if (cap.symbols[start + i].val != pattern[i].val) {
                match = false;
                break;
            }
        }
        if (match)
            return start;
    }
    return SIZE_MAX;
}

}   // namespace multidevice
