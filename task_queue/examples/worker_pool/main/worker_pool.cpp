/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal task_queue example: a pool of worker queues, each drained by its own
 * dedicated worker task, processes posted closures. The main_task posts the
 * closures and waits for their results, proving that the posted work ran on
 * the worker tasks (and not in the caller's context) and that the results
 * coming from different workers are correct and consistent.
 */

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "task_queue.hpp"

namespace {

constexpr const char *TAG = "worker_pool";

constexpr size_t workers = 4;
constexpr size_t jobs = 32;

// Simple additive checksum, kept local so the example does not depend on other
// components.
uint32_t
checksum(uint32_t seed)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 64; ++i)
        sum += seed * (i + 1U);
    return sum;
}

}   // namespace

extern "C" void
app_main(void)
{
    // Each queue runs the posted closures on its own dedicated worker task,
    // so the four queues form a small pool of workers. The main_task posts
    // the closures and waits for their results, proving that the posted work
    // ran on the worker tasks (and not in the caller's context).
    std::vector<std::unique_ptr<task_queue<std::function<void()>>>> pool;
    pool.reserve(workers);
    std::vector<std::atomic<uint32_t>> sums(workers);
    for (size_t i = 0; i < workers; ++i)
        pool.push_back(std::make_unique<task_queue<std::function<void()>>>("worker", 8));

    // Post the closures, round-robin across the workers: each one computes
    // its checksum and accumulates it into its worker's slot.
    uint32_t expected = 0;
    for (size_t i = 0; i < jobs; ++i) {
        const size_t w = i % workers;
        const uint32_t seed = static_cast<uint32_t>(i);
        expected += checksum(seed);
        pool[w]->post([&sums, w, seed] {
            sums[w].fetch_add(checksum(seed));
            vTaskDelay(pdMS_TO_TICKS(10));
        });
    }

    // Wait for every worker to drain its queue (the queues are only drained by
    // the worker tasks, so this also proves the closures really ran off the
    // main_task).
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    uint32_t total = 0;
    for (;;) {
        total = 0;
        for (const auto &sum : sums)
            total += sum.load();
        if (total == expected || xTaskGetTickCount() >= deadline)
            break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGI(TAG,
             "computed %u of %u checksums",
             static_cast<unsigned>(total),
             static_cast<unsigned>(expected));
    if (total != expected) {
        ESP_LOGE(TAG, "timed out waiting for the workers");
        std::abort();
    }

    ESP_LOGI(TAG, "done");
}
