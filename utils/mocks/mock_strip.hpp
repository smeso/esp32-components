/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mock_bus {

// A software LED strip that records every update, so LED-effect tests can
// verify the exact sequence of frames without any hardware. The effects only
// require num_leds(), an RGB color type, set_leds(), set_all_leds() and
// all_off(), so this mock implements exactly that interface and nothing else.
template<std::size_t N>
class mock_strip
{
  public:
    struct RGB
    {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;

        friend bool operator==(const RGB &a, const RGB &b)
        {
            return a.r == b.r && a.g == b.g && a.b == b.b;
        }
    };

    using colors_t = std::array<RGB, N>;

    static constexpr std::size_t num_leds()
    {
        return N;
    }

    void set_leds(std::span<const RGB> leds)
    {
        colors_t colors{};
        std::copy(leds.begin(), leds.end(), colors.begin());
        history_.push_back(colors);
    }

    void set_all_leds(const RGB &color)
    {
        colors_t leds{};
        leds.fill(color);
        history_.push_back(leds);
    }

    void all_off()
    {
        history_.push_back(colors_t{});
    }

    // Test helpers ---------------------------------------------------------

    void reset()
    {
        history_.clear();
    }

    [[nodiscard]] std::size_t frame_count() const
    {
        return history_.size();
    }

    [[nodiscard]] const colors_t &frame(std::size_t i) const
    {
        return history_[i];
    }

  private:
    std::vector<colors_t> history_;
};

}   // namespace mock_bus
