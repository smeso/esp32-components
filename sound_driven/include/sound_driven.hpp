/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <limits>
#include <numbers>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <esp_err.h>
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
#include "fft4r_tables.hpp"
#include "sdkconfig.h"
#include "spiram_allocation.hpp"

/**
 * Wraps a DAC (e.g. es8311) and reports the amplitude of every buffer
 * written to it, so that e.g. a LED can be dimmed from 0 to 100% following
 * the music. It can also be used to pilot any sort of actuator.
 *
 * With SpectralGroups == 1 the callback receives one value per output
 * channel: the peak amplitude of that channel in the buffer. With
 * SpectralGroups > 1 the callback receives OutputChannels * SpectralGroups
 * values: for every output channel the audible spectrum of the buffer is
 * computed with an FFT (the fastest optimized FFT available in esp-dsp: the
 * radix-4 implementation) and the (scaled) magnitude of each of the
 * SpectralGroups frequency bands is reported. The bands follow the bark scale
 * of human hearing, so low frequencies get proportionally more bands than
 * high ones. Only bins up to the Nyquist frequency (sample_rate / 2) are
 * usable: a sample rate so low that no bin falls inside the audible range
 * makes the bands fall back to an even split.
 *
 * The analysis window is the largest power-of-four number of frames not
 * exceeding the chunk size used by write(): when a stream ends with a partial
 * chunk (or is written in uneven pieces) the last callbacks are computed with
 * a smaller FFT, so the bark band edges differ from the previous ones and
 * the visual output can jump for that buffer. Buffers with fewer than 16
 * frames per channel report all zeros (and, for SpectralGroups > 1, so do the
 * trailing samples that do not form a whole frame). The peak path instead
 * scans every sample, including a trailing partial frame. The FFT input is
 * DC-removed and Hann-windowed before the transform (the window's coherent
 * gain is compensated afterwards), so a DC offset and the leakage of the
 * rectangular window do not land in the low-frequency bands.
 *
 * The (potentially large) FFT scratch buffer is heap-allocated in SPI RAM
 * through the spiram_allocation component, so a sound_driven instance stays
 * small enough to live on the caller's stack.
 *
 * The destructor invokes the callback once more with an all-zero report (as
 * an end-of-stream notification) whenever the callback is not empty, even if
 * write() was never called. The notification is best-effort: the exceptions
 * thrown by the callback are caught and logged, so destroying a sound_driven
 * instance never throws. The callback must therefore not rely on resources
 * that may already be gone when the instance is destroyed.
 *
 * Can throw: the sink, the callback and (in the spectral path) the FFT buffer
 * allocation may throw, so construction and every method can propagate an
 * exception. See write() for the details.
 *
 * Threading: not thread-safe. Instances hold mutable state (the FFT buffer)
 * and esp-dsp keeps global twiddle tables with non-atomic init flags, so
 * write() (and every other method) must only be called from a single audio
 * task. An instance constructed with an empty callback (e.g. a moved-from
 * one) skips the notifications.
 */
template<typename T,
         std::size_t OutputChannels = 1,
         std::size_t SpectralGroups = 1,
         typename DataType = uint16_t>
class sound_driven
{
    static constexpr const char *TAG = "sound_driven";

  public:
    static_assert(OutputChannels >= 1 && OutputChannels <= 255,
                  "OutputChannels must be between 1 and 255");
    static_assert(SpectralGroups >= 1, "SpectralGroups must be at least 1");
    static_assert(std::is_integral_v<DataType>,
                  "DataType must be an integral type (the amplitudes are counts)");
    static constexpr auto mclk_multiple = T::mclk_multiple;
    static constexpr size_t callback_size = OutputChannels * SpectralGroups;

    class exception final : public std::exception
    {
      public:
        explicit exception(const char *msg, esp_err_t err = ESP_OK) noexcept
        {
            if (err != ESP_OK) {
                char err_buf[64];
                esp_err_to_name_r(err, err_buf, sizeof(err_buf));
                std::snprintf(msg_buf_, sizeof(msg_buf_), "%s: %s: %s", TAG, msg, err_buf);
            } else {
                std::snprintf(msg_buf_, sizeof(msg_buf_), "%s: %s", TAG, msg);
            }
        }
        ~exception() override = default;
        const char *what() const noexcept override   // GCOVR_EXCL_BR_LINE
        {
            return msg_buf_;   // GCOVR_EXCL_BR_LINE
        }

      private:
        char msg_buf_[128]{};
    };

    /**
     * @param dac       DAC (or any sink) the audio chunks are written to
     * @param callback  Called with the computed amplitudes of every chunk (by
     *                  const reference: the report array is not copied)
     */
    sound_driven(T &dac,
                 std::function<void(const std::array<DataType, callback_size> &)> callback)
            : dac_(dac)
            , callback_(std::move(callback))
    {
    }

    sound_driven(const sound_driven &) = delete;
    sound_driven &operator=(const sound_driven &) = delete;
    sound_driven(sound_driven &&other) noexcept
            : dac_(other.dac_)
            , callback_(std::move(other.callback_))
            , fft_buf_(std::move(other.fft_buf_))
    {
        // The destructor notifies the (moved-away) callback: without this
        // reset the source object could fire a spurious extra "all zeros"
        // notification on top of the one from the moved-to instance.
        other.callback_ = nullptr;
    }
    sound_driven &operator=(sound_driven &&) = delete;

    ~sound_driven()
    {
        if (callback_) {
            try {
                // GCOVR_EXCL_BR_LINE: destructor exception-cleanup edge (the
                // throwing-callback tests cover the unwind into the catch
                // clauses; gcov reports the never-taken throw arcs of the
                // non-throwing instantiations as uncovered branches).
                callback_({});   // GCOVR_EXCL_BR_LINE
            } catch (const std::exception &e) {
                ESP_LOGW(TAG, "callback failed in destructor: %s", e.what());
            } catch (...) {
                ESP_LOGW(TAG, "callback failed in destructor");
            }
        }
    }

    [[nodiscard]] uint32_t get_sample_rate() const
    {
        return dac_.get_sample_rate();   // GCOVR_EXCL_BR_LINE
    }

    /**
     * Write interleaved audio samples to the DAC, analyzing every chunk and
     * reporting its amplitudes through the callback.
     *
     * The analysis callback runs before dac_.write(): a failed or timed-out
     * DAC write still fed the visualizer with audio that is then not played.
     * The last chunk may end on a partial frame (buf.size() not a multiple of
     * channels): it is passed verbatim to dac_.write() (a sink like es8311
     * rejects it) and, for SpectralGroups == 1, its samples still take part in
     * the peak (the spectral path drops them).
     *
     * @param buf       Interleaved signed integer samples
     * @param channels  Number of interleaved channels in buf (1..255, must
     *                  not be 0 or the call does nothing and returns 0). When
     *                  it is smaller than OutputChannels the extra output
     *                  channels permanently report 0.
     * @param timeout   FreeRTOS ticks to wait for the DAC write
     * @return Number of samples actually accepted by the DAC, in the unit the
     *         sink's write() reports them in (samples for the sinks shipped
     *         with this component, bytes for es8311::write()). It may be less
     *         than buf.size() when the DAC accepts a partial chunk (e.g. on
     *         timeout expiry): the unwritten tail is dropped and not
     *         re-analyzed. The total is clamped to buf.size() in case a sink
     *         reports more than the chunk it was handed.
     * @throw Whatever dac_.write() or the callback throw, plus the spectral
     *        path's std::bad_alloc (FFT buffer) and sound_driven::exception
     *        (unusable esp-dsp twiddle table). When one of them is thrown the
     *        current chunk has already been analyzed but not written.
     */
    template<typename Buf>
    [[nodiscard]] size_t write(const Buf &buf,
                               uint8_t channels = 2,
                               TickType_t timeout = portMAX_DELAY)
    {
        if (channels == 0 || buf.size() == 0)
            return 0;
        using value_type = typename Buf::value_type;
        // Reject unsupported sample containers here (the analysis paths repeat
        // the check) so the diagnostic points at the call site.
        static_assert(std::is_integral_v<value_type> && std::is_signed_v<value_type>,
                      "sound_driven only supports signed integer sample types");
        const size_t total = buf.size();
        const size_t frames = max_chunk_frames();
        size_t written = 0;
        size_t offset = 0;
        while (offset < total) {
            const size_t n = std::min(frames * channels, total - offset);
            std::span<const value_type> chunk{ buf.data() + offset, n };
            if (callback_)
                // GCOVR_EXCL_BR_LINE: the callback report path is exercised
                // by the partial-write tests; gcov only mis-attributes the
                // exception-handling edge of the analysis call.
                callback_(compute_amplitudes(chunk, channels));   // GCOVR_EXCL_BR_LINE
            // GCOVR_EXCL_BR_LINE: the never-taken arcs gcov reports here are
            // the exception edges of the sink's write() call (no test sink
            // throws), which gcov keeps on the block it could not execute.
            written += dac_.write(chunk, channels, timeout);   // GCOVR_EXCL_BR_LINE
            offset += n;
        }
        return std::min(written, total);
    }

  private:
    static constexpr size_t min_chunk_frames = 64;
    static constexpr size_t max_allowed_chunk_frames = CONFIG_DSP_MAX_FFT_SIZE;
    // The two std::clamp() bounds below must be ordered (lo <= hi).
    static_assert(max_allowed_chunk_frames >= min_chunk_frames,
                  "CONFIG_DSP_MAX_FFT_SIZE must be at least 64");

    [[nodiscard]] size_t max_chunk_frames() const
    {
        // ~50 ms worth of frames at the current sample rate, rounded to the
        // nearest power of four and clamped to
        // [min_chunk_frames, max_allowed_chunk_frames]. The candidates are
        // computed in 64 bits so the arithmetic cannot wrap for any sample
        // rate or CONFIG_DSP_MAX_FFT_SIZE (including the non-power-of-four
        // values, 512 / 2048 / ..., offered by the esp-dsp Kconfig).
        const uint64_t n = std::clamp(static_cast<size_t>(dac_.get_sample_rate()) / 20,
                                      min_chunk_frames,
                                      max_allowed_chunk_frames);
        uint64_t lower = 1;
        while (lower * 4 <= n &&                          // GCOVR_EXCL_BR_LINE
               lower * 4 <= max_allowed_chunk_frames) {   // GCOVR_EXCL_BR_LINE
            lower *= 4;
        }   // GCOVR_EXCL_LINE
        // GCOVR_EXCL_BR_START: the two candidates and the tie-break below are
        // covered by the chunk-size tests; gcov only mis-attributes the edges.
        // The upper candidate is rounded down to a power of four as well: with
        // a non-power-of-four CONFIG_DSP_MAX_FFT_SIZE the clamp would
        // otherwise return a frame count the FFT cannot use (e.g. 512, when
        // only a 256-point transform fits).
        const uint64_t upper = largest_power_of_four(
                std::min(lower * 4, static_cast<uint64_t>(max_allowed_chunk_frames)));
        // Pick the candidate closest to n; ties round down. upper can be below
        // n (both bounds are not powers of four), so the difference is only
        // meaningful when upper >= n.
        const bool upper_closer = upper >= n && upper - n < n - lower;
        return static_cast<size_t>(upper_closer ? upper : lower);
        // GCOVR_EXCL_BR_STOP
    }

    template<typename Buf>
    std::array<DataType, callback_size> compute_amplitudes(const Buf &buf, uint8_t channels)
    {
        if constexpr (SpectralGroups == 1)
            return compute_peak_amplitudes(buf, channels);
        else
            // GCOVR_EXCL_BR_LINE: gcov reports the never-taken throw arcs of
            // the spectral call (compute_spectral_amplitudes can throw).
            return compute_spectral_amplitudes(buf, channels);   // GCOVR_EXCL_BR_LINE
    }

    template<typename Buf>
    static std::array<DataType, OutputChannels> compute_peak_amplitudes(const Buf &buf,
                                                                        uint8_t channels)
    {
        using value_type = typename Buf::value_type;
        // Signed integer samples are required: the peak logic below negates
        // the samples and scales them against their maximum, which is
        // meaningless (and, for floating-point types, undefined) otherwise.
        static_assert(std::is_integral_v<value_type>,
                      "sound_driven only supports integer sample types");
        static_assert(std::is_signed_v<value_type>,
                      "sound_driven only supports signed sample types");
        std::array<DataType, OutputChannels> amplitudes{};
        if constexpr (!std::is_integral_v<value_type> || !std::is_signed_v<value_type>) {
            // The static_assert() calls above already fired a clean
            // diagnostic; the else branch below is discarded for these types,
            // so the code it holds (make_unsigned and the arithmetic) is never
            // instantiated for them and does not add errors to the diagnostic.
            return amplitudes;
        } else {
            // The intermediate is the unsigned counterpart of the sample type,
            // one bit wider than the positive range: |INT16_MIN| does not fit
            // in int16_t and negating INT32_MIN would overflow.
            using wide_type = std::make_unsigned_t<value_type>;
            constexpr wide_type full_scale = std::numeric_limits<wide_type>::max() / 2 + 1;
            for (size_t o = 0; o < OutputChannels; ++o) {
                wide_type peak = 0;
                // A size_t counter: an 8-bit one would wrap when
                // o + k * OutputChannels exceeds 255 (channels can be up to
                // 255) and either loop forever or re-read a channel.
                for (size_t c = o; c < channels; c += OutputChannels) {
                    // GCOVR_EXCL_BR_LINE: gcovr's decision analyzer reports
                    // "couldn't be analyzed" for this loop (its variable step
                    // and merged template copies defeat the arc pattern
                    // matching), even though the loop is heavily exercised by
                    // every peak test and all of its branches are taken.
                    for (size_t i = c; i < buf.size(); i += channels) {   // GCOVR_EXCL_BR_LINE
                        // Casting through the unsigned counterpart of the
                        // sample type yields the absolute value without any
                        // overflow, so a full-scale negative sample is
                        // measured correctly.
                        const wide_type u = static_cast<wide_type>(buf[i]);
                        const wide_type v = buf[i] < 0 ? static_cast<wide_type>(-u) : u;
                        if (v > peak)
                            peak = v;
                    }
                }
                amplitudes[o] = scale_to_data_type(peak, full_scale);
            }
            return amplitudes;
        }
    }

    template<typename Buf>
    std::array<DataType, callback_size> compute_spectral_amplitudes(const Buf &buf,
                                                                    uint8_t channels)
    {
        using value_type = typename Buf::value_type;
        // Signed samples are required: they are scaled to [-1, 1] against
        // their maximum below, which is meaningless (and, for floating-point
        // types, undefined) otherwise. See compute_peak_amplitudes().
        static_assert(std::is_integral_v<value_type>,
                      "sound_driven only supports integer sample types");
        static_assert(std::is_signed_v<value_type>,
                      "sound_driven only supports signed sample types");
        constexpr auto data_max = std::numeric_limits<DataType>::max();
        std::array<DataType, callback_size> amplitudes{};
        const size_t frames = buf.size() / channels;   // frames per channel
        if (frames == 0)
            return {};
        // The radix-4 FFT requires a power-of-four number of complex points.
        const size_t fft_size = largest_power_of_four(frames);
        if (fft_size < 16)
            return {};
        // The frequency of bin k is k * sample_rate / fft_size; only bins up
        // to the Nyquist frequency are usable.
        const size_t bins = fft_size / 2 + 1;
        const auto bin_edges = bark_bin_edges(bins, dac_.get_sample_rate(), fft_size);
        // The twiddle tables are process-global esp-dsp state shared by every
        // instance: they are sized for CONFIG_DSP_MAX_FFT_SIZE (the largest
        // possible FFT). fft4r_tables owns their initialization and
        // deinitialization; usable() verifies the live esp-dsp state (a
        // foreign owner may have released the tables behind our back) and
        // that the table is large enough for this power-of-four fft_size.
        if (!fft4r_tables::instance().usable(fft_size)) {     // GCOVR_EXCL_BR_LINE
            throw exception(                                  // GCOVR_EXCL_LINE
                    "FFT twiddle table unavailable",          // GCOVR_EXCL_LINE
                    fft4r_tables::instance().init_error());   // GCOVR_EXCL_LINE
        }
        // Values are scaled to [-1, 1] before the FFT. The 2x factor
        // compensates the coherent gain (0.5) of the Hann window applied by
        // prepare_fft_frame(), so a full-scale sine wave still reads as full
        // scale after the transform.
        constexpr float sample_scale =
                static_cast<float>(2) /
                static_cast<float>(std::numeric_limits<value_type>::max());
        ensure_fft_buffer(fft_size * 2);
        float *fft_buf = fft_buf_.data();
        for (size_t o = 0; o < OutputChannels; ++o) {
            std::fill(fft_buf, fft_buf + fft_size * 2, 0.0f);
            // A size_t counter: an 8-bit one would wrap when
            // o + k * OutputChannels exceeds 255 (channels can be up to 255)
            // and either loop forever or re-accumulate a channel that was
            // already summed into the FFT buffer.
            for (size_t c = o; c < channels; c += OutputChannels) {
                size_t frame = 0;
                for (size_t i = c; i < buf.size(); i += channels) {   // GCOVR_EXCL_BR_LINE
                    if (frame >= fft_size)                            // GCOVR_EXCL_BR_LINE
                        break;
                    fft_buf[2 * frame] += static_cast<float>(buf[i]) * sample_scale;
                    ++frame;
                }
            }
            prepare_fft_frame(fft_buf, fft_size);
            // The esp-dsp real FFT: the optimized radix-4 implementation is
            // selected by CONFIG_DSP_OPTIMIZED and the result is converted
            // back to a real spectrum (bin 0 is DC, bin fft_size / 2 is the
            // Nyquist frequency).
            //
            // Failure (ESP_ERR_DSP_UNINITIALIZED for tables released behind
            // our back after the usable() check above, a bad size, ...) would
            // otherwise leave the buffer holding the time-domain input and the
            // bands would be computed from it instead of the spectrum, so the
            // codes are checked and reported.
            esp_err_t err = dsps_fft4r_fc32(fft_buf, static_cast<int>(fft_size));
            if (err == ESP_OK)   // GCOVR_EXCL_BR_LINE
                err = dsps_bit_rev4r_fc32(fft_buf, static_cast<int>(fft_size));
            if (err == ESP_OK)   // GCOVR_EXCL_BR_LINE
                err = dsps_cplx2real_fc32(fft_buf, static_cast<int>(fft_size));
            if (err != ESP_OK)                                  // GCOVR_EXCL_BR_LINE
                throw exception("FFT transform failed", err);   // GCOVR_EXCL_LINE
            for (size_t g = 0; g < SpectralGroups; ++g) {
                const size_t first = bin_edges[g];
                const size_t last = bin_edges[g + 1];
                if (last <= first)
                    continue;
                // The power of the band is the sum of the power of its bins
                // (bark-band analysis integrates the power inside each
                // critical band).
                double power = 0.0;
                for (size_t k = first; k < last; ++k) {
                    const double re = fft_buf[2 * k];
                    const double im = fft_buf[2 * k + 1];
                    power += re * re + im * im;
                }
                // A full-scale sine wave on a single bin has magnitude
                // fft_size / 2, so scale by 2 / fft_size to make it saturate
                // the output.
                const double scaled = std::sqrt(power) * 2.0 / static_cast<double>(fft_size) *
                                      static_cast<double>(data_max);
                amplitudes[o * SpectralGroups + g] =
                        static_cast<DataType>(scaled > data_max ? data_max : scaled);
            }
        }
        return amplitudes;
    }

    // Removes the DC offset and applies a Hann window to the real part of the
    // FFT input buffer. Without the DC removal a constant offset (and, with
    // the rectangular window, the leakage of any strong content) lands in the
    // first bins, which are exactly the ones the first bark band relies on.
    // The window's coherent gain (0.5) is compensated by the callers through
    // sample_scale.
    static void prepare_fft_frame(float *fft, size_t fft_size)
    {
        double mean = 0.0;
        for (size_t i = 0; i < fft_size; ++i)
            mean += fft[2 * i];
        mean /= static_cast<double>(fft_size);
        const double denom = static_cast<double>(fft_size) - 1.0;
        for (size_t i = 0; i < fft_size; ++i) {
            const double window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi *
                                                       static_cast<double>(i) / denom);
            fft[2 * i] = static_cast<float>((static_cast<double>(fft[2 * i]) - mean) * window);
        }
    }

    void ensure_fft_buffer(size_t floats)
    {
        // GCOVR_EXCL_BR_LINE: gcov synthesizes a branch edge for the
        // short-circuit `&&` + early return that never has a taken arc on
        // xtensa, no matter how the buffer is reused (the early-return path
        // is exercised by the buffer-reuse tests).
        if (!fft_buf_.empty() && fft_buf_.size() >= floats)   // GCOVR_EXCL_BR_LINE
            return;
        // The FFT buffer can be large (up to CONFIG_DSP_MAX_FFT_SIZE complex
        // samples) and is only needed transiently while computing the
        // spectral amplitudes, so keep it out of internal RAM. The buffer is
        // released before the resize so a failing (larger) allocation cannot
        // leave the old block around; a failed allocation throws
        // std::bad_alloc out of write(), and only a later, smaller request can
        // succeed again.
        fft_buf_.shrink_to_fit();
        fft_buf_.resize(floats);
    }

    // Scales a sample peak (in [0, full_scale], where full_scale is the
    // magnitude of the most negative sample) to the output range [0,
    // numeric_limits<DataType>::max()], rounding to the nearest value.
    //
    // The product peak * data_max is evaluated in std::uint64_t: done in Value
    // it would silently wrap (Value can be as narrow as the sample type while
    // DataType is much wider), and the truncation of
    // numeric_limits<DataType>::max() down to Value would cap the output
    // below the advertised range. The static_assert rejects the pairs whose
    // product would not fit in 64 bits: the widest supported combination is
    // 32-bit samples with a 32-bit output, and 64-bit samples are rejected
    // (their product with any output maximum needs more than 64 bits).
    template<typename Value>
    static DataType scale_to_data_type(Value peak, Value full_scale)
    {
        static_assert(std::is_integral_v<Value> && std::is_unsigned_v<Value>,
                      "peak scaling requires an unsigned integral intermediate type");
        using wide_type = std::uint64_t;
        constexpr size_t value_bits = std::numeric_limits<Value>::digits;
        constexpr size_t data_bits =
                std::bit_width(static_cast<wide_type>(std::numeric_limits<DataType>::max()));
        static_assert(value_bits + data_bits <= 64,
                      "the sample and output types are too wide for the scaling arithmetic");
        constexpr wide_type data_max =
                static_cast<wide_type>(std::numeric_limits<DataType>::max());
        const wide_type scaled = (static_cast<wide_type>(peak) * data_max +
                                  static_cast<wide_type>(full_scale) / 2) /
                                 static_cast<wide_type>(full_scale);
        // peak <= full_scale by construction (it is a sample magnitude, at
        // most |INT_MIN| == full_scale), so scaled cannot exceed data_max and
        // the clamp the previous implementation needed is unnecessary.
        return static_cast<DataType>(scaled);
    }

    static constexpr size_t largest_power_of_four(size_t n) noexcept
    {
        size_t p = 1;
        while (p * 4 <= n && p * 4 <= CONFIG_DSP_MAX_FFT_SIZE)   // GCOVR_EXCL_BR_LINE
            p *= 4;
        return p;
    }

    static size_t bark_scale(double frequency) noexcept
    {
        // A negative frequency (only possible with pathological template
        // parameters) would make the float-to-integer conversion undefined;
        // clamp it to bin 0 instead.
        if (frequency <= 0.0)
            return 0;
        // Approximate the bark scale with the formula proposed by
        // Terhardt (1982): bark = 13.3 * atan(0.75 * f / 1000).
        const double bark = 13.3 * std::atan(0.75 * frequency / 1000.0);
        return static_cast<size_t>(bark);
    }

    static std::array<size_t, SpectralGroups + 1> bark_bin_edges(size_t bins,
                                                                 uint32_t sample_rate,
                                                                 size_t fft_size)
    {
        // The spectral path never reaches here with an empty spectrum (it
        // returns before the FFT when it cannot produce at least 16 frames),
        // so bins is always at least fft_size / 2 + 1 with fft_size >= 16.
        std::array<size_t, SpectralGroups + 1> edges{};
        constexpr size_t groups = SpectralGroups;
        const auto bark_of_bin = [sample_rate, fft_size](size_t k) {
            return bark_scale(static_cast<double>(k) * sample_rate /
                              static_cast<double>(fft_size));
        };
        // The highest usable bin is the Nyquist frequency.
        const size_t max_bark = bark_of_bin(bins - 1);
        if (max_bark == 0) {   // GCOVR_EXCL_BR_LINE
            // No usable bin: fall back to an even split.
            for (size_t g = 0; g <= groups; ++g)
                edges[g] = g * bins / groups;
            return edges;   // GCOVR_EXCL_LINE
        }
        // Split the audible range so that every group covers approximately
        // the same number of barks: low frequencies get more bands. The
        // boundary is a float so that groups near the top of the bark scale
        // (where the scale saturates) do not collapse onto each other.
        const double bark_per_group =
                static_cast<double>(max_bark) / static_cast<double>(groups);
        size_t group = 0;
        for (size_t k = 0; k < bins; ++k) {
            const size_t b = bark_of_bin(k);
            while (group < groups - 1 &&               // GCOVR_EXCL_LINE
                   b > (group + 1) * bark_per_group)   // GCOVR_EXCL_LINE
                ++group;
            edges[group + 1] = k + 1;
        }
        edges[groups] = bins;
        // Make the edges strictly increasing and within [0, bins].
        for (size_t g = 1; g <= groups; ++g) {
            if (edges[g] < edges[g - 1])
                edges[g] = edges[g - 1];
            if (edges[g] > bins)   // GCOVR_EXCL_BR_LINE
                edges[g] = bins;   // GCOVR_EXCL_LINE
        }
        for (size_t g = 0; g < groups; ++g) {
            if (edges[g] >= edges[g + 1])
                edges[g + 1] = std::min(edges[g] + 1, bins);
        }
        return edges;
    }

    T &dac_;
    std::function<void(const std::array<DataType, callback_size> &)> callback_;
    spiram_vector<float> fft_buf_;
};

/**
 * Ready-made sink for a microphone ADC (e.g. an es7210) whose recording is
 * only meant to be analyzed: it fulfills the "DAC" interface sound_driven
 * expects (mclk_multiple, get_sample_rate() and write()) by forwarding the
 * sample rate to the wrapped ADC and discarding every chunk written to it.
 *
 * The caller still drives the recording (e.g. through es7210::read()) and
 * feeds the chunks to sound_driven::write(); the ADC is only referenced for
 * its configuration. See the sound_driven mic_vu_meter example.
 *
 * write() never looks at the samples, so it accepts any container whose
 * element type can be any width (16- or 32-bit recordings alike) and only
 * reports back the sample count.
 *
 * @tparam Adc  ADC type (e.g. es7210<...>): it must provide mclk_multiple and
 *              get_sample_rate() (i.e. it must be a sound_driven sink itself)
 *              and it must outlive the sink.
 */
template<typename Adc>
struct adc_sink
{
    static constexpr auto mclk_multiple = Adc::mclk_multiple;
    Adc &adc;

    // GCOVR_EXCL_BR_START: the only branches gcov reports in the two trivial
    // forwarders below are the never-taken exception arcs of the calls.
    [[nodiscard]] uint32_t get_sample_rate() const noexcept
    {
        return adc.get_sample_rate();
    }

    // The sample type is deduced from the container so a 32-bit recording
    // (es7210<..., true>) can be fed to sound_driven<adc_sink<...>>::write()
    // without a narrowing conversion; the samples are discarded.
    template<typename Buf>
    std::size_t write(const Buf &data, uint8_t = 2, TickType_t = portMAX_DELAY) noexcept
    {
        return data.size();
    }
    // GCOVR_EXCL_BR_STOP
};
