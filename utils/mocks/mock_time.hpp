/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <sys/time.h>

namespace mock_bus {

// Reset all time mock state: fake clock disabled and settimeofday history
// cleared.
void mock_time_reset();

// Enable the fake clock and set the value returned by gettimeofday().
void mock_time_set(const struct timeval &tv);
[[nodiscard]] struct timeval mock_time_get();

// Whether the fake clock is currently enabled (gettimeofday() returns the
// fake value instead of the real one).
[[nodiscard]] bool mock_time_is_enabled();

// History of settimeofday() calls made while the fake clock is enabled.
[[nodiscard]] unsigned mock_time_settimeofday_count();
[[nodiscard]] struct timeval mock_time_last_settimeofday();
void mock_time_record_settimeofday(const struct timeval &tv);

// History of adjtime() calls made while the fake clock is enabled.
[[nodiscard]] unsigned mock_time_adjtime_count();
[[nodiscard]] struct timeval mock_time_last_adjtime();
void mock_time_record_adjtime(const struct timeval &delta);

// Failure injection: make settimeofday() return -1.
void mock_time_fail_settimeofday(bool fail);
[[nodiscard]] bool mock_time_fail_settimeofday_enabled();

// Failure injection: make adjtime() return -1.
void mock_time_fail_adjtime(bool fail);
[[nodiscard]] bool mock_time_fail_adjtime_enabled();

}   // namespace mock_bus
