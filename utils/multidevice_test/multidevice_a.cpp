/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

// Leader firmware of the two-board rig: it drives every bus and the follower
// (multidevice_b) answers. Both firmwares are coordinated over UART1.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <doctest/doctest.h>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <driver/ledc.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <driver/spi_master.h>
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

TEST_SUITE("multidevice_a")
{
    TEST_CASE("the gpio link carries both levels")
    {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << kSyncPin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        REQUIRE(gpio_config(&io) == ESP_OK);

        uint8_t value = 0xFF;
        gpio_set_level(kSyncPin, 0);
        vTaskDelay(pdMS_TO_TICKS(2));
        CHECK(lead_send(1, 0));
        CHECK(lead_wait(1, value));
        CHECK(value == 0);

        gpio_set_level(kSyncPin, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
        CHECK(lead_send(2, 1));
        CHECK(lead_wait(2, value));
        CHECK(value == 1);
    }

    TEST_CASE("the uart1 control link carries a payload")
    {
        constexpr size_t kLen = 64;
        std::array<uint8_t, kLen> tx{};
        fill_pattern(tx.data(), tx.size(), 0x1234);

        CHECK(lead_send(3, static_cast<uint8_t>(kLen)));
        ctrl_write(tx.data(), tx.size());

        uint8_t type = 0;
        uint8_t a = 0;
        uint8_t b = 0;
        CHECK(recv_frame(type, a, b, 10000));
        CHECK(type == kDataAck);
        CHECK(a == 3);
        CHECK(b == crc8(tx.data(), tx.size()));
    }

    TEST_CASE("the uart2 link carries a payload both ways")
    {
        data_uart_install();
        constexpr size_t kLen = 64;
        constexpr uint8_t kToken = 0x5A;

        CHECK(lead_send(4, kToken));
        uint8_t ready = 0xFF;
        CHECK(lead_wait(4, ready));
        CHECK(ready == 0);

        std::array<uint8_t, kLen> tx{};
        std::array<uint8_t, kLen> rx{};
        fill_pattern(tx.data(), tx.size(), kToken);
        CHECK(uart_write_bytes(UART_NUM_2, tx.data(), tx.size()) ==
              static_cast<int>(tx.size()));
        CHECK(uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(1000)) == ESP_OK);

        CHECK(data_read_exact(rx.data(), rx.size(), 5000) == rx.size());
        std::array<uint8_t, kLen> expect{};
        fill_pattern(expect.data(), expect.size(), static_cast<uint32_t>(kToken) ^ 0xFFu);
        CHECK(std::memcmp(rx.data(), expect.data(), rx.size()) == 0);

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
        REQUIRE(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED) == ESP_OK);

        spi_device_interface_config_t dev = {};
        dev.clock_speed_hz = 1000000;
        dev.mode = 0;
        dev.spics_io_num = kSpiCs;
        dev.queue_size = 1;
        spi_device_handle_t handle = nullptr;
        REQUIRE(spi_bus_add_device(SPI2_HOST, &dev, &handle) == ESP_OK);

        constexpr uint8_t kToken = 0x69;
        CHECK(lead_send(8, kToken));
        uint8_t ready = 0xFF;
        CHECK(lead_wait(8, ready));
        CHECK(ready == 0);

        std::array<uint8_t, 4> tx{};
        std::array<uint8_t, 4> rx{};
        fill_pattern(tx.data(), tx.size(), kToken);
        spi_transaction_t trans = {};
        trans.length = tx.size() * 8;
        trans.tx_buffer = tx.data();
        trans.rx_buffer = rx.data();
        CHECK(spi_device_polling_transmit(handle, &trans) == ESP_OK);

        std::array<uint8_t, 4> expect{};
        fill_pattern(expect.data(), expect.size(), static_cast<uint32_t>(kToken) ^ 0x5Cu);
        CHECK(std::memcmp(rx.data(), expect.data(), rx.size()) == 0);

        REQUIRE(spi_bus_remove_device(handle) == ESP_OK);
        REQUIRE(spi_bus_free(SPI2_HOST) == ESP_OK);
    }

    TEST_CASE("the i2s master sends samples to the slave")
    {
        const i2s_chan_config_t chan =
                I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
        i2s_chan_handle_t tx = nullptr;
        REQUIRE(i2s_new_channel(&chan, &tx, nullptr) == ESP_OK);

        const i2s_std_config_t cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = kI2sBclk,
                .ws = kI2sWs,
                .dout = kI2sSd,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {},
            },
        };
        REQUIRE(i2s_channel_init_std_mode(tx, &cfg) == ESP_OK);
        REQUIRE(i2s_channel_enable(tx) == ESP_OK);

        constexpr uint8_t kToken = 0x27;
        CHECK(lead_send(9, kToken));
        uint8_t ready = 0xFF;
        CHECK(lead_wait(9, ready));
        CHECK(ready == 0);

        std::array<uint8_t, 32> buf{};
        fill_pattern(buf.data(), buf.size(), kToken);
        const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(500);
        size_t written = 0;
        while (xTaskGetTickCount() < end) {
            if (i2s_channel_write(tx, buf.data(), buf.size(), &written, 200) != ESP_OK)
                break;
        }

        REQUIRE(i2s_channel_disable(tx) == ESP_OK);
        REQUIRE(i2s_del_channel(tx) == ESP_OK);
    }

    TEST_CASE("the rmt link reproduces the transmitted waveform")
    {
        const rmt_tx_channel_config_t txcfg = {
            .gpio_num = kRmtPin,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 1000000,
            .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
            .trans_queue_depth = 4,
            .intr_priority = 0,
            .flags = { .invert_out = 0, .with_dma = 0, .allow_pd = 0, .init_level = 0 },
        };
        rmt_channel_handle_t tx = nullptr;
        REQUIRE(rmt_new_tx_channel(&txcfg, &tx) == ESP_OK);
        REQUIRE(rmt_enable(tx) == ESP_OK);
        const rmt_copy_encoder_config_t enc_cfg = {};
        rmt_encoder_handle_t enc = nullptr;
        REQUIRE(rmt_new_copy_encoder(&enc_cfg, &enc) == ESP_OK);

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
        std::array<rmt_symbol_word_t, 32> stream{};
        for (size_t i = 0; i < stream.size(); ++i)
            stream[i] = pattern[i % pattern.size()];

        CHECK(lead_send(10, 0));
        uint8_t ready = 0xFF;
        CHECK(lead_wait(10, ready));
        CHECK(ready == 0);

        const rmt_transmit_config_t tcfg = {
            .loop_count = 1, .flags = { .eot_level = 0, .queue_nonblocking = 0 }
        };
        CHECK(rmt_transmit(tx,
                           enc,
                           stream.data(),
                           stream.size() * sizeof(rmt_symbol_word_t),
                           &tcfg) == ESP_OK);
        CHECK(rmt_tx_wait_all_done(tx, 1000) == ESP_OK);

        REQUIRE(rmt_del_encoder(enc) == ESP_OK);
        REQUIRE(rmt_disable(tx) == ESP_OK);
        REQUIRE(rmt_del_channel(tx) == ESP_OK);
    }

    TEST_CASE("the pwm output is measured by the peer")
    {
        const ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
            .deconfigure = false,
        };
        REQUIRE(ledc_timer_config(&timer) == ESP_OK);

        ledc_channel_config_t channel = {};
        channel.gpio_num = kPwmPin;
        channel.speed_mode = LEDC_LOW_SPEED_MODE;
        channel.channel = LEDC_CHANNEL_0;
        channel.timer_sel = LEDC_TIMER_0;
        channel.duty = 256;
        channel.hpoint = 0;
        channel.sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD;
        REQUIRE(ledc_channel_config(&channel) == ESP_OK);

        CHECK(lead_send(11, 0));
        uint8_t result = 0xFF;
        CHECK(lead_wait(11, result));
        CHECK(result == 0);

        REQUIRE(ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0) == ESP_OK);
        // The timer keeps running after ledc_stop(): it has to be paused
        // before it can be deconfigured.
        REQUIRE(ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0) == ESP_OK);
        ledc_timer_config_t deconf = {};
        deconf.speed_mode = LEDC_LOW_SPEED_MODE;
        deconf.timer_num = LEDC_TIMER_0;
        deconf.deconfigure = true;
        ledc_timer_config(&deconf);
    }

    TEST_CASE("the i2c master and slave exchange data")
    {
        i2c_master_bus_config_t bus = {};
        bus.i2c_port = I2C_NUM_0;
        bus.sda_io_num = kI2cSda;
        bus.scl_io_num = kI2cScl;
        bus.clk_source = I2C_CLK_SRC_DEFAULT;
        bus.glitch_ignore_cnt = 7;
        bus.intr_priority = 0;
        bus.trans_queue_depth = 0;
        // The rig carries external 10K pull-ups on both I2C lines, so the weak
        // internal ones are left disabled.
        bus.flags.enable_internal_pullup = 0;
        i2c_master_bus_handle_t bus_handle = nullptr;
        REQUIRE(i2c_new_master_bus(&bus, &bus_handle) == ESP_OK);

        i2c_device_config_t dev = {};
        dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev.device_address = kI2cSlaveAddr;
        dev.scl_speed_hz = 100000;
        // The slave holds SCL low from the address match of a read until its
        // task has answered, which takes longer than the default timeout: give
        // the master room to wait out that stretch instead of failing the
        // transfer with ESP_ERR_INVALID_RESPONSE.
        dev.scl_wait_us = 20000;
        i2c_master_dev_handle_t dev_handle = nullptr;
        REQUIRE(i2c_master_bus_add_device(bus_handle, &dev, &dev_handle) == ESP_OK);

        constexpr uint8_t kToken = 0x3C;
        constexpr size_t kLen = 8;

        // Single rendezvous up front. After it the two boards only talk over
        // I2C until the final rendezvous below: any control-link traffic here
        // would delay the slave task past the address-match stretch of its
        // read, which then self-releases and makes the master clock out stale
        // TX FIFO bytes (see report.md).
        CHECK(lead_send(12, kToken));
        uint8_t ready = 0xFF;
        CHECK(lead_wait(12, ready));
        CHECK(ready == 0);

        // Phase 1: the master writes a pattern to the slave. Phase 2: the
        // slave answers the very next read with a pattern derived from the
        // token, so there is no control-link handshake in between.
        std::array<uint8_t, kLen> tx{};
        fill_pattern(tx.data(), tx.size(), kToken);
        CHECK(i2c_master_transmit(dev_handle, tx.data(), tx.size(), 1000) == ESP_OK);

        std::array<uint8_t, kLen> rx{};
        CHECK(i2c_master_receive(dev_handle, rx.data(), rx.size(), 1000) == ESP_OK);
        std::array<uint8_t, kLen> expect{};
        fill_pattern(expect.data(), expect.size(), static_cast<uint32_t>(kToken) ^ 0xA5u);
        CHECK(std::memcmp(rx.data(), expect.data(), rx.size()) == 0);

        // Final rendezvous: exchange the CRC each board computed for the bytes
        // it sent or received, so both agree on the exchange.
        CHECK(lead_send(13, crc8(tx.data(), tx.size())));
        uint8_t slave_crc = 0;
        CHECK(lead_wait(13, slave_crc));
        CHECK(slave_crc == crc8(tx.data(), tx.size()));

        REQUIRE(i2c_master_bus_rm_device(dev_handle) == ESP_OK);
        REQUIRE(i2c_del_master_bus(bus_handle) == ESP_OK);
    }
}
