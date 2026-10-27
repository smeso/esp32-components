/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

namespace mock_bus {

// Reset the tick-hook registry to its initial (empty) state.
void mock_hooks_reset();

// Register a callback to be invoked from the next FreeRTOS tick. Returns true
// on success, false when the registry is full or the callback is already
// registered.
bool mock_hooks_register_tick_cb(void (*cb)(void));

// Remove a previously registered callback. No-op when it is not registered.
void mock_hooks_deregister_tick_cb(void (*cb)(void));

// Force xPortInIsrContext() to report the given state. The FreeRTOS linux port
// never tracks interrupt nesting, so tests that need to exercise ISR-context
// code paths (e.g. task_queue::post() from a simulated ISR) toggle this.
void mock_hooks_set_in_isr(bool in_isr);

// Install a callback that is invoked whenever abort() is called by the
// firmware under test. This only works for test firmwares whose test
// component requests the --wrap=abort linker redirect (e.g. cpptaskcreate);
// without it the wrapper is not linked in and the hook is never consulted.
// The hook typically parks the aborting task (vTaskSuspend(NULL) from there
// makes the abort non-fatal for the rest of the suite; vTaskDelete(NULL)
// is NOT safe from inside the abort machinery on the linux port). Pass
// nullptr to restore the default behavior: abort() falls back to the real
// one.
void mock_hooks_set_abort_hook(void (*hook)(void));

// The currently installed abort hook (nullptr when none is installed), type
// erased to void * so it can be compared against any hook function in tests.
[[nodiscard]] const void *mock_hooks_abort_hook();

}   // namespace mock_bus
