/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

// Follower firmware of the two-board rig: it answers the bus transactions
// driven by the leader (multidevice_a) and coordinates with it over UART1.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <doctest/doctest.h>

#include <driver/gpio.h>
#include <driver/i2c_slave.h>
#include <driver/i2s_std.h>
#include <driver/rmt_rx.h>
#include <driver/spi_slave.h>
#include <driver/uart.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "multidevice_test.hpp"

using namespace multidevice;

// Status LED hook used by the shared test runner (see test_runner.cpp). Only
// one of the two suite sources is compiled into each firmware.
extern "C" void
multidevice_set_status(int value)
{
    set_status(static_cast<status>(value));
}

namespace {

// UART2 is only used by its own test; UART1 (the control link) is installed by
// the shared helpers.
void
data_uart_install()
{
    uart_config_t cfg = {};
    cfg.baud_rate = 115200;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    REQUIRE(uart_param_config(UART_NUM_2, &cfg) == ESP_OK);
    REQUIRE(uart_set_pin(
                    UART_NUM_2, kDataTx, kDataRx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) ==
            ESP_OK);
    REQUIRE(uart_driver_install(UART_NUM_2, 2048, 2048, 0, nullptr, 0) == ESP_OK);
    // The rig has no external pull-up resistors, and this link has no framing
    // to resynchronise on: hold the idle level high with the internal pull-up
    // so a floating line cannot be decoded as a spurious start bit, and drop
    // whatever the driver captured while the peer's TX was still high-Z.
    REQUIRE(gpio_set_pull_mode(kDataRx, GPIO_PULLUP_ONLY) == ESP_OK);
    uart_flush_input(UART_NUM_2);
}

size_t
data_read_exact(void *dst, size_t len, uint32_t timeout_ms)
{
    auto *out = static_cast<uint8_t *>(dst);
    size_t got = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (got < len) {
        const TickType_t now = xTaskGetTickCount();
        if (now >= deadline)
            break;
        const TickType_t wait = std::min<TickType_t>(deadline - now, pdMS_TO_TICKS(50));
        const int r = uart_read_bytes(UART_NUM_2, out + got, len - got, wait);
        if (r > 0)
            got += static_cast<size_t>(r);
    }
    return got;
}

}   // namespace

TEST_SUITE("multidevice_b")
{
    TEST_CASE("the gpio link carries both levels")
    {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << kSyncPin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        REQUIRE(gpio_config(&io) == ESP_OK);

        uint8_t level = 0xFF;
        CHECK(follow_wait(1, level));
        CHECK(level == 0);
        const int read_low = gpio_get_level(kSyncPin);
        CHECK(read_low == 0);
        follow_reply(1, static_cast<uint8_t>(read_low));

        CHECK(follow_wait(2, level));
        CHECK(level == 1);
        const int read_high = gpio_get_level(kSyncPin);
        CHECK(read_high == 1);
        follow_reply(2, static_cast<uint8_t>(read_high));
    }

    TEST_CASE("the uart1 control link carries a payload")
    {
        constexpr size_t kLen = 64;
        uint8_t len = 0;
        CHECK(follow_wait(3, len));
        CHECK(len == kLen);

        std::array<uint8_t, kLen> rx{};
        CHECK(ctrl_read_exact(rx.data(), rx.size(), 10000) == rx.size());
        std::array<uint8_t, kLen> expect{};
        fill_pattern(expect.data(), expect.size(), 0x1234);
        CHECK(std::memcmp(rx.data(), expect.data(), rx.size()) == 0);

        send_frame(kDataAck, 3, crc8(rx.data(), rx.size()));
    }

    TEST_CASE("the uart2 link carries a payload both ways")
    {
        data_uart_install();
        constexpr size_t kLen = 64;

        uint8_t token = 0;
        CHECK(follow_wait(4, token));
        CHECK(token == 0x5A);
        follow_reply(4, 0);

        std::array<uint8_t, kLen> rx{};
        CHECK(data_read_exact(rx.data(), rx.size(), 5000) == rx.size());
        std::array<uint8_t, kLen> expect{};
        fill_pattern(expect.data(), expect.size(), token);
        CHECK(std::memcmp(rx.data(), expect.data(), rx.size()) == 0);

        std::array<uint8_t, kLen> echo{};
        fill_pattern(echo.data(), echo.size(), static_cast<uint32_t>(token) ^ 0xFFu);
        CHECK(uart_write_bytes(UART_NUM_2, echo.data(), echo.size()) ==
              static_cast<int>(echo.size()));
        CHECK(uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(1000)) == ESP_OK);

        REQUIRE(uart_driver_delete(UART_NUM_2) == ESP_OK);
    }

    TEST_CASE("the spi master and slave exchange data")
    {
        spi_bus_config_t bus = {};
        bus.mosi_io_num = kSpiMosi;
        bus.miso_io_num = kSpiMiso;
        bus.sclk_io_num = kSpiSck;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 16;
        const spi_slave_interface_config_t slv = {
            .spics_io_num = kSpiCs,
            .flags = 0,
            .queue_size = 1,
            .mode = 0,
            .post_setup_cb = nullptr,
            .post_trans_cb = nullptr,
        };
        REQUIRE(spi_slave_initialize(SPI2_HOST, &bus, &slv, SPI_DMA_DISABLED) == ESP_OK);

        uint8_t token = 0;
        CHECK(follow_wait(8, token));
        CHECK(token == 0x69);

        std::array<uint8_t, 4> tx{};
        std::array<uint8_t, 4> rx{};
        fill_pattern(tx.data(), tx.size(), static_cast<uint32_t>(token) ^ 0x5Cu);
        spi_slave_transaction_t trans = {};
        trans.length = tx.size() * 8;
        trans.tx_buffer = tx.data();
        trans.rx_buffer = rx.data();
        CHECK(spi_slave_queue_trans(SPI2_HOST, &trans, pdMS_TO_TICKS(1000)) == ESP_OK);
        follow_reply(8, 0);

        spi_slave_transaction_t *done = nullptr;
        CHECK(spi_slave_get_trans_result(SPI2_HOST, &done, pdMS_TO_TICKS(3000)) == ESP_OK);
        std::array<uint8_t, 4> expect{};
        fill_pattern(expect.data(), expect.size(), token);
        CHECK(std::memcmp(rx.data(), expect.data(), rx.size()) == 0);

        REQUIRE(spi_slave_free(SPI2_HOST) == ESP_OK);
    }

    TEST_CASE("the i2s master sends samples to the slave")
    {
        const i2s_chan_config_t chan =
                I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_SLAVE);
        i2s_chan_handle_t rx = nullptr;
        REQUIRE(i2s_new_channel(&chan, nullptr, &rx) == ESP_OK);

        const i2s_std_config_t cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = kI2sBclk,
                .ws = kI2sWs,
                .dout = I2S_GPIO_UNUSED,
                .din = kI2sSd,
                .invert_flags = {},
            },
        };
        REQUIRE(i2s_channel_init_std_mode(rx, &cfg) == ESP_OK);
        REQUIRE(i2s_channel_enable(rx) == ESP_OK);

        uint8_t token = 0;
        CHECK(follow_wait(9, token));
        CHECK(token == 0x27);
        follow_reply(9, 0);

        // The master clock is already running when its task starts writing, so
        // the first DMA buffer it sends is silence: read several buffers' worth
        // (and keep the buffer off the 4 KB main task stack) to reach the
        // pattern. The read blocks until the buffer fills (or times out) while
        // the master keeps streaming.
        static std::array<uint8_t, 8192> buf{};
        buf.fill(0);
        size_t read = 0;
        CHECK(i2s_channel_read(rx, buf.data(), buf.size(), &read, 3000) == ESP_OK);

        std::array<uint8_t, 32> pattern{};
        fill_pattern(pattern.data(), pattern.size(), token);
        const auto end = buf.begin() + static_cast<std::ptrdiff_t>(read);
        CHECK(std::search(buf.begin(), end, pattern.begin(), pattern.end()) != end);

        REQUIRE(i2s_channel_disable(rx) == ESP_OK);
        REQUIRE(i2s_del_channel(rx) == ESP_OK);
    }

    TEST_CASE("the rmt link reproduces the transmitted waveform")
    {
        const rmt_rx_channel_config_t rxcfg = {
            .gpio_num = kRmtPin,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 1000000,
            .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
            .intr_priority = 0,
            .flags = { .invert_in = 0, .with_dma = 0, .allow_pd = 0 },
        };
        rmt_channel_handle_t rx = nullptr;
        REQUIRE(rmt_new_rx_channel(&rxcfg, &rx) == ESP_OK);
        REQUIRE(rmt_enable(rx) == ESP_OK);

        static rmt_capture capture;
        capture.reset();
        const rmt_rx_event_callbacks_t cbs = { .on_recv_done = &rmt_capture::on_done };
        REQUIRE(rmt_rx_register_event_callbacks(rx, &cbs, &capture) == ESP_OK);

        const std::array<rmt_symbol_word_t, 8> pattern = { {
                { .duration0 = 10, .level0 = 1, .duration1 = 20, .level1 = 0 },
                { .duration0 = 30, .level0 = 1, .duration1 = 15, .level1 = 0 },
                { .duration0 = 25, .level0 = 1, .duration1 = 25, .level1 = 0 },
                { .duration0 = 5, .level0 = 1, .duration1 = 40, .level1 = 0 },
                { .duration0 = 12, .level0 = 1, .duration1 = 8, .level1 = 0 },
                { .duration0 = 18, .level0 = 1, .duration1 = 22, .level1 = 0 },
                { .duration0 = 7, .level0 = 1, .duration1 = 33, .level1 = 0 },
                { .duration0 = 16, .level0 = 1, .duration1 = 24, .level1 = 0 },
        } };

        // Arm the receiver only when the peer is about to transmit. The leader
        // sends the waveform as soon as it sees our reply, but it stays silent
        // while it waits for the step above (it can be held back by the
        // previous test for hundreds of milliseconds), and a receiver armed
        // too early stops on the idle line. For the same reason the idle
        // threshold has to clear the control-link round trip between arming
        // and the transmission.
        const rmt_receive_config_t rcfg = {
            .signal_range_min_ns = 100,
            .signal_range_max_ns = 30000000,
            .flags = { .en_partial_rx = 0 },
        };

        uint8_t ready = 0;
        CHECK(follow_wait(10, ready));
        REQUIRE(rmt_receive(rx,
                            capture.symbols.data(),
                            capture.symbols.size() * sizeof(rmt_symbol_word_t),
                            &rcfg) == ESP_OK);
        follow_reply(10, 0);
        CHECK(wait_flag(capture.done, 3000));

        const size_t index = find_symbol_pattern(capture, pattern);
        CHECK(index != SIZE_MAX);

        REQUIRE(rmt_disable(rx) == ESP_OK);
        REQUIRE(rmt_del_channel(rx) == ESP_OK);
    }

    TEST_CASE("the pwm output is measured by the peer")
    {
        const rmt_rx_channel_config_t rxcfg = {
            .gpio_num = kPwmPin,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 1000000,
            .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
            .intr_priority = 0,
            .flags = { .invert_in = 0, .with_dma = 0, .allow_pd = 0 },
        };
        rmt_channel_handle_t rx = nullptr;
        REQUIRE(rmt_new_rx_channel(&rxcfg, &rx) == ESP_OK);
        REQUIRE(rmt_enable(rx) == ESP_OK);

        static rmt_capture capture;
        capture.reset();
        const rmt_rx_event_callbacks_t cbs = { .on_recv_done = &rmt_capture::on_done };
        REQUIRE(rmt_rx_register_event_callbacks(rx, &cbs, &capture) == ESP_OK);

        const rmt_receive_config_t rcfg = {
            .signal_range_min_ns = 100,
            .signal_range_max_ns = 1000000,
            .flags = { .en_partial_rx = 0 },
        };
        REQUIRE(rmt_receive(rx,
                            capture.symbols.data(),
                            capture.symbols.size() * sizeof(rmt_symbol_word_t),
                            &rcfg) == ESP_OK);

        uint8_t ready = 0;
        CHECK(follow_wait(11, ready));
        follow_reply(11, 0);
        CHECK(wait_flag(capture.done, 3000));

        const uint32_t high = captured_level_time(capture, 1);
        const uint32_t low = captured_level_time(capture, 0);
        const uint32_t period = high + low;
        int duty = 0;
        if (period > 0)
            duty = static_cast<int>(high * 100 / period);
        CHECK(period > 0);
        CHECK(duty >= 15);
        CHECK(duty <= 35);

        REQUIRE(rmt_disable(rx) == ESP_OK);
        REQUIRE(rmt_del_channel(rx) == ESP_OK);
    }

    TEST_CASE("the i2c master and slave exchange data")
    {
        i2c_slave_config_t cfg = {};
        cfg.i2c_port = I2C_NUM_0;
        cfg.sda_io_num = kI2cSda;
        cfg.scl_io_num = kI2cScl;
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        cfg.send_buf_depth = 128;
        cfg.receive_buf_depth = 128;
        cfg.slave_addr = kI2cSlaveAddr;
        cfg.addr_bit_len = I2C_ADDR_BIT_LEN_7;
        cfg.intr_priority = 0;
        // The rig carries external 10K pull-ups on both I2C lines, so the weak
        // internal ones are left disabled.
        cfg.flags.enable_internal_pullup = 0;
        i2c_slave_dev_handle_t slave = nullptr;
        REQUIRE(i2c_new_slave_device(&cfg, &slave) == ESP_OK);

        static i2c_capture capture;
        capture.reset();
        capture.requested = xSemaphoreCreateBinary();
        REQUIRE(capture.requested != nullptr);
        const i2c_slave_event_callbacks_t cbs = {
            .on_request = &i2c_capture::on_request,
            .on_receive = &i2c_capture::on_receive,
        };
        REQUIRE(i2c_slave_register_event_callbacks(slave, &cbs, &capture) == ESP_OK);

        // The slave driver does not reset the TX FIFO when the device is
        // created: anything left there by the boot or an earlier peripheral
        // would be clocked out by the master ahead of the answer.
        REQUIRE(i2c_slave_reset_tx_fifo(slave) == ESP_OK);

        constexpr uint8_t kToken = 0x3C;
        constexpr size_t kLen = 8;

        // Single rendezvous up front. After it the exchange runs over I2C only
        // until the final rendezvous below, so this task can park on the
        // request semaphore and answer the read while the peripheral still
        // holds the address-match stretch. Extra control-link traffic here
        // would delay the task past that stretch, the slave would start
        // shifting whatever the TX FIFO holds, and the master would clock out
        // stale bytes (see report.md).
        uint8_t token = 0;
        CHECK(follow_wait(12, token));
        CHECK(token == kToken);
        follow_reply(12, 0);

        std::array<uint8_t, kLen> response{};
        fill_pattern(response.data(), response.size(), static_cast<uint32_t>(kToken) ^ 0xA5u);

        // Park before the read. The master writes its pattern first, captured
        // by the receive callback; the read request that follows wakes this
        // task, whose single i2c_slave_write() fills the FIFO and releases the
        // stretch in one go. There is no pre-queued answer and no second,
        // zero-length write to release the stretch separately.
        CHECK(xSemaphoreTake(capture.requested, pdMS_TO_TICKS(1000)) == pdTRUE);
        uint32_t written = 0;
        CHECK(i2c_slave_write(slave, response.data(), response.size(), &written, 1000) ==
              ESP_OK);
        CHECK(written == response.size());

        // The write phase is captured by the receive callback.
        CHECK(wait_flag(capture.done, 5000));
        CHECK(capture.length == kLen);
        std::array<uint8_t, kLen> captured{};
        std::copy(capture.data.begin(), capture.data.begin() + kLen, captured.begin());
        std::array<uint8_t, kLen> expect{};
        fill_pattern(expect.data(), expect.size(), kToken);
        CHECK(std::memcmp(captured.data(), expect.data(), kLen) == 0);

        // Final rendezvous: exchange the CRC each board computed for the bytes
        // it sent or received, so both agree on the exchange.
        uint8_t master_crc = 0;
        CHECK(follow_wait(13, master_crc));
        CHECK(master_crc == crc8(captured.data(), captured.size()));
        follow_reply(13, crc8(captured.data(), captured.size()));

        REQUIRE(i2c_del_slave_device(slave) == ESP_OK);
        vSemaphoreDelete(capture.requested);
        capture.requested = nullptr;
    }
}
