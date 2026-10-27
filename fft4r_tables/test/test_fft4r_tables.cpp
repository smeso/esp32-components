/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstddef>
#include <string>
#include <type_traits>

#include <doctest/doctest.h>

#include "fft4r_tables.hpp"

namespace {
// Return the value through a volatile load so the optimizer cannot fold the
// usable() branches of the callers below at compile time (a constant argument
// would make gcov report folded, never-taken edges).
std::size_t
as_runtime(std::size_t value)
{
    volatile std::size_t loaded = value;
    return loaded;
}
}   // namespace

TEST_CASE("the instance is usable after the first access")
{
    // The first instance() call initializes the global twiddle tables for
    // CONFIG_DSP_MAX_FFT_SIZE complex points.
    REQUIRE(fft4r_tables::instance().init_error() == ESP_OK);
    CHECK(fft4r_tables::instance().usable(16));
    // usable() assumes esp-dsp counts 2 * max_fft_size twiddles and exposes
    // the table through dsps_fft4r_w_table_fc32: pin those internals so an
    // esp-dsp change breaks the build instead of the transform.
    CHECK(dsps_fft4r_initialized != 0);
    CHECK(dsps_fft4r_w_table_fc32 != nullptr);
    CHECK(dsps_fft4r_w_table_size == 2 * CONFIG_DSP_MAX_FFT_SIZE);
}

TEST_CASE("usable() only accepts power-of-four lengths")
{
    // The radix-4 transform is defined for 4, 16, 64, ... complex points
    // only: a zero, non-power-of-two or power-of-two-but-not-power-of-four
    // length is rejected.
    CHECK_FALSE(fft4r_tables::instance().usable(as_runtime(0)));
    CHECK_FALSE(fft4r_tables::instance().usable(as_runtime(1)));
    CHECK_FALSE(fft4r_tables::instance().usable(as_runtime(2)));
    CHECK_FALSE(fft4r_tables::instance().usable(as_runtime(8)));
    CHECK_FALSE(fft4r_tables::instance().usable(as_runtime(12)));
    CHECK_FALSE(fft4r_tables::instance().usable(as_runtime(32)));
    CHECK(fft4r_tables::instance().usable(as_runtime(4)));
    CHECK(fft4r_tables::instance().usable(as_runtime(16)));
    CHECK(fft4r_tables::instance().usable(as_runtime(64)));
}

TEST_CASE("usable() rejects sizes that exceed the twiddle table")
{
    // A power-of-four size larger than any table CONFIG_DSP_MAX_FFT_SIZE can
    // produce (the largest is 2 * 32768 twiddles) is rejected by the size
    // comparison, not by the length check.
    CHECK_FALSE(fft4r_tables::instance().usable(as_runtime(std::size_t{ 1 } << 24)));
}

TEST_CASE("usable() rejects sizes whose doubled length exceeds INT_MAX")
{
    // 2^30 is a power of four but does not fit the int-typed table size
    // doubled: it is rejected by the range check so the size comparison
    // never has to compute fft_size * 2 (which would wrap on 32-bit).
    CHECK_FALSE(fft4r_tables::instance().usable(as_runtime(std::size_t{ 1 } << 30)));
}

TEST_CASE("usable() trusts the live table size, not the init flag")
{
    // A foreign earlier initialization could have left a smaller table
    // behind: the live table size (not the initialization flag recorded by
    // this class) decides. The size is restored before the test ends: the
    // table itself is untouched, only the (otherwise global) size variable
    // is briefly wrong.
    REQUIRE(dsps_fft4r_init_fc32(nullptr, CONFIG_DSP_MAX_FFT_SIZE) == ESP_OK);
    const int saved_size = dsps_fft4r_w_table_size;
    dsps_fft4r_w_table_size = 0;
    CHECK_FALSE(fft4r_tables::instance().usable(16));
    dsps_fft4r_w_table_size = saved_size;
    CHECK(fft4r_tables::instance().usable(16));
}

TEST_CASE("usable() detects a table de-initialized by a foreign owner")
{
    auto &tables = fft4r_tables::instance();
    REQUIRE(tables.usable(16));
    // Release the owned tables and hand esp-dsp a foreign buffer instead.
    tables.reset();
    REQUIRE(dsps_fft4r_initialized == 0);
    float foreign_buf[4 * 16];
    REQUIRE(dsps_fft4r_init_fc32(foreign_buf, 16) == ESP_OK);
    // instance() adopts the foreign initialization (it does not re-init).
    REQUIRE(&fft4r_tables::instance() == &tables);
    CHECK(tables.usable(16));
    // The foreign owner releases the tables behind our back: the recorded
    // initialized_ flag is stale, so usable() must inspect esp-dsp's flag.
    dsps_fft4r_deinit_fc32();
    CHECK(dsps_fft4r_initialized == 0);
    CHECK_FALSE(tables.usable(16));

    // Restore the owned full-size tables for the remaining tests.
    tables.reset();
    REQUIRE(fft4r_tables::instance().usable(16));
    CHECK(dsps_fft4r_w_table_fc32 != foreign_buf);
}

TEST_CASE("usable() detects a null table pointer")
{
    auto &tables = fft4r_tables::instance();
    REQUIRE(tables.usable(16));
    float *const saved_table = dsps_fft4r_w_table_fc32;
    dsps_fft4r_w_table_fc32 = nullptr;
    CHECK_FALSE(tables.usable(16));
    dsps_fft4r_w_table_fc32 = saved_table;
    CHECK(tables.usable(16));
}

TEST_CASE("reset() leaves a foreign initialization untouched")
{
    auto &tables = fft4r_tables::instance();
    REQUIRE(tables.usable(16));
    tables.reset();
    // A foreign initialization with its own buffer (esp-dsp does not own the
    // memory): reset() must not de-initialize it.
    float foreign_buf[4 * 16];
    REQUIRE(dsps_fft4r_init_fc32(foreign_buf, 16) == ESP_OK);
    REQUIRE(&fft4r_tables::instance() == &tables);
    CHECK(dsps_fft4r_w_table_fc32 == foreign_buf);
    tables.reset();
    CHECK(dsps_fft4r_initialized != 0);
    CHECK(dsps_fft4r_w_table_fc32 == foreign_buf);

    // Restore the owned full-size tables for the remaining tests.
    dsps_fft4r_deinit_fc32();
    REQUIRE(fft4r_tables::instance().usable(16));
}

TEST_CASE("reset() releases the tables and instance() re-initializes them")
{
    // The phoenix behavior: reset() frees the twiddle tables (usable() goes
    // false) and the next instance() call re-initializes them at full size.
    auto &tables = fft4r_tables::instance();
    REQUIRE(tables.usable(16));
    tables.reset();
    CHECK_FALSE(tables.usable(16));
    CHECK(tables.init_error() == ESP_OK);

    REQUIRE(&fft4r_tables::instance() == &tables);
    CHECK(fft4r_tables::instance().usable(16));
    CHECK(dsps_fft4r_w_table_size == 2 * CONFIG_DSP_MAX_FFT_SIZE);
}

TEST_CASE("reset() is a no-op when the tables are already released")
{
    // Resetting twice in a row must not de-initialize esp-dsp a second time
    // (the second deinit would free nothing, but keeping the state machine
    // strict keeps the esp-dsp global flags in sync with the singleton).
    auto &tables = fft4r_tables::instance();
    tables.reset();
    tables.reset();
    CHECK(tables.init_error() == ESP_OK);
    CHECK_FALSE(tables.usable(16));

    REQUIRE(&fft4r_tables::instance() == &tables);
    CHECK(fft4r_tables::instance().usable(16));
}

TEST_CASE("repeated instance() calls return the same object")
{
    // The singleton is neither copyable nor movable: every caller shares the
    // one process-global instance (and its tables are initialized once).
    CHECK(&fft4r_tables::instance() == &fft4r_tables::instance());
    static_assert(!std::is_copy_constructible_v<fft4r_tables>);
    static_assert(!std::is_copy_assignable_v<fft4r_tables>);
}
