# sound_driven

Header-only C++ helper that wraps a DAC (e.g. an `es8311`) and reports the
amplitude of every buffer written to it, so that e.g. a LED can be dimmed
from 0 to 100% following the music. It can also be used to pilot any sort of
actuator.

With `SpectralGroups == 1` the callback receives one value per output
channel: the peak amplitude of that channel in the buffer. With
`SpectralGroups > 1` the callback receives `OutputChannels * SpectralGroups`
values: for every output channel the audible spectrum of the buffer is
computed with an FFT (the fastest optimized FFT available in esp-dsp: the
radix-4 implementation) and the (scaled) magnitude of each of the
`SpectralGroups` frequency bands is reported. The bands follow the bark scale
of human hearing, so low frequencies get proportionally more bands than high
ones.

For recordings that are only analyzed (e.g. a microphone ADC like the
`es7210`) the header also provides `adc_sink<Adc>`: a ready-made sink that
forwards the ADC's sample rate and discards every chunk written to it, so a
`sound_driven` instance can be pointed at the ADC without copying the sink
boilerplate.

The complete API is documented in
[`include/sound_driven.hpp`](include/sound_driven.hpp).

## Memory placement

The (potentially large) FFT scratch buffer is heap-allocated in SPI RAM
through the sibling `spiram_allocation` component, so a `sound_driven`
instance stays small enough to live on the caller's stack.

## Requirements

- ESP-IDF >= 6.0
- `esp-dsp` (for the FFT used by the spectral path, and for the twiddle
  tables owned by `fft4r_tables`)
- `fft4r_tables` (owns the esp-dsp radix-4 FFT twiddle tables)
- `spiram_allocation` (the FFT scratch buffer is kept in SPI RAM)

## Example

`examples/audio_meter` generates a tone in memory and feeds it to a
`sound_driven` instance backed by a software DAC, printing the per-band
amplitudes to the log. The example only needs the `sound_driven` component
itself (and the components it requires); it does not depend on any other
component.

`examples/mic_vu_meter` records audio from an ES7210 microphone ADC with the
`es7210` component and pilots a WS2812 LED strip (`ws2812`) from the
bark-scale spectral amplitudes computed by a `sound_driven` instance, using
`leds_effects` to calibrate the LED colors: the LEDs follow the music picked
up by the microphones in real time.

## License

Apache-2.0
