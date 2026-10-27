/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mock_hooks.hpp"

#include <cstdlib>

#include <esp_err.h>

#include <freertos/FreeRTOS.h>

// The real abort() behind the --wrap redirect (only linked when a test
// component requests --wrap=abort).
extern "C" [[noreturn]] void __real_abort(void);

namespace mock_bus {

namespace {

constexpr int MAX_HOOKS = 8;
void (*tick_cb[MAX_HOOKS])(void) = {};
void (*abort_hook)(void) = nullptr;

// The FreeRTOS linux port (port_idf.c) keeps uxInterruptNesting at 0 and
// never reports ISR context, so the tick-hook dispatch fakes it: while the
// registered callbacks run (inside the SIGALRM tick handler) the wrapped
// xPortCheckIfInISR() reports an ISR.
bool in_tick_isr = false;

}   // namespace

void
mock_hooks_set_in_isr(bool in_isr)
{
    in_tick_isr = in_isr;
}

void
mock_hooks_set_abort_hook(void (*hook)(void))
{
    abort_hook = hook;
}

const void *
mock_hooks_abort_hook()
{
    return reinterpret_cast<const void *>(abort_hook);
}

void
mock_hooks_reset()
{
    for (auto &cb : tick_cb)
        cb = nullptr;
    abort_hook = nullptr;
}

bool
mock_hooks_register_tick_cb(void (*cb)(void))
{
    if (cb == nullptr)
        return false;
    for (auto &slot : tick_cb) {
        if (slot == cb)
            return true;
        if (slot == nullptr) {
            slot = cb;
            return true;
        }
    }
    return false;
}

void
mock_hooks_deregister_tick_cb(void (*cb)(void))
{
    for (auto &slot : tick_cb) {
        if (slot == cb)
            slot = nullptr;
    }
}

}   // namespace mock_bus

extern "C" {

// The FreeRTOS linux port (port_idf.c) defines an empty esp_vApplicationTickHook()
// stub, so instead of overriding it we wrap vApplicationTickHook(): tasks.c
// calls it from xTaskIncrementTick() whenever CONFIG_FREERTOS_USE_TICK_HOOK is
// enabled, and --wrap redirects that (cross-object) call here. The registered
// callbacks run inside the tick signal handler, so xPortInIsrContext() reports
// true (via the wrapped xPortCheckIfInISR()) and the task_queue ISR test can
// exercise its FromISR path. On device builds the real esp_system tick-hook
// machinery is used and this wrapper is never referenced.
void
__wrap_vApplicationTickHook(void)
{
    mock_bus::in_tick_isr = true;
    for (auto &cb : mock_bus::tick_cb) {
        if (cb != nullptr)
            cb();
    }
    mock_bus::in_tick_isr = false;
}

// The FreeRTOS linux port never sets uxInterruptNesting (the non-SMP port
// tracks uxCriticalNesting instead), so xPortInIsrContext() would always
// report task context. The --wrap options in this component's CMakeLists.txt
// redirect it here so the tick-hook dispatch above can report ISR context.
BaseType_t
__wrap_xPortCheckIfInISR(void)
{
    return mock_bus::in_tick_isr ? pdTRUE : pdFALSE;
}

// The real esp_register/deregister_freertos_tick_hook() live in esp_system's
// freertos_hooks.c, which is not built for the linux target; the --wrap
// options in this component's CMakeLists.txt redirect the test's calls to
// these implementations.
esp_err_t
__wrap_esp_register_freertos_tick_hook(void (*cb)(void))
{
    return mock_bus::mock_hooks_register_tick_cb(cb) ? ESP_OK : ESP_ERR_NO_MEM;
}

void
__wrap_esp_deregister_freertos_tick_hook(void (*cb)(void))
{
    mock_bus::mock_hooks_deregister_tick_cb(cb);
}

// --wrap=abort redirect (requested by test components that need to survive
// abort() calls made by the firmware under test, e.g. cpptaskcreate's
// task_entry()). The hook usually parks the aborting task (vTaskSuspend(NULL)), so
// execution never comes back here and the real abort is not reached. Without
// a hook installed we fall back to the real abort(), so every other test
// firmware behaves exactly as before.
void
__wrap_abort(void)
{
    if (mock_bus::abort_hook != nullptr) {
        mock_bus::abort_hook();
    }
    __real_abort();
}

}   // extern "C"
