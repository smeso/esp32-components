/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <driver/gpio.h>

#include "ws2812.hpp"

namespace multidevice {

// The two boards of the rig carry a single WS2812 on GPIO 38 used to give the
// operator run-level feedback: blue while booting, yellow while the suite is
// running, green when it completed successfully and red when it failed.
enum class status
{
    booting,
    running,
    ok,
    failed,
};

// Pin 38 is the on-board status LED on both boards of the rig.
inline constexpr gpio_num_t kStatusPin = GPIO_NUM_38;

inline void
set_status(status value)
{
    // The strip is created lazily on the first call and kept alive for the
    // whole run so the RMT channel is not torn down between status updates.
    static ws2812<1> led(kStatusPin);

    switch (value) {
    case status::booting:
        led.set_all_leds({ 0, 0, 255 });   // blue
        break;
    case status::running:
        led.set_all_leds({ 255, 255, 0 });   // yellow
        break;
    case status::ok:
        led.set_all_leds({ 0, 255, 0 });   // green
        break;
    case status::failed:
        led.set_all_leds({ 255, 0, 0 });   // red
        break;
    }
}

}   // namespace multidevice
