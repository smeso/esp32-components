# xoroshiro

Header-only C++ implementation of the xoroshiro128++ pseudo-random number
generator.

The generator satisfies the C++ `UniformRandomBitGenerator` requirements
(`result_type`, `min()`, `max()` and `operator()`) and can be used directly
with the standard library distributions. The complete API is documented in
[`include/xoroshiro.hpp`](include/xoroshiro.hpp).

**Not cryptographically secure**: xoroshiro128++ is a fast statistical PRNG.
Observing a few consecutive outputs is enough to recover the internal 128-bit
state, after which all past and future outputs become predictable. Do not use
it for tokens, nonces, keys or anything else security-relevant.

**Not thread-safe**: an instance must not be shared between tasks without
external synchronization. Give each task its own instance instead.

Reference: <https://prng.di.unimi.it/xoroshiro128plusplus.c> (David Blackman
and Sebastiano Vigna).

## Example

`examples/random_walk` runs a two-dimensional Gaussian random walk: every
step is drawn with `std::normal_distribution` fed by `xoroshiro128pp`,
which only needs the `UniformRandomBitGenerator` interface the generator
provides. The walk's drift over a fixed number of steps is printed to show
that the output sequence is sane. The example only needs the `xoroshiro`
component itself: the generator is seeded from the target's hardware RNG
(`esp_random`, part of the common `esp_hw_support` dependency), so it does
not depend on any other component.

## Requirements

- ESP-IDF >= 6.0
- C++20 or later (the header uses `std::rotl` from `<bit>`); the consuming
  project must set `CMAKE_CXX_STANDARD` to 20 or higher

## License

Apache-2.0
