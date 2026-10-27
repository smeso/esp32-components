# mic_utils

Header-only C++ helpers for microphone/silence detection on interleaved audio
buffers.

`has_silence()` tells whether the most recent `seconds` of audio in an
interleaved buffer are silent, i.e. whether their RMS level, normalized to the
sample type's full scale, is at or below a configurable threshold. Only the
channels listed in `channels` contribute to the measurement, so the function
works both on mono/stereo buffers and on multi-channel (e.g. TDM) ones. The
complete API is documented in [`include/mic_utils.hpp`](include/mic_utils.hpp).

The accumulation type is a template parameter (`SumT`, `float` by default):
use `double` on targets with hardware double-precision math support.

## Requirements

- ESP-IDF >= 6.0

## Example

`examples/detect_silence` fills an in-memory buffer with a sine wave followed
by silence and reports the `has_silence()` result for the whole buffer and for
a sliding window, showing when the silent tail is detected. The example is
self-contained: it only needs the `mic_utils` component itself and does not
depend on any other component.

## License

Apache-2.0
