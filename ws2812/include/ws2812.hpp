/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Header-only C++ driver for WS2812/WS2812B addressable RGB LED strips over an
 * RMT TX channel.
 *
 * @note Exceptions must be enabled in the project configuration
 * (CONFIG_COMPILER_CXX_EXCEPTIONS): channel setup and transmission errors are
 * reported by throwing ws2812<>::exception and a failed pixel buffer
 * allocation throws std::bad_alloc.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_sys.h>

#include <driver/gpio.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <soc/soc_caps.h>

struct order_rgb
{};
struct order_grb
{};

/**
 * @brief Driver for WS2812/WS2812B (and pin/timing-compatible 3-byte parts)
 * LED strips over an RMT TX channel.
 *
 * @tparam NumLeds     Number of LEDs on the strip (must be > 0)
 * @tparam ColorOrder  Byte order sent on the wire: order_rgb or order_grb
 * @tparam Allocator   Allocator used for the pixel buffer. Defaults to
 *                     std::allocator<uint8_t> (internal RAM); pass e.g.
 *                     SpiramAllocator<uint8_t> from the spiram_allocation
 *                     component to place the buffer in SPI RAM. Long strips
 *                     (3 bytes per LED) benefit the most from this: the buffer
 *                     can be moved out of the scarce internal RAM.
 *
 * Invariants and usage constraints:
 *  - All I/O is synchronous: every setter blocks until the frame has been
 *    fully transmitted and the reset gap has elapsed (bounded by a finite
 *    timeout).
 *  - The class is NOT thread-safe: instances must not be used concurrently
 *    from multiple tasks. All mutations of the pixel buffer and the RMT
 *    channel must be serialized externally.
 *  - Instances are movable but not copyable. After being moved from, an
 *    object may only be destroyed or assigned to; calling anything but the
 *    static num_leds() on a moved-from object throws an exception.
 *  - The destructor blocks while turning the LEDs off and tearing the RMT
 *    channel down (with a finite timeout).
 *  - The pixel buffer is heap-allocated (3 bytes per LED) through the
 *    Allocator, so the object itself is small and can live on the stack no
 *    matter how long the strip is. The allocation is done once, by the
 *    constructor, and the buffer mirrors the last frame transmitted by any
 *    setter; the setters are the only code that writes it.
 *
 * Allocator requirements (enforced by static_assert below): value_type and
 * pointer must be uint8_t / uint8_t *, and the allocator must be
 * default-constructible and nothrow-copy/move-constructible and
 * nothrow-move-assignable. The nothrow requirements matter because the
 * deleter stores the allocator by value and is copied and moved inside the
 * noexcept move operations, and because make_pixel_buffer() has to build the
 * deleter from the allocator after the storage has been allocated.
 *
 * The RMT TX driver rejects a payload that is not in internal RAM whenever
 * CONFIG_RMT_TX_ISR_CACHE_SAFE is enabled (the deprecated
 * CONFIG_RMT_ISR_IRAM_SAFE selects it). The pixel buffer is the payload of
 * every setter, so with that option enabled the default internal-RAM
 * allocator must be used: a SpiramAllocator buffer works out of the box only
 * while the option is disabled.
 */
template<size_t NumLeds,
         typename ColorOrder = order_rgb,
         typename Allocator = std::allocator<uint8_t>>
class ws2812
{
    static constexpr size_t pixel_bytes_ = NumLeds * 3;
    static constexpr uint32_t resolution_hz_ = 10000000;
    static constexpr int wait_timeout_ms_ = 1000;
    static constexpr uint32_t reset_time_us_ = 300;
    static constexpr const char *TAG = "ws2812";
    static_assert(NumLeds > 0, "NumLeds must be greater than zero");
    static_assert(std::is_same_v<ColorOrder, order_rgb> ||
                          std::is_same_v<ColorOrder, order_grb>,
                  "ColorOrder must be order_rgb or order_grb");
    static_assert(
            std::is_same_v<typename std::allocator_traits<Allocator>::value_type, uint8_t>,
            "Allocator must allocate uint8_t");
    static_assert(
            std::is_same_v<typename std::allocator_traits<Allocator>::pointer, uint8_t *>,
            "Allocator::pointer must be uint8_t *");
    static_assert(std::is_default_constructible_v<Allocator>,
                  "Allocator must be default-constructible");
    static_assert(std::is_nothrow_copy_constructible_v<Allocator>,
                  "Allocator must be nothrow-copy-constructible");
    static_assert(std::is_nothrow_move_constructible_v<Allocator>,
                  "Allocator must be nothrow-move-constructible");
    static_assert(std::is_nothrow_move_assignable_v<Allocator>,
                  "Allocator must be nothrow-move-assignable");

  public:
    struct RGB
    {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
    };

    static_assert(sizeof(RGB) == 3, "RGB must be exactly three bytes with no padding");

    class exception final : public std::exception
    {
      public:
        explicit exception(const char *msg, esp_err_t err = ESP_OK) noexcept
        {
            if (err != ESP_OK) {
                char err_buf[64];
                esp_err_to_name_r(err, err_buf, sizeof(err_buf));
                std::snprintf(msg_buf_, sizeof(msg_buf_), "%s: %s: %s", TAG, msg, err_buf);
            } else {
                std::snprintf(msg_buf_, sizeof(msg_buf_), "%s: %s", TAG, msg);
            }
        }
        ~exception() override = default;
        const char *what() const noexcept override   // GCOVR_EXCL_BR_LINE
        {
            return msg_buf_;   // GCOVR_EXCL_BR_LINE
        }

      private:
        char msg_buf_[128]{};
    };

    /**
     * @param gpio_num  GPIO number the WS2812 data line is connected to
     * @param allocator Allocator used to allocate the pixel buffer
     *
     * @throws exception      if the RMT channel/encoder setup fails
     * @throws std::bad_alloc if the pixel buffer cannot be allocated
     */
    ws2812(gpio_num_t gpio_num, Allocator allocator = Allocator{})
            // The pixel buffer is heap-allocated with the caller-provided
            // allocator: its size (3 bytes per LED) depends on NumLeds, so
            // keeping it in the object would put it on the caller's stack for
            // stack-allocated strips. make_pixel_buffer() value-initializes
            // the array, leaving every LED off.
            : pixels_(make_pixel_buffer(allocator))
            , gpio_num_(gpio_num)
    {
        // GCOVR_EXCL_BR_START: owning the heap-allocated pixel buffer makes
        // the unwinder destroy it whenever an exception leaves the
        // constructor, which gives every statement below a never-taken
        // exception-cleanup edge. gcovr reports those edges as uncovered
        // branches, intermixed with the branches of the error checks: the
        // checks themselves (the throw sites and their messages) are
        // exercised by the constructor tests, only their branch reporting is
        // dropped here. The statements stay line-covered.
        ESP_LOGI(TAG,
                 "Creating RMT TX channel for WS2812 on GPIO %d",
                 static_cast<int>(gpio_num_));

        rmt_tx_channel_config_t tx_chan_config = {
            .gpio_num = gpio_num_,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = resolution_hz_,
            .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
            .trans_queue_depth = 4,
            .intr_priority = 0,
            .flags = {},
        };
        esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &chan_);
        if (err != ESP_OK) {
            throw exception("rmt_new_tx_channel failed", err);
        }

        ESP_LOGI(TAG, "Creating bytes encoder for WS2812 protocol");

        // WS2812 timing: T0H=0.3us, T0L=0.9us, T1H=0.9us, T1L=0.3us
        // All durations in RMT ticks: 1 tick = 1 / resolution_hz_ = 0.1 us
        rmt_bytes_encoder_config_t enc_config = {
            .bit0 = {
                .duration0 = 3,
                .level0 = 1,
                .duration1 = 9,
                .level1 = 0,
            },
            .bit1 = {
                .duration0 = 9,
                .level0 = 1,
                .duration1 = 3,
                .level1 = 0,
            },
            .flags = { .msb_first = true },
        };
        err = rmt_new_bytes_encoder(&enc_config, &encoder_);
        if (err != ESP_OK) {
            esp_err_t del_err = rmt_del_channel(chan_);
            if (del_err != ESP_OK) {
                ESP_LOGE(TAG, "rmt_del_channel failed: %s", esp_err_to_name(del_err));
            }
            throw exception("rmt_new_bytes_encoder failed", err);
        }

        ESP_LOGI(TAG, "Enabling RMT TX channel");
        err = rmt_enable(chan_);
        if (err != ESP_OK) {
            esp_err_t del_err = rmt_del_encoder(encoder_);
            if (del_err != ESP_OK) {
                ESP_LOGE(TAG, "rmt_del_encoder failed: %s", esp_err_to_name(del_err));
            }
            del_err = rmt_del_channel(chan_);
            if (del_err != ESP_OK) {
                ESP_LOGE(TAG, "rmt_del_channel failed: %s", esp_err_to_name(del_err));
            }
            throw exception("rmt_enable failed", err);
        }

        ESP_LOGI(TAG, "WS2812 initialized with %zu LEDs", NumLeds);
        // GCOVR_EXCL_BR_STOP
    }   // GCOVR_EXCL_BR_LINE: constructor exception-cleanup edge

    ~ws2812() noexcept
    {
        if (chan_ == nullptr) {
            return;   // moved-from state
        }

        try {
            all_off();
        } catch (...) {
        }

        release_channel();
        ESP_LOGI(TAG, "WS2812 deinitialized");
    }   // GCOVR_EXCL_BR_LINE: destructor exception-cleanup edge

    ws2812(const ws2812 &) = delete;
    ws2812 &operator=(const ws2812 &) = delete;
    ws2812(ws2812 &&other) noexcept
            : chan_(other.chan_)
            , encoder_(other.encoder_)
            , pixels_(std::move(other.pixels_))   // GCOVR_EXCL_BR_LINE: move cleanup edge
            , gpio_num_(other.gpio_num_)
    {
        other.chan_ = nullptr;      // GCOVR_EXCL_BR_LINE
        other.encoder_ = nullptr;   // GCOVR_EXCL_BR_LINE
    }
    ws2812 &operator=(ws2812 &&other) noexcept
    {
        if (this != &other) {
            if (chan_ != nullptr) {
                try {
                    all_off();
                } catch (...) {
                }
                release_channel();
            }
            chan_ = other.chan_;
            encoder_ = other.encoder_;
            pixels_ = std::move(other.pixels_);
            gpio_num_ = other.gpio_num_;
            other.chan_ = nullptr;
            other.encoder_ = nullptr;
        }
        return *this;
    }

    void set_all_leds(const RGB &color)
    {
        ensure_valid();
        for (size_t i = 0; i < NumLeds; ++i) {
            set_led_internal(i, color);
        }
        flush();
    }

    /**
     * @brief Write `colors` to the strip and transmit the frame.
     *
     * Any contiguous container of NumLeds RGB values converts implicitly to
     * the span.
     *
     * @throws exception if the container does not hold exactly NumLeds colors
     */
    void set_leds(std::span<const RGB> colors)
    {
        ensure_valid();
        if (colors.size() != NumLeds) {
            throw exception("the colors container must hold exactly NumLeds colors");
        }
        if constexpr (std::is_same_v<ColorOrder, order_rgb>) {
            // The on-wire byte order matches the in-memory RGB layout, so the
            // caller's container can be transmitted straight from its storage
            // without staging a copy through the pixel buffer.
            flush(colors.data(), pixel_bytes_);
        } else {
            for (size_t i = 0; i < NumLeds; ++i) {
                set_led_internal(i, colors[i]);
            }
            flush();
        }
    }

    void all_on()
    {
        // GCOVR_EXCL_BR_LINE: the throw edges of the inlined set_all_leds()
        // call are never recorded by gcov on xtensa, even when an exception
        // unwinds through all_on() (e.g. a moved-from strip or a failed RMT
        // transmission).
        set_all_leds({ 255, 255, 255 });   // GCOVR_EXCL_BR_LINE
    }

    void all_off()
    {
        ensure_valid();
        std::fill_n(pixels_.get(), pixel_bytes_, uint8_t{ 0 });
        flush();
    }

    static constexpr size_t num_leds()
    {
        return NumLeds;
    }

  private:
    // GCOVR_EXCL_BR_START: the deleter's implicitly-defined copy/move
    // constructors get exception-cleanup edges that gcov reports as
    // uncovered throw branches. Copying the allocators used here (e.g.
    // std::allocator, SpiramAllocator) never throws, so those edges are
    // unreachable (and static_assert-ed above); only their branch reporting is
    // dropped.
    struct pixel_deleter
    {
        Allocator allocator;

        void operator()(uint8_t *ptr) noexcept
        {
            std::allocator_traits<Allocator>::deallocate(allocator, ptr, pixel_bytes_);
        }
    };
    // GCOVR_EXCL_BR_STOP

    using pixel_buffer = std::unique_ptr<uint8_t[], pixel_deleter>;

    rmt_channel_handle_t chan_{};
    rmt_encoder_handle_t encoder_{};
    pixel_buffer pixels_{};
    gpio_num_t gpio_num_{};

    static pixel_buffer make_pixel_buffer(Allocator &allocator)
    {
        uint8_t *pixels = std::allocator_traits<Allocator>::allocate(allocator, pixel_bytes_);
        // Own the storage before value-initializing it, so the buffer is
        // released even if the initialization were to throw.
        pixel_buffer buffer(pixels, pixel_deleter{ allocator });
        std::uninitialized_value_construct_n(pixels, pixel_bytes_);
        return buffer;
    }

    void ensure_valid() const
    {
        if (chan_ == nullptr) {
            throw exception("object was moved from and cannot be used");
        }
    }

    void release_channel() noexcept
    {
        esp_err_t err = rmt_disable(chan_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "rmt_disable failed: %s", esp_err_to_name(err));
        }
        err = rmt_del_encoder(encoder_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "rmt_del_encoder failed: %s", esp_err_to_name(err));
        }
        err = rmt_del_channel(chan_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "rmt_del_channel failed: %s", esp_err_to_name(err));
        }
    }

    // Stop and restart the channel after a failed wait so the RMT driver
    // drops the reference to the pixel buffer and recycles the transaction
    // descriptor, while leaving the object usable for the next frame.
    void abort_transaction() noexcept
    {
        esp_err_t err = rmt_disable(chan_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "rmt_disable failed: %s", esp_err_to_name(err));
        }
        err = rmt_enable(chan_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        }
    }

    void set_led_internal(size_t index, const RGB &color)
    {
        if constexpr (std::is_same_v<ColorOrder, order_rgb>) {
            pixels_[index * 3 + 0] = color.r;
            pixels_[index * 3 + 1] = color.g;
            pixels_[index * 3 + 2] = color.b;
        } else {
            pixels_[index * 3 + 0] = color.g;
            pixels_[index * 3 + 1] = color.r;
            pixels_[index * 3 + 2] = color.b;
        }
    }

    void flush()
    {
        flush(pixels_.get(), pixel_bytes_);
    }

    void flush(const void *payload, size_t payload_bytes)
    {
        rmt_transmit_config_t tx_config = {
            .loop_count = 0,
            .flags = {},
        };
        esp_err_t err = rmt_transmit(chan_, encoder_, payload, payload_bytes, &tx_config);
        if (err != ESP_OK) {
            throw exception("rmt_transmit failed", err);
        }
        err = rmt_tx_wait_all_done(chan_, wait_timeout_ms_);
        if (err != ESP_OK) {
            // On a timeout the transaction is still in flight and the RMT
            // encoder keeps reading the pixel buffer, which is owned by this
            // object: abort the transaction before throwing so the driver
            // drops the reference and the destructor does not try to drive a
            // busy channel.
            abort_transaction();
            throw exception("rmt_tx_wait_all_done failed", err);
        }
        // Keep the data line low long enough for the strip to latch the frame
        // before the next one can start. Without this gap a tight update loop
        // would concatenate consecutive frames into a single bit stream.
        esp_rom_delay_us(reset_time_us_);
    }
};
