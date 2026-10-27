# LEDS Effects

Header-only C++ LED effects for addressable RGB LED strips (e.g. WS2812
driven by the `ws2812` component).

The effects are templates over the strip type and only require the following
interface:

- `T::num_leds()` — number of LEDs in the strip,
- `T::RGB` — a color type with `r`, `g` and `b` members,
- `strip.set_leds(std::span<const T::RGB>)` — set the LEDs from a contiguous
  range of exactly `T::num_leds()` colors (a `std::array`, a `std::vector` or a
  `std::span` all convert implicitly),
- `strip.set_all_leds(T::RGB)` — set all the LEDs to the same color,
- `strip.all_off()` — turn every LED off.

All effects are blocking and drive the strip from the calling task; each one
accepts an optional `std::atomic<bool> *stop` flag that aborts the effect as
soon as it is set. The complete API is documented in
[`include/leds_effects.hpp`](include/leds_effects.hpp).

## transition_task

`leds_effects::transition_task<T>` is a non-blocking alternative to the
effects above: instantiating it spawns a dedicated FreeRTOS task (via
`cppTaskCreate()`) that owns the strip. The task starts with all LEDs off and,
on every color posted to it, fades the strip from the currently shown color to
the new one over a configurable transition time (using the same temporal
dithering transition engine as `smooth_transition()` and `deep_color()`; see
`DitherConfig`). After reaching the target the task keeps
dithering it until the next color is posted, so the perceived color stays
accurate instead of freezing on the 8-bit dither phase the fade ended on.
Colors are posted as 16-bit-per-channel calibrated targets (like the
`deep_color()` arguments) through `post_transition()`; there is also an
overload taking one 16-bit target color per LED.

The class also mirrors the `ws2812` interface: `set_all_leds()`, `set_leds()`,
`all_on()` and `all_off()` just post transitions (using the default transition
time passed to the constructor), and `num_leds()` forwards to the strip type.
Posts are safe from any task but not from an ISR (a command is heap-allocated
and the transition queue is locked). The destructor and `stop()` turn the LEDs
off again (dropping transitions that are still queued); `stop()` also deletes
the task, and `start()` re-creates it. A strip fault (the strip setters may
throw) is caught by the transition task and stops it instead of aborting the
firmware, so `start()` can bring it back.

`transition_task::park_sentinel_ms` is reserved for the internal park
sentinel, so passing it as a transition time (to `post_transition()`) or as
the constructor's default transition time throws
`transition_task::exception`.

The dithered transitions (`smooth_transition`, `breathing`, `sun_effect`,
`deep_color` and `transition_task`) render their sub-8-bit color detail with
temporal dithering, parameterized by a `DitherConfig`:

- `dither_hz` is the requested dither frame rate. A frame cannot be shorter
  than one FreeRTOS tick, so the usable rate is capped to the tick rate
  (`CONFIG_FREERTOS_HZ`).
- `flicker_hz` is the flicker-fusion frequency the dither cycle must reach
  (roughly 60-120 Hz for a human eye). A cycle that repeats slower than this
  is seen as flicker.

The two frequencies bound the number of dither levels, and with them the
effective bit depth the strip can render: `levels()` is
`frame_hz() / flicker_hz`, and `effective_bits()` is 8 plus one bit per
doubling of the renderable levels. Use `DitherConfig::effective_bits()` to
query the bit depth a configuration renders. As an example, with a tick rate
high enough for the requested 200 Hz dither, a 100 Hz fusion frequency renders
8 + log2(2) = 9 effective bits. A high tick rate (e.g.
`CONFIG_FREERTOS_HZ=1000`, as the main project uses) therefore buys deeper
colors; on a slow tick the dither is reduced to what the eye can actually
fuse. The effects always block for at least one tick between frames
(they clamp their frame delays instead of spinning on a zero-tick
`vTaskDelay()`), so they are safe on any tick rate.

The dither phases can also be spread over a group of neighboring LEDs
(spatial dithering) instead of relying on the temporal cycle alone:
`fade_all()` and its users (`smooth_transition`, `deep_color` and
`transition_task`) take a `spatial_leds` template argument selecting how many
LEDs share the group. Every LED of the group renders a different phase of the
cycle, so the renderable levels (and the effective bit depth) are multiplied
by `spatial_leds` — `effective_levels(spatial_leds)` is `levels() *
spatial_leds` and `effective_bits(spatial_leds)` counts the bits of that
product — without shortening the temporal cycle, so the flicker-fusion
frequency is not affected. The group repeats along the strip; `spatial_leds`
must not exceed the number of LEDs.

## Strip requirements

`T::RGB` must be constructible by brace initialization from three integers
(e.g. an aggregate like `struct RGB { uint8_t r, g, b; };`). The strip must
have at least one LED for `rotating` and the other effects, and at least two
for `bouncing`; the effects `static_assert` these contracts.

## Requirements

- ESP-IDF >= 6.0
- `freertos`
- `cpptaskcreate` (used by `transition_task`)

## Example

`examples/effects_show` cycles through the effects on a simulated strip: it
runs `rotating`, `bouncing`, `sun_effect`, `smooth_transition`, `breathing`
and `deep_color` in sequence, printing the state of the strip to the console.
The number of LEDs and the effect timings are configurable through Kconfig.
The example only needs the `leds_effects` component itself, so it does not
depend on any other component.

## Calibration

Real LEDs are not perfectly balanced: the three dies have different
strengths, perceived brightness is nonlinear, and only colors inside the
LED's gamut can be reproduced. To get accurate colors, calibrate the LED and
pass the result to `deep_color` through its `calibration` argument (a
pointer to a `LedCalibration`), and to `sun_effect` and `breathing` through
their `calibration` argument (a `LedCalibration`). In short:

1. Measure the black level - the tristimulus values with all channels off -
   and store them in `LedCalibration::black`.
2. Measure the three primaries at full drive (`255,0,0`, `0,255,0`,
   `0,0,255`), subtract the black level, and build the 3×3 mixing matrix
   `M` whose columns are these three measurements.
3. Invert `M` and store the inverse in `LedCalibration::inv_matrix`.
4. Optionally set `LedCalibration::gamma` (e.g. 2.2) so that evenly spaced
   targets look evenly bright.

`LedCalibration::drive()` then maps a target color to the gamma-encoded
drive values that reproduce it on your LED, clamping out-of-gamut channels
and rescaling to preserve hue (see
[`include/leds_effects.hpp`](include/leds_effects.hpp)). Keep `gamma` > 0
and the matrix/black data finite: non-finite or non-positive values are
guarded (they map to black / the identity encoding), but a sane calibration
is the caller's responsibility. `calibrate()` is a convenience wrapper
mapping an 8-bit RGB color through `drive()` (see
[`include/leds_effects.hpp`](include/leds_effects.hpp)).

## License

Apache-2.0
