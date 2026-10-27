/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal cpptaskcreate example: three tasks are started from a single lambda
 * factory, each counting at its own pace. The main task waits for all of them
 * to report their final count and verifies the totals, proving that the
 * callables really run on their own FreeRTOS tasks (with state handed over
 * through a move-only counter object).
 */

#include <atomic>
#include <memory>
#include <string>

#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "cpptaskcreate.hpp"

namespace {

constexpr const char *TAG = "counter_task";

constexpr UBaseType_t worker_priority = 5;
constexpr size_t workers = 3;
constexpr int steps = 5;

// A move-only counter: the shared_ptr below carries it across tasks without
// copying, and the lambda's copyable shared_ptr capture is what std::function
// requires (the component itself still moves the closure onto the heap).
class counter
{
  public:
    counter(std::string name, std::atomic<int> &total, std::atomic<size_t> &done)
            : name_(std::move(name))
            , total_(total)
            , done_(done)
    {
    }
    counter(counter &&) = default;
    counter &operator=(counter &&) = delete;

    void operator()()
    {
        for (int i = 1; i <= steps; ++i) {
            total_.fetch_add(1);
            ESP_LOGI(TAG, "task '%s': count %d of %d", pcTaskGetName(nullptr), i, steps);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        done_.fetch_add(1);
    }

  private:
    std::string name_;
    std::atomic<int> &total_;
    std::atomic<size_t> &done_;
};

}   // namespace

extern "C" void
app_main(void)
{
    std::atomic<int> total{ 0 };
    std::atomic<size_t> done{ 0 };

    for (size_t i = 0; i < workers; ++i) {
        std::string name = "counter_" + std::to_string(i);
        // The counter is held by a std::shared_ptr: the lambda's capture
        // must be copyable to travel through std::function<void()>.
        auto c = std::make_shared<counter>(name, total, done);
        // A task must end explicitly: returning from the callable would make
        // the trampoline log an error and abort the application. Ending the
        // task with vTaskDelete(NULL) releases the callable's context on every
        // target.
        const BaseType_t ok = cppTaskCreate(name.c_str(), 4096, worker_priority, nullptr, [c] {
            (*c)();
            vTaskDelete(nullptr);
        });
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "failed to create task '%s'", name.c_str());
            std::abort();
        }
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    while (done.load() < workers && xTaskGetTickCount() < deadline)
        vTaskDelay(pdMS_TO_TICKS(10));

    const int expected = static_cast<int>(workers) * steps;
    ESP_LOGI(TAG,
             "counted %d of %d increments across %u workers",
             total.load(),
             expected,
             static_cast<unsigned>(workers));
    if (total.load() != expected) {
        ESP_LOGE(TAG, "timed out waiting for the worker tasks");
        std::abort();
    }

    ESP_LOGI(TAG, "done");
}
