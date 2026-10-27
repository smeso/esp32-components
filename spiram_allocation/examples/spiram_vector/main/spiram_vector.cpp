/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal spiram_allocation example: builds a histogram of a pseudo-random
 * data stream inside a spiram_vector (whose storage always lives in SPI RAM),
 * checks that the data is really in external memory and prints a short
 * summary proving it.
 *
 * The SPI RAM heap is enabled through sdkconfig.defaults (CONFIG_SPIRAM=y
 * and CONFIG_SPIRAM_USE_CAPS_ALLOC=y). On targets or setups without SPI RAM
 * (e.g. QEMU started without the -m option) the allocator fails cleanly and
 * the example prints a warning instead of panicking.
 */

#include <cstdint>
#include <cstdlib>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "spiram_allocation.hpp"

namespace {

constexpr const char *TAG = "spiram_vector";

// Linear congruential generator, kept small so the example does not depend on
// other components.
uint32_t
next_random(uint32_t &state)
{
    state = state * 1664525U + 1013904223U;
    return state;
}

}   // namespace

extern "C" void
app_main(void)
{
    constexpr size_t bins = 16;
    constexpr size_t samples = 1U << 20;
    constexpr uint32_t seed = 0x12345678U;

    // heap_caps_get_free_size() is callable even when no SPI RAM is present,
    // unlike esp_psram_is_initialized() which is not linked on targets
    // without CONFIG_SPIRAM support.
    const size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "free SPI RAM: %u bytes", static_cast<unsigned>(spiram_free));
    if (spiram_free == 0) {
        ESP_LOGW(TAG, "no SPI RAM available, skipping the demo");
        return;
    }

    spiram_vector<uint32_t> histogram(bins, 0);
    if (histogram.data() == nullptr) {
        // Cannot happen: a default-constructed vector never allocates.
        ESP_LOGE(TAG, "failed to allocate histogram");
        return;
    }
    ESP_LOGI(TAG,
             "histogram buffer at %p (in external RAM: %s)",
             static_cast<void *>(histogram.data()),
             esp_ptr_external_ram(histogram.data()) ? "yes" : "no");

    uint32_t state = seed;
    for (size_t i = 0; i < samples; ++i) {
        const uint32_t value = next_random(state);
        histogram[static_cast<size_t>(value >> (32U - 4U))]++;
    }

    uint64_t sum = 0;
    for (size_t i = 0; i < bins; ++i)
        sum += histogram[i];
    ESP_LOGI(TAG,
             "histogram of %llu samples across %u bins, %s in external RAM",
             static_cast<unsigned long long>(sum),
             static_cast<unsigned>(bins),
             esp_ptr_external_ram(histogram.data()) ? "all stored" : "not stored");
    ESP_LOGI(TAG, "done");
}
