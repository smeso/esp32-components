/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <bit>
#include <climits>
#include <cstddef>

#include <esp_err.h>

#include "dsps_fft4r.h"
#include "sdkconfig.h"

/**
 * Process-global owner of the esp-dsp radix-4 FFT twiddle tables.
 *
 * The tables are sized for CONFIG_DSP_MAX_FFT_SIZE (the largest possible
 * FFT) and are shared by every sound_driven instance: they are initialized
 * on first use and can be released again with reset() to free their memory
 * (a phoenix singleton: the next instance() call re-initializes them).
 * esp-dsp keeps global twiddle tables with non-atomic init flags, so the
 * users of this class must only access it from a single audio task.
 *
 * esp-dsp may already be initialized by somebody else (with its own buffer
 * or a smaller table): such a foreign initialization is adopted, not owned,
 * so reset() and the destructor leave it alone. Because a foreign owner can
 * also de-initialize the tables at any time, usable() verifies the live
 * esp-dsp state (initialization flag and table pointer) as well as the table
 * size instead of trusting the state recorded at initialization time.
 */
class fft4r_tables
{
  public:
    static fft4r_tables &instance()
    {
        // The thread-safe function-local static guard has a dead edge that
        // gcovr would otherwise count as uncovered.
        static fft4r_tables tables;   // GCOVR_EXCL_BR_LINE
        tables.init();
        return tables;
    }

    fft4r_tables(const fft4r_tables &) = delete;
    fft4r_tables &operator=(const fft4r_tables &) = delete;

    /**
     * Whether a radix-4 FFT of the given size can be run right now: the
     * transform is only defined for a power-of-four number of complex points
     * (>= 4), and the twiddle tables must currently be held by the process
     * and be large enough for that many complex points.
     */
    [[nodiscard]] bool usable(std::size_t fft_size) const noexcept
    {
        if (!initialized_)
            return false;
        if (!is_power_of_four(fft_size))
            return false;
        // Reject sizes whose doubled length would not fit the int-typed table
        // size (the comparison below is done in std::size_t so it cannot
        // wrap, but an oversized fft_size would scale to a zero wind step).
        if (fft_size > static_cast<std::size_t>(INT_MAX) / 2)
            return false;
        if (dsps_fft4r_initialized == 0)
            return false;
        if (dsps_fft4r_w_table_fc32 == nullptr)
            return false;
        return static_cast<std::size_t>(dsps_fft4r_w_table_size) >= fft_size * 2;
    }

    /**
     * The error reported by the last dsps_fft4r_init_fc32() call, ESP_OK on
     * success (and while the tables are released by reset()). Note that
     * instance() re-initializes the tables on every call, so the released
     * state is only observable through a reference obtained before reset().
     */
    [[nodiscard]] esp_err_t init_error() const noexcept
    {
        return init_err_;   // GCOVR_EXCL_BR_LINE: the throw edge of -fexceptions
    }

    /**
     * Release the twiddle tables this class owns, freeing their memory. A
     * foreign initialization is left untouched. As every other method, it
     * must only be called from the task owning the tables, when no FFT is
     * running. The next instance() call re-initializes them.
     */
    void reset() noexcept
    {
        // The compiler inlines reset() into every caller and folds this guard
        // to the owned_ value known there, so each inlined copy keeps only one
        // edge and gcovr counts the other as a dead branch, even though both
        // the owned and the foreign path are exercised by the reset() tests.
        if (owned_)   // GCOVR_EXCL_BR_LINE
            dsps_fft4r_deinit_fc32();
        initialized_ = false;
        owned_ = false;
        init_err_ = ESP_OK;
    }

  private:
    fft4r_tables() = default;

    // GCOVR_EXCL_START: never runs in the tests (static lifetime); its body
    // must stay inside this region, otherwise the uncovered branches of the
    // ownership guard (repeated by hand instead of by calling reset(), whose
    // inlined copy would be attributed to reset()'s own lines) would pollute
    // the coverage report.
    ~fft4r_tables()
    {
        if (owned_)
            dsps_fft4r_deinit_fc32();
    }
    // GCOVR_EXCL_STOP

    void init()
    {
        if (initialized_)
            return;
        // Adopt (without owning) a table somebody else initialized, and
        // remember whether this call turned esp-dsp off -> on so that reset()
        // and the destructor only de-initialize what this class allocated.
        const bool foreign = dsps_fft4r_initialized != 0;
        init_err_ = dsps_fft4r_init_fc32(nullptr, CONFIG_DSP_MAX_FFT_SIZE);
        initialized_ = init_err_ == ESP_OK;
        owned_ = false;
        if (!foreign)
            owned_ = initialized_;
    }

    // 4, 16, 64, ...: a radix-4 transform needs a power of two whose only set
    // bit sits at an even position, and at least one radix-4 stage (1 = 4^0
    // is rejected).
    static constexpr bool is_power_of_four(std::size_t n) noexcept
    {
        return std::has_single_bit(n) && n >= 4 && (std::countr_zero(n) % 2 == 0);
    }

    bool initialized_ = false;
    bool owned_ = false;
    esp_err_t init_err_ = ESP_OK;
};
