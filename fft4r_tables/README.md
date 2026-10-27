# fft4r_tables

Header-only C++ singleton that owns the esp-dsp radix-4 FFT twiddle tables.

The tables are sized for `CONFIG_DSP_MAX_FFT_SIZE` (the largest possible FFT)
and are initialized on first use. They can be released again with `reset()`
to free their memory when no FFT is expected for a while (a phoenix
singleton: the next `instance()` call re-initializes them). esp-dsp keeps
global twiddle tables with non-atomic init flags, so the users of this class
must only access it from a single task. A foreign initialization (somebody
else's buffer or a smaller table) is adopted, not owned, so `reset()` and the
destructor leave it alone; `usable()` verifies the live esp-dsp state and the
table size, and accepts a radix-4 length (a power of four) only, instead of
trusting the state recorded at initialization time.

The complete API is documented in
[`include/fft4r_tables.hpp`](include/fft4r_tables.hpp).

## Requirements

- ESP-IDF >= 6.0
- `esp-dsp`

## Example

There is no example project for this component. See the `sound_driven`
component (`components/sound_driven/`) for a real-world user: its spectral
path verifies the table with `usable()` before running an FFT.

## License

Apache-2.0
