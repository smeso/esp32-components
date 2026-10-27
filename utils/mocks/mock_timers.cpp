/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mock_timers.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/timers.h>

namespace mock_bus {

namespace {

// Number of xTimerCreate() calls to let through before the next one fails.
// 0 = fail the very next call; SIZE_MAX = never fail (the default).
unsigned fail_x_timer_create_after = ~0U;
bool fail_x_timer_start = false;
bool fail_x_timer_stop = false;
bool fail_x_task_create = false;
// Number of xQueueGenericCreate() calls to let through before the next one
// fails. 0 = fail the very next call; SIZE_MAX = never fail (the default).
unsigned fail_x_queue_create_after = ~0U;
// Number of tmrCOMMAND_DELETE commands enqueued since the last reset.
unsigned delete_count = 0;

}   // namespace

void
mock_timers_reset()
{
    fail_x_timer_create_after = ~0U;
    fail_x_timer_start = false;
    fail_x_timer_stop = false;
    fail_x_task_create = false;
    fail_x_queue_create_after = ~0U;
    delete_count = 0;
}

void
mock_timers_fail_x_timer_create_after(unsigned calls)
{
    fail_x_timer_create_after = calls;
}

void
mock_timers_fail_x_timer_start(bool fail)
{
    fail_x_timer_start = fail;
}

void
mock_timers_fail_x_timer_stop(bool fail)
{
    fail_x_timer_stop = fail;
}

void
mock_timers_fail_x_task_create(bool fail)
{
    fail_x_task_create = fail;
}

void
mock_timers_fail_x_queue_create(unsigned calls)
{
    fail_x_queue_create_after = calls;
}

unsigned
mock_timers_delete_count()
{
    return delete_count;
}

}   // namespace mock_bus

extern "C" {

TimerHandle_t __real_xTimerCreate(const char *name,
                                  TickType_t period,
                                  BaseType_t reload,
                                  void *id,
                                  TimerCallbackFunction_t cb);

BaseType_t __real_xTimerGenericCommand(TimerHandle_t timer,
                                       BaseType_t command,
                                       TickType_t value,
                                       BaseType_t *woken,
                                       TickType_t wait);

BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t task_code,
                                          const char *name,
                                          const uint32_t depth,
                                          void *param,
                                          UBaseType_t priority,
                                          TaskHandle_t *handle,
                                          const BaseType_t core_id);

QueueHandle_t __real_xQueueGenericCreate(const UBaseType_t queue_length,
                                         const UBaseType_t item_size,
                                         const uint8_t queue_type);

// The FreeRTOS linux port's software timers are driven by the POSIX
// simulator, so tests can only force xTimerCreate()/xTimerStart() to fail by
// redirecting them here. The --wrap options in this component's CMakeLists.txt
// apply to the linux target only; device builds use the real kernel functions.
TimerHandle_t
__wrap_xTimerCreate(const char *name,
                    TickType_t period,
                    BaseType_t reload,
                    void *id,
                    TimerCallbackFunction_t cb)
{
    if (mock_bus::fail_x_timer_create_after != ~0U) {
        if (mock_bus::fail_x_timer_create_after == 0) {
            mock_bus::fail_x_timer_create_after = ~0U;
            return nullptr;
        }
        --mock_bus::fail_x_timer_create_after;
    }
    return __real_xTimerCreate(name, period, reload, id, cb);
}

BaseType_t
__wrap_xTimerGenericCommand(TimerHandle_t timer,
                            BaseType_t command,
                            TickType_t value,
                            BaseType_t *woken,
                            TickType_t wait)
{
    if (mock_bus::fail_x_timer_start && command == tmrCOMMAND_START)
        return pdFAIL;
    if (mock_bus::fail_x_timer_stop && command == tmrCOMMAND_STOP)
        return pdFAIL;
    if (command == tmrCOMMAND_DELETE)
        ++mock_bus::delete_count;
    return __real_xTimerGenericCommand(timer, command, value, woken, wait);
}

BaseType_t
__wrap_xTaskCreatePinnedToCore(TaskFunction_t task_code,
                               const char *name,
                               const uint32_t depth,
                               void *param,
                               UBaseType_t priority,
                               TaskHandle_t *handle,
                               const BaseType_t core_id)
{
    if (mock_bus::fail_x_task_create)
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    return __real_xTaskCreatePinnedToCore(
            task_code, name, depth, param, priority, handle, core_id);
}

QueueHandle_t
__wrap_xQueueGenericCreate(const UBaseType_t queue_length,
                           const UBaseType_t item_size,
                           const uint8_t queue_type)
{
    if (mock_bus::fail_x_queue_create_after != ~0U) {
        if (mock_bus::fail_x_queue_create_after == 0) {
            mock_bus::fail_x_queue_create_after = ~0U;
            return nullptr;
        }
        --mock_bus::fail_x_queue_create_after;
    }
    return __real_xQueueGenericCreate(queue_length, item_size, queue_type);
}

}   // extern "C"
