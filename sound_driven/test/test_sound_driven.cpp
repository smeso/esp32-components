/*
 * SPDX-FileCopyrightText: 2025 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <doctest/doctest.h>

#include <freertos/FreeRTOS.h>

#include "sound_driven.hpp"

namespace {

// Software DAC sink: sound_driven only needs mclk_multiple, get_sample_rate()
// and write(). Every chunk handed to it is recorded so tests can check the
// chunking and the channel/timeout forwarding.
struct fake_dac
{
    static constexpr auto mclk_multiple = 256;
    // Only the first samples handed to the sink are kept: the chunking tests
    // inspect the contents of the first few chunks, while the detector tests
    // write hundreds of buffers whose samples would otherwise exhaust the
    // small heap of the chip targets (each recorded chunk is a 4 KB copy).
    // The slot and the per-chunk metadata are always recorded, so counting
    // the chunks still works whatever the budget is.
    static constexpr size_t max_recorded_samples = 8192;
    std::vector<std::vector<int16_t>> chunks;
    std::vector<uint8_t> channel_counts;
    std::vector<TickType_t> timeouts;
    uint32_t sample_rate;
    // Partial-write emulation: when > 0, write() reports only this many
    // samples of the chunk as accepted (the tail is dropped).
    size_t partial_write = 0;

    explicit fake_dac(uint32_t rate = 44100)
            : sample_rate(rate)
    {
    }

    [[nodiscard]] uint32_t get_sample_rate() const noexcept
    {
        return sample_rate;
    }

    size_t write(std::span<const int16_t> data,
                 uint8_t channels = 2,
                 TickType_t timeout = portMAX_DELAY)
    {
        if (recorded_samples_ < max_recorded_samples) {
            chunks.emplace_back(data.begin(), data.end());
            recorded_samples_ += data.size();
        } else {
            chunks.emplace_back();
        }
        channel_counts.push_back(channels);
        timeouts.push_back(timeout);
        if (partial_write > 0 && partial_write < data.size())
            return partial_write;
        return data.size();
    }

  private:
    size_t recorded_samples_ = 0;
};

// Stand-in microphone ADC for the adc_sink tests: adc_sink only needs
// mclk_multiple and get_sample_rate() (it never records anything, the
// recording is driven by the caller).
struct fake_adc
{
    static constexpr auto mclk_multiple = 512;
    uint32_t sample_rate;

    explicit fake_adc(uint32_t rate)
            : sample_rate(rate)
    {
    }

    [[nodiscard]] uint32_t get_sample_rate() const noexcept
    {
        return sample_rate;
    }
};

template<std::size_t N>
using amplitudes_t = std::array<uint16_t, N>;

// Records every callback invocation (including the zero report emitted by
// the destructor), so tests can verify both the computed amplitudes and the
// end-of-life notification.
template<std::size_t N>
struct callback_log
{
    std::vector<amplitudes_t<N>> calls;

    void operator()(amplitudes_t<N> amps)
    {
        calls.push_back(amps);
    }
};

template<std::size_t OutputChannels = 1, std::size_t SpectralGroups = 1>
using under_test = sound_driven<fake_dac, OutputChannels, SpectralGroups, uint16_t>;

template<typename SD>
SD
make_sound_driven(fake_dac &dac, callback_log<SD::callback_size> &log)
{
    return SD(dac,
              [&log](amplitudes_t<SD::callback_size> amps) { log.calls.push_back(amps); });
}

// Interleaved stereo buffer with `frames` frames of a sine wave at
// `frequency` Hz and the given peak amplitude.
std::vector<int16_t>
sine_buffer(uint32_t sample_rate, double frequency, size_t frames, int16_t amplitude)
{
    std::vector<int16_t> buf(frames * 2);
    const double step = 2.0 * M_PI * frequency / static_cast<double>(sample_rate);
    for (size_t i = 0; i < frames; ++i) {
        const int16_t v = static_cast<int16_t>(static_cast<double>(amplitude) *
                                               std::sin(step * static_cast<double>(i)));
        buf[2 * i] = v;
        buf[2 * i + 1] = v;
    }
    return buf;
}

// Interleaved stereo buffer with `frames` frames of a constant sample value.
std::vector<int16_t>
constant_buffer(int16_t value, size_t frames)
{
    return std::vector<int16_t>(frames * 2, value);
}

// A full-scale sine sitting exactly on bin 2 of a `frames`-point FFT, so that
// the band that receives the tone saturates whatever the FFT size is.
std::vector<int16_t>
bin_two_sine(size_t frames)
{
    return sine_buffer(44100, 2.0 * 44100.0 / static_cast<double>(frames), frames, 32767);
}

// The largest amplitude reported for a write.
template<std::size_t N>
uint16_t
largest_band(const amplitudes_t<N> &amps)
{
    return *std::max_element(amps.begin(), amps.end());
}

// Restores esp-dsp's global twiddle table size on scope exit. A manual restore
// at the end of the test body would be skipped when an assertion in between
// throws (doctest's FAIL and REQUIRE abort the test case), leaving the
// process-global size corrupted for every later test case.
class scoped_w_table_size
{
  public:
    explicit scoped_w_table_size(int corrupt_to)
            : saved_(dsps_fft4r_w_table_size)
    {
        dsps_fft4r_w_table_size = corrupt_to;
    }
    ~scoped_w_table_size()
    {
        dsps_fft4r_w_table_size = saved_;
    }
    scoped_w_table_size(const scoped_w_table_size &) = delete;
    scoped_w_table_size &operator=(const scoped_w_table_size &) = delete;

  private:
    int saved_;
};

}   // namespace

TEST_CASE("template parameters are reflected in the helper")
{
    static_assert(under_test<>::mclk_multiple == 256);
    static_assert(under_test<>::callback_size == 1);
    static_assert(under_test<2, 7>::callback_size == 14);
    CHECK(under_test<>::callback_size == 1);
    CHECK(under_test<2, 7>::callback_size == 14);
}

TEST_CASE("degenerate template parameters are rejected at compile time")
{
    // OutputChannels == 0 produces an empty callback array and fires the
    // class's static_assert; the fact that this test case compiles proves
    // the assertion does not reject the valid instantiations used above.
    CHECK(under_test<>::callback_size == 1);
}

TEST_CASE("sound_driven is movable but not copyable")
{
    static_assert(!std::is_copy_constructible_v<under_test<>>);
    static_assert(!std::is_copy_assignable_v<under_test<>>);
    static_assert(std::is_move_constructible_v<under_test<>>);
    // The DAC is held by reference, so a moved-from instance cannot be
    // assigned to again.
    static_assert(!std::is_move_assignable_v<under_test<>>);
    // The move constructor resets the source's callback: its destructor must
    // never fire the spurious notification that a moved std::function could
    // still hold.
    static_assert(noexcept(under_test<>(std::declval<under_test<> &&>())));
}

TEST_CASE("destructor reports zero amplitudes and moved-from instances are safe")
{
    fake_dac dac;
    callback_log<1> log;
    {
        auto sd = make_sound_driven<under_test<>>(dac, log);
        CHECK(log.calls.empty());
        // The moved-from instance has an empty callback: its destructor must
        // not try to call it.
        auto moved = std::move(sd);
        (void) moved;
    }
    REQUIRE(log.calls.size() == 1);
    CHECK(log.calls[0] == amplitudes_t<1>{ 0 });
}

TEST_CASE("get_sample_rate forwards to the DAC")
{
    fake_dac dac(48000);
    callback_log<1> log;
    {
        auto sd = make_sound_driven<under_test<>>(dac, log);
        CHECK(sd.get_sample_rate() == 48000);
    }
}

TEST_CASE("adc_sink forwards the sample rate and drops the samples")
{
    static_assert(adc_sink<fake_adc>::mclk_multiple == fake_adc::mclk_multiple);
    fake_adc adc(16000);
    adc_sink<fake_adc> sink{ adc };
    CHECK(sink.get_sample_rate() == 16000);
    const std::vector<int16_t> buf(100, -1234);
    CHECK(sink.write(buf) == buf.size());

    // The sink's write() is a template over the sample type, so a 32-bit
    // recording (es7210<..., true>) is accepted as well.
    const std::vector<int32_t> buf32(10, -1234);
    CHECK(sink.write(buf32) == buf32.size());

    // sound_driven accepts the sink as its DAC and reports the ADC's sample
    // rate, while the samples are silently discarded.
    sound_driven<adc_sink<fake_adc>> sd(sink, [](amplitudes_t<1>) {});
    CHECK(sd.get_sample_rate() == 16000);
    CHECK(sd.write(buf) == buf.size());
}

TEST_CASE("write splits large buffers into chunks and forwards them")
{
    fake_dac dac(44100);   // 44100 Hz -> chunks of 1024 frames = 2048 samples
    callback_log<1> log;
    std::vector<int16_t> buf(5000);
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<int16_t>(i % 1000 - 500);
    {
        auto sd = make_sound_driven<under_test<>>(dac, log);
        const size_t written = sd.write(buf);
        CHECK(written == buf.size());
    }
    REQUIRE(dac.chunks.size() == 3);
    CHECK(dac.chunks[0].size() == 2048);
    CHECK(dac.chunks[1].size() == 2048);
    CHECK(dac.chunks[2].size() == 904);
    // The chunks contain the original samples in order.
    for (size_t c = 0; c < dac.chunks.size(); ++c) {
        const auto &chunk = dac.chunks[c];
        for (size_t i = 0; i < chunk.size(); ++i)
            CHECK(chunk[i] == buf[c * 2048 + i]);
    }
    // One callback per chunk (the fourth entry is the destructor's zero
    // report).
    REQUIRE(log.calls.size() == 4);
    CHECK(log.calls[3] == amplitudes_t<1>{ 0 });
}

TEST_CASE(
        "write uses the lower chunk size for low sample rates and forwards channels and timeout")
{
    fake_dac dac(8000);   // 8000 Hz -> chunks of 256 frames = 512 samples
    callback_log<1> log;
    std::vector<int16_t> buf(600, 1);
    {
        auto sd = make_sound_driven<under_test<>>(dac, log);
        (void) sd.write(buf, 2, 100);
    }
    REQUIRE(dac.chunks.size() == 2);
    CHECK(dac.chunks[0].size() == 512);
    CHECK(dac.chunks[1].size() == 88);
    CHECK(dac.channel_counts == std::vector<uint8_t>{ 2, 2 });
    CHECK(dac.timeouts == std::vector<TickType_t>{ 100, 100 });
}

TEST_CASE("writing an empty buffer does not call the callback")
{
    fake_dac dac;
    callback_log<1> log;
    {
        auto sd = make_sound_driven<under_test<>>(dac, log);
        CHECK(sd.write(std::vector<int16_t>{}) == 0);
        CHECK(dac.chunks.empty());
        CHECK(log.calls.empty());
    }
    // Only the destructor's zero report.
    CHECK(log.calls.size() == 1);
}

TEST_CASE("peak amplitudes scale the absolute sample peak")
{
    fake_dac dac;
    callback_log<1> log;
    std::vector<int16_t> buf{ 1000, -2000, 3000, -4000 };
    {
        auto sd = make_sound_driven<under_test<>>(dac, log);
        (void) sd.write(buf);
    }
    // Peak 4000 of full scale 32767 -> 4000 * 65535 / 32767 = 8000.
    REQUIRE(log.calls.size() == 2);
    CHECK(log.calls[0][0] == 8000);
    CHECK(log.calls[1] == amplitudes_t<1>{ 0 });
}

TEST_CASE("a full-scale negative sample counts as the peak")
{
    fake_dac dac;
    callback_log<1> log;
    // The wrapped -(-32768) would fail the peak comparison and report 0.
    std::vector<int16_t> buf{ -32768, -32768, -32768, -32768 };
    {
        auto sd = make_sound_driven<under_test<>>(dac, log);
        (void) sd.write(buf, 1);
    }
    REQUIRE(log.calls.size() == 2);
    CHECK(log.calls[0][0] == 65535);
}

TEST_CASE("write rejects a zero channel count without touching the DAC")
{
    fake_dac dac;
    callback_log<1> log;
    {
        auto sd = make_sound_driven<under_test<>>(dac, log);
        const std::vector<int16_t> buf{ 100, -200, 300 };
        // A runtime value keeps the `channels == 0` branch in the compiled
        // copy: a literal 0 is folded away at the call site by constprop.
        volatile uint8_t zero_channels = 0;
        CHECK(sd.write(buf, zero_channels) == 0);
        // The empty buffer is also a no-op.
        CHECK(sd.write(std::vector<int16_t>{}) == 0);
    }
    CHECK(dac.chunks.empty());
    // No chunk was written, so only the destructor's zero report remains.
    REQUIRE(log.calls.size() == 1);
    CHECK(log.calls[0] == amplitudes_t<1>{ 0 });
}

TEST_CASE("peak amplitudes for multiple output channels")
{
    fake_dac dac;
    callback_log<2> log;
    // 4 interleaved channels: output channel 0 sees channels 0 and 2,
    // output channel 1 sees channels 1 and 3.
    std::vector<int16_t> buf{ 1000, 2000, -3000, -4000, 5000, -6000, -7000, 8000 };
    {
        auto sd = make_sound_driven<under_test<2>>(dac, log);
        (void) sd.write(buf, 4);
    }
    REQUIRE(log.calls.size() == 2);
    CHECK(log.calls[0][0] == 14000);   // peak |7000| -> 14000
    CHECK(log.calls[0][1] == 16000);   // peak |8000| -> 16000
    CHECK(log.calls[1] == amplitudes_t<2>{ 0, 0 });
}

TEST_CASE("peak amplitudes handle more input channels than output channels")
{
    fake_dac dac;
    callback_log<2> log;
    // 255 interleaved channels with OutputChannels == 2: the channel stride
    // reaches the last (odd) channel. An 8-bit stride counter would wrap
    // 254 + 2 back to 0 and loop forever on the even offset (and re-read
    // channel 0 on the odd one).
    std::vector<int16_t> buf(255, 0);
    buf[0] = 1000;     // channel 0: seen by output channel 0
    buf[1] = 2000;     // channel 1: seen by output channel 1
    buf[254] = 3000;   // channel 254: seen by output channel 0
    {
        auto sd = make_sound_driven<under_test<2>>(dac, log);
        (void) sd.write(buf, 255);
    }
    REQUIRE(log.calls.size() == 2);
    CHECK(log.calls[0][0] == 6000);   // peak |3000| -> 6000
    CHECK(log.calls[0][1] == 4000);   // peak |2000| -> 4000
    CHECK(log.calls[1] == amplitudes_t<2>{ 0, 0 });
}

TEST_CASE("peak amplitudes for mono buffers")
{
    fake_dac dac;
    callback_log<1> log;
    std::vector<int16_t> buf{ 1000, -2000, 3000, -4000 };
    {
        auto sd = make_sound_driven<under_test<>>(dac, log);
        (void) sd.write(buf, 1);
    }
    REQUIRE(log.calls.size() == 2);
    CHECK(log.calls[0][0] == 8000);
}

TEST_CASE("write accepts spans and vectors")
{
    fake_dac dac;
    callback_log<1> log;
    std::vector<int16_t> data{ 1, -1, 2, -2 };
    {
        auto sd = make_sound_driven<under_test<>>(dac, log);
        (void) sd.write(std::span<const int16_t>{ data });
    }
    // Peak 2 -> 2 * 65535 / 32767 = 4.
    REQUIRE(log.calls.size() == 2);
    CHECK(log.calls[0][0] == 4);
}

TEST_CASE("spectral path removes the DC offset")
{
    fake_dac dac(44100);
    callback_log<7> log;
    {
        auto sd = make_sound_driven<under_test<1, 7>>(dac, log);
        (void) sd.write(constant_buffer(32767, 1024));
        (void) sd.write(constant_buffer(-32768, 1024));
    }
    REQUIRE(log.calls.size() == 3);
    // A constant buffer is pure DC: it is removed before the FFT, so no band
    // carries any power.
    for (size_t g = 0; g < 7; ++g)
        CHECK(log.calls[0][g] <= 1);
    for (size_t g = 0; g < 7; ++g)
        CHECK(log.calls[1][g] <= 1);
    CHECK(log.calls[2] == amplitudes_t<7>{});
}

TEST_CASE("spectral path clamps a full-scale low-frequency sine to the data maximum")
{
    fake_dac dac(44100);
    callback_log<7> log;
    {
        auto sd = make_sound_driven<under_test<1, 7>>(dac, log);
        // 43.07 Hz is exactly bin 1 of the 1024-point FFT at 44100 Hz: a
        // full-scale sine on a single bin must saturate its band (the Hann
        // window coherent gain is compensated).
        (void) sd.write(sine_buffer(44100, 44100.0 / 1024.0, 1024, 32767));
    }
    REQUIRE(log.calls.size() == 2);
    // Bin 1 falls in the first (low-frequency) band.
    CHECK(log.calls[0][0] == 65535);
    // A single-bin sine has no power in the other bands.
    for (size_t g = 1; g < 7; ++g)
        CHECK(log.calls[0][g] <= 1);
    CHECK(log.calls[1] == amplitudes_t<7>{});
}

TEST_CASE("spectral path concentrates a sine in the low bands")
{
    fake_dac dac(44100);
    callback_log<7> log;
    const std::vector<int16_t> buf = sine_buffer(44100, 440.0, 1024, 30000);
    {
        auto sd = make_sound_driven<under_test<1, 7>>(dac, log);
        (void) sd.write(buf);
    }
    REQUIRE(log.calls.size() == 2);
    uint16_t total = 0;
    uint16_t max_amp = 0;
    for (size_t g = 0; g < 7; ++g) {
        total = static_cast<uint16_t>(total + log.calls[0][g]);
        max_amp = std::max(max_amp, log.calls[0][g]);
    }
    CHECK(total > 0);
    CHECK(max_amp > 1000);
    CHECK(max_amp <= 65535);
}

TEST_CASE("spectral path returns zeros for buffers too short for the FFT")
{
    fake_dac dac(44100);
    callback_log<7> log;
    {
        auto sd = make_sound_driven<under_test<1, 7>>(dac, log);
        // 1 sample: frames = 0 -> no spectrum.
        (void) sd.write(std::vector<int16_t>{ 100 });
        // 1 frame: frames = 1 -> fft_size = 1 < 16.
        (void) sd.write(std::vector<int16_t>{ 100, -100 });
        // 8 frames: frames = 8 -> fft_size = 4 < 16.
        (void) sd.write(constant_buffer(32767, 8));
    }
    REQUIRE(log.calls.size() == 4);
    CHECK(log.calls[0] == amplitudes_t<7>{});
    CHECK(log.calls[1] == amplitudes_t<7>{});
    CHECK(log.calls[2] == amplitudes_t<7>{});
    CHECK(log.calls[3] == amplitudes_t<7>{});
}

TEST_CASE("spectral path grows the FFT buffer when a larger FFT is needed")
{
    fake_dac dac(44100);
    callback_log<7> log;
    {
        auto sd = make_sound_driven<under_test<1, 7>>(dac, log);
        // 64 frames -> 64-point FFT -> 128-float FFT buffer.
        (void) sd.write(bin_two_sine(64));
        // 1024 frames -> 1024-point FFT -> 2048-float FFT buffer: the
        // internal buffer must grow between the two writes.
        (void) sd.write(bin_two_sine(1024));
    }
    REQUIRE(log.calls.size() == 3);
    CHECK(largest_band(log.calls[0]) == 65535);
    CHECK(largest_band(log.calls[1]) == 65535);
    CHECK(log.calls[2] == amplitudes_t<7>{});
}

TEST_CASE("spectral path reuses a large-enough FFT buffer")
{
    fake_dac dac(44100);
    callback_log<7> log;
    {
        auto sd = make_sound_driven<under_test<1, 7>>(dac, log);
        // 1024 frames -> 1024-point FFT -> 2048-float FFT buffer.
        (void) sd.write(bin_two_sine(1024));
        const size_t calls_after_grow = log.calls.size();
        // Smaller windows afterwards fit in the existing buffer, so the
        // spectral path must not reallocate (the writes still report the
        // correct saturated amplitude).
        (void) sd.write(bin_two_sine(16));
        (void) sd.write(bin_two_sine(64));
        (void) sd.write(bin_two_sine(256));
        REQUIRE(log.calls.size() == calls_after_grow + 3);
        for (size_t i = 0; i < 3; ++i)
            CHECK(largest_band(log.calls[calls_after_grow + i]) == 65535);
    }
    REQUIRE(log.calls.size() == 5);
    CHECK(log.calls[4] == amplitudes_t<7>{});
}

TEST_CASE("a partial DAC write is reported, not silently dropped")
{
    fake_dac dac(44100);
    callback_log<1> log;
    std::vector<int16_t> buf(600, 100);
    {
        auto sd = make_sound_driven<under_test<>>(dac, log);
        // The DAC only accepts the first 100 samples of every chunk.
        dac.partial_write = 100;
        const size_t written = sd.write(buf, 1);
        CHECK(written == 100);
    }
    // One (partial) chunk reached the DAC; one callback fired for it and the
    // second one is the destructor's zero report.
    REQUIRE(dac.chunks.size() == 1);
    REQUIRE(log.calls.size() == 2);
    CHECK(log.calls[1] == amplitudes_t<1>{ 0 });
}

TEST_CASE("bark band edges fall back to an even split for a very low sample rate")
{
    fake_dac dac(100);   // Nyquist 50 Hz: the whole bark scale is 0
    callback_log<12> log;
    {
        auto sd = make_sound_driven<under_test<1, 12>>(dac, log);
        // 16 frames -> 16-point FFT -> 9 bins, fewer than the 12 groups.
        (void) sd.write(constant_buffer(32767, 16));
    }
    REQUIRE(log.calls.size() == 2);
    for (size_t g = 0; g < 12; ++g)
        CHECK(log.calls[0][g] <= 65535);
}

TEST_CASE("bark band edges are clamped when there are more groups than bins")
{
    fake_dac dac(44100);
    callback_log<12> log;
    {
        auto sd = make_sound_driven<under_test<1, 12>>(dac, log);
        // 16 frames -> 16-point FFT -> 9 bins, fewer than the 12 groups.
        (void) sd.write(constant_buffer(32767, 16));
    }
    REQUIRE(log.calls.size() == 2);
    for (size_t g = 0; g < 12; ++g)
        CHECK(log.calls[0][g] <= 65535);
}

TEST_CASE("an instance with an empty callback is safe to write to")
{
    fake_dac dac;
    {
        under_test<> sd(dac, {});
        const std::vector<int16_t> buf{ 100, -200, 300, -400 };
        CHECK(sd.write(buf) == buf.size());
    }
    CHECK(dac.chunks.size() == 1);
}

TEST_CASE("sound_driven exception includes the message and the error name")
{
    // Call what() through a std::exception reference: a local object would
    // let the compiler devirtualize the call (and the out-of-line what()
    // would never run).
    under_test<>::exception plain("boom");
    const std::exception &plain_ref = plain;
    CHECK(std::strcmp(plain_ref.what(), "sound_driven: boom") == 0);

    under_test<>::exception with_err("fft failed", ESP_ERR_INVALID_ARG);
    const std::exception &with_err_ref = with_err;
    const std::string msg(with_err_ref.what());
    CHECK(msg.find("sound_driven: fft failed") == 0);
    CHECK(msg.find("ESP_ERR_INVALID_ARG") != std::string::npos);
}

TEST_CASE("exception derives from std::exception")
{
    static_assert(std::is_base_of_v<std::exception, under_test<>::exception>);
}

TEST_CASE("the spectral path throws when the FFT twiddle table is unusable")
{
    fake_dac dac(44100);
    callback_log<2> log;
    // Initialize the process-global twiddle table, then corrupt the recorded
    // table size so the defensive check in the spectral path fires. The size
    // is restored when the guard leaves scope: the table itself is untouched,
    // only the (otherwise global) size variable is briefly wrong.
    REQUIRE(dsps_fft4r_init_fc32(nullptr, CONFIG_DSP_MAX_FFT_SIZE) == ESP_OK);
    const scoped_w_table_size corrupt_size(0);
    try {
        auto sd = make_sound_driven<under_test<1, 2>>(dac, log);
        (void) sd.write(constant_buffer(32767, 1024));
        FAIL("write() should have thrown sound_driven::exception");
    } catch (const under_test<1, 2>::exception &e) {
        const std::string msg(e.what());
        CHECK(msg.find("FFT twiddle table unavailable") != std::string::npos);
    }
}

TEST_CASE("a throwing callback does not escape the destructor")
{
    fake_dac dac;
    {
        // A std::exception-derived failure is logged with its message.
        under_test<> sd(dac, [](amplitudes_t<1>) { throw std::runtime_error("boom"); });
    }
    {
        // A non-standard failure is logged without a message.
        under_test<> sd(dac, [](amplitudes_t<1>) { throw 42; });
    }
    // Reaching the end of the block proves the destructor swallowed both.
}
