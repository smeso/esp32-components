/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

namespace mock_bus {

// Reset all timer mock state (failure injection disabled).
void mock_timers_reset();

// Make xTimerCreate() fail after \p calls have been let through (0 = the very
// next call fails). Resets to disabled (never fails) on the next call to
// mock_timers_reset().
void mock_timers_fail_x_timer_create_after(unsigned calls);

// While enabled, make xTimerGenericCommand() fail for tmrCOMMAND_START, so
// xTimerStart() reports pdFAIL. All other timer commands are unaffected.
void mock_timers_fail_x_timer_start(bool fail);

// While enabled, make xTimerGenericCommand() fail for tmrCOMMAND_STOP, so
// xTimerStop() reports pdFAIL (the stop is dropped, as when the timer command
// queue is full). All other timer commands are unaffected.
void mock_timers_fail_x_timer_stop(bool fail);

// Number of tmrCOMMAND_DELETE commands sent to the timer daemon task since the
// last mock_timers_reset(). Counted when the command is enqueued (i.e. inside
// xTimerDelete()), not when the daemon processes it, so a construction or
// destruction path can be checked for having released its timers.
[[nodiscard]] unsigned mock_timers_delete_count();

// While enabled, make xTaskCreate() report failure without creating a task,
// so components that wrap it (e.g. cpptaskcreate, task_queue) can exercise
// their error-recovery paths on the Linux simulator, where the host
// allocator otherwise never runs out of memory.
void mock_timers_fail_x_task_create(bool fail);

// While enabled, make xQueueGenericCreate() (the function behind the
// xQueueCreate()/xSemaphoreCreateBinary() macros) return nullptr, so
// components can exercise their queue/semaphore creation failure paths.
// Fails the queue/semaphore creation call after \p calls have been let
// through (0 = the very next call fails).
void mock_timers_fail_x_queue_create(unsigned calls);

}   // namespace mock_bus
