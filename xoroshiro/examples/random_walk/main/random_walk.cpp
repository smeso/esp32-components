/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal xoroshiro example: a two-dimensional Gaussian random walk. Each
 * step is drawn with std::normal_distribution fed by xoroshiro128pp,
 * which only needs the UniformRandomBitGenerator interface the generator
 * provides. The drift of the walk over a fixed number of steps is reported
 * to show that the generator produces a reasonable sequence.
 *
 * The example only uses the xoroshiro component itself: the RNG is seeded
 * from the target's hardware random number generator (esp_random, provided
 * by the common `esp_hw_support` dependency that every component gets), so
 * no other component is required.
 */

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <random>

#include <esp_log.h>
#include <esp_random.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "xoroshiro.hpp"

namespace {

constexpr const char *TAG = "random_walk";

constexpr size_t steps = 1U << 16;
constexpr double step_sigma = 1.0;

}   // namespace

extern "C" void
app_main(void)
{
    xoroshiro128pp rng(esp_random(), esp_random());
    std::normal_distribution<double> step(0.0, step_sigma);

    double x = 0.0;
    double y = 0.0;
    for (size_t i = 0; i < steps; ++i) {
        x += step(rng);
        y += step(rng);
    }

    const double distance = sqrt(x * x + y * y);
    ESP_LOGI(TAG,
             "random walk: %llu steps, landed at (%.2f, %.2f), "
             "distance %.2f (expected ~256)",
             static_cast<unsigned long long>(steps),
             x,
             y,
             distance);
    // With `steps` unit-variance steps the expected displacement is of the
    // order of sqrt(steps) = 256, so an outcome far above that would point
    // at a broken generator or a broken distribution integration.
    if (distance > 20.0 * step_sigma * sqrt(static_cast<double>(steps))) {
        ESP_LOGE(TAG, "implausible displacement, the RNG is not working");
        std::abort();
    }
    ESP_LOGI(TAG, "done");
}
