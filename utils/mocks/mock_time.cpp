/*
 * SPDX-FileCopyrightText: 2026 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mock_time.hpp"

#include <errno.h>

namespace mock_bus {

namespace {

bool enabled = false;
struct timeval now{};
unsigned settimeofday_count = 0;
struct timeval last_settimeofday{};
bool fail_settimeofday = false;
unsigned adjtime_count = 0;
struct timeval last_adjtime{};
bool fail_adjtime = false;

}   // namespace

void
mock_time_reset()
{
    enabled = false;
    now = {};
    settimeofday_count = 0;
    last_settimeofday = {};
    fail_settimeofday = false;
    adjtime_count = 0;
    last_adjtime = {};
    fail_adjtime = false;
}

void
mock_time_set(const struct timeval &tv)
{
    enabled = true;
    now = tv;
}

struct timeval
mock_time_get()
{
    return now;
}

bool
mock_time_is_enabled()
{
    return enabled;
}

unsigned
mock_time_settimeofday_count()
{
    return settimeofday_count;
}

struct timeval
mock_time_last_settimeofday()
{
    return last_settimeofday;
}

void
mock_time_record_settimeofday(const struct timeval &tv)
{
    ++settimeofday_count;
    last_settimeofday = tv;
}

unsigned
mock_time_adjtime_count()
{
    return adjtime_count;
}

struct timeval
mock_time_last_adjtime()
{
    return last_adjtime;
}

void
mock_time_record_adjtime(const struct timeval &delta)
{
    ++adjtime_count;
    last_adjtime = delta;
}

void
mock_time_fail_settimeofday(bool fail)
{
    fail_settimeofday = fail;
}

bool
mock_time_fail_settimeofday_enabled()
{
    return fail_settimeofday;
}

void
mock_time_fail_adjtime(bool fail)
{
    fail_adjtime = fail;
}

bool
mock_time_fail_adjtime_enabled()
{
    return fail_adjtime;
}

}   // namespace mock_bus

// ---------------------------------------------------------------------------
// Mocked libc time entry points. The test component is linked into the
// firmware before the esp_libc library, so the linker --wrap options in this
// component's CMakeLists.txt redirect references to gettimeofday() and
// settimeofday() to the __wrap_* implementations below. This lets tests
// exercise drivers that read or set the ESP32 system time without depending
// on the real clock. While the fake clock is disabled the real functions are
// called, so the rest of the firmware (e.g. the pthread condition-variable
// implementation and the log timestamp code) keeps working.
// ---------------------------------------------------------------------------

extern "C" {

int __real_gettimeofday(struct timeval *tv, void *tz);
int __real_settimeofday(const struct timeval *tv, const struct timezone *tz);
int __real_adjtime(const struct timeval *delta, struct timeval *olddelta);

int
__wrap_gettimeofday(struct timeval *tv, void *tz)
{
    if (!mock_bus::mock_time_is_enabled())
        return __real_gettimeofday(tv, tz);
    if (tv != nullptr)
        *tv = mock_bus::mock_time_get();
    (void) tz;
    return 0;
}

int
__wrap_settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    if (!mock_bus::mock_time_is_enabled()) {
        if (mock_bus::mock_time_fail_settimeofday_enabled())
            return -1;
        return __real_settimeofday(tv, tz);
    }
    if (mock_bus::mock_time_fail_settimeofday_enabled()) {
        errno = EINVAL;
        return -1;
    }
    if (tv != nullptr)
        mock_bus::mock_time_record_settimeofday(*tv);
    (void) tz;
    return 0;
}

int
__wrap_adjtime(const struct timeval *delta, struct timeval *olddelta)
{
    if (!mock_bus::mock_time_is_enabled()) {
        if (mock_bus::mock_time_fail_adjtime_enabled())
            return -1;
        return __real_adjtime(delta, olddelta);
    }
    if (mock_bus::mock_time_fail_adjtime_enabled()) {
        errno = EINVAL;
        return -1;
    }
    if (delta != nullptr)
        mock_bus::mock_time_record_adjtime(*delta);
    if (olddelta != nullptr)
        *olddelta = {};
    return 0;
}

}   // extern "C"
