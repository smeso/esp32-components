# Components

This directory holds some ESP-IDF components. Two kinds live here:
first-party components maintained in this repository, and third-party
components that are vendored (pinned snapshots of upstream projects, updated
with the `fetch_*.sh` scripts in `utils/`).

This project is still WIP.

## First-party components

| Component | Description |
| --- | --- |
| `button` | Header-only C++ driver for a debounced push-button on a GPIO: press, release and multiple long-press callbacks, with an optional `task_queue` for running callbacks outside the timer context. Ships unit tests (`test/`) and an example (`examples/press_counter`). See its own [`button/README.md`](button/README.md). |
| `cpptaskcreate` | Header-only C++ drop-in replacement for `xTaskCreate()` that accepts any C++ callable instead of a plain function pointer, with a trampoline that contains exceptions and aborts on task-body exit. Ships unit tests (`test/`) and an example (`examples/counter_task`). See its own [`cpptaskcreate/README.md`](cpptaskcreate/README.md). |
| `es7210` | Header-only C++ driver for the ES7210 quad-channel ADC with I2S output (I2C control, I2S audio). Ships unit tests (`test/`) and an example (`examples/record_audio`). See its own [`es7210/README.md`](es7210/README.md). |
| `es8311` | Header-only C++ driver for the ES8311 low-power audio codec (I2C control, I2S audio). Ships unit tests (`test/`) and an example (`examples/single_tune`). See its own [`es8311/README.md`](es8311/README.md). |
| `fft4r_tables` | Header-only C++ resettable singleton that owns the esp-dsp radix-4 FFT twiddle tables, initializing them on first use (and again after `reset()`), letting users free them when unneeded and verify their size before running an FFT. Ships unit tests (`test/`). See its own [`fft4r_tables/README.md`](fft4r_tables/README.md). |
| `leds_effects` | Header-only C++ LED effects for addressable RGB strips: rotating, bouncing, smooth transitions, sun, breathing and deep-color effects with optional stop flags, plus a `transition_task` background task that fades the strip to posted colors. Ships unit tests (`test/`) and an example (`examples/effects_show`). See its own [`leds_effects/README.md`](leds_effects/README.md). |
| `mic_utils` | Header-only C++ helpers for microphone/silence detection on interleaved audio buffers. Ships unit tests (`test/`) and an example (`examples/detect_silence`). See its own [`mic_utils/README.md`](mic_utils/README.md). |
| `pcf85063a` | Header-only C++ driver for the NXP PCF85063A real-time clock (RTC) over I2C. Ships unit tests (`test/`) and an example (`examples/rtc_datetime`). See its own [`pcf85063a/README.md`](pcf85063a/README.md). |
| `sound_driven` | Header-only C++ helper that wraps a DAC and reports the amplitude (peak or bark-scale spectral) of every audio buffer written to it. Ships unit tests (`test/`) and examples (`examples/audio_meter`, `examples/mic_vu_meter`). See its own [`sound_driven/README.md`](sound_driven/README.md). |
| `spiram_allocation` | Header-only C++ helpers to allocate memory in SPI RAM: a STL-compatible `SpiramAllocator<T>` and `spiram_vector<T>`. Ships unit tests (`test/`) and an example (`examples/spiram_vector`). See its own [`spiram_allocation/README.md`](spiram_allocation/README.md). |
| `task_queue` | Header-only C++ generic task queue: invocable items (e.g. `std::function<void()>` closures) are posted to a FreeRTOS queue and run on a dedicated worker task, so posted work never runs in the caller's context. Ships unit tests (`test/`) and an example (`examples/worker_pool`). See its own [`task_queue/README.md`](task_queue/README.md). |
| `tca9555` | Header-only C++ driver for the TCA9555 16-bit I2C GPIO expander, plus an interrupt-driven button manager for all 16 pins. Ships unit tests (`test/`) and an example (`examples/gpio_blink`). See its own [`tca9555/README.md`](tca9555/README.md). |
| `ws2812` | Header-only C++ driver for WS2812 addressable RGB LED strips over the RMT peripheral. Ships unit tests (`test/`) and an example (`examples/color_cycle`). See its own [`ws2812/README.md`](ws2812/README.md). |
| `xoroshiro` | Header-only C++ xoroshiro128++ pseudo-random number generator with a built-in self-test. Ships unit tests (`test/`) and an example (`examples/random_walk`). See its own [`xoroshiro/README.md`](xoroshiro/README.md). |
| `utils` | Developer/CI tooling and shared test infrastructure. Not an ESP-IDF component itself (no `CMakeLists.txt` at its root); see [Tools in `utils/`](#tools-in-utils) below. |

## Vendored components

These are snapshots of upstream projects, pinned to a specific version and
tracked in this repository. Do not edit them directly; update them with the
corresponding `fetch_*.sh` script and commit the result. The pinned versions
are checked against upstream by `utils/check_upstream_updates.sh`.

| Component | Upstream | Updated with |
| --- | --- | --- |
| `doctest` | [doctest/doctest](https://github.com/doctest/doctest) | `utils/fetch_doctest.sh` |
| `esp-dsp` | [espressif/esp-dsp](https://github.com/espressif/esp-dsp) (git subtree) | `utils/fetch_esp_dsp.sh` |
| `esp-mqtt` | [espressif/esp-mqtt](https://github.com/espressif/esp-mqtt) | `utils/fetch_esp_mqtt.sh` |
| `esp_gcov` | [espressif/idf-extra-components](https://github.com/espressif/idf-extra-components) (`esp_gcov/` subdir) | `utils/fetch_esp_gcov.sh` |

- `doctest` is the header-only doctest test framework, wrapped in a small
  interface component (`CMakeLists.txt`) so test code can pull it in with
  `REQUIRES doctest`.
- `esp-dsp` is Espressif's DSP library, vendored as a git subtree.
- `esp-mqtt` is Espressif's MQTT client library supporting MQTT 3.1.1 and 5.0
  over multiple transports, used by the MQTT C++ wrapper in `mqtt`.
- `esp_gcov` is the Gcov source-code-coverage runtime for ESP-IDF, used by the
  coverage-enabled test runs.

## Tools in `utils/`

All scripts live in `utils/` and are run from anywhere in the repository.
Scripts that build or run the firmware take
`TARGET=<esp32|esp32c3|esp32s3|...>`, and `ROOT`/`TEST_APP` default to the
repository root and `utils/test_app` respectively.

### Formatting and static checks

| Tool | What it does |
| --- | --- |
| `format.sh` | Formats C/C++ (clang-format), Python (ruff) and shell (shfmt) files in place; `--check` only verifies. |
| `check_clang_tidy.sh` | Builds `utils/test_app/` with the clang toolchain and runs clang-tidy over the resulting compile database. |

### Project and CI orchestration

| Tool | What it does |
| --- | --- |
| `check_fast.sh` | Runs the fast local CI checks: format check, script checks (ruff, shellcheck, cmake-lint), clang-tidy, and the doctest suites on the Linux simulator. `TARGET=` is optional; when unset, clang-tidy is skipped with a warning. |
| `check_project.sh` | Full check of one ESP-IDF project: static checks (clang-tidy, cmake-lint, ruff, shellcheck, format) followed by a build and a QEMU run that verifies the firmware prints to stdout. |
| `ci.sh` | Full local CI pipeline for one target: format check, script checks, clang-tidy, example checks, and component tests. |
| `ci_all.sh` | Runs `ci.sh` for every supported target (esp32s3, esp32c3, esp32). |
| `check_upstream_updates.sh` | Compares the pinned vendored versions (doctest, esp-dsp, esp-mqtt, esp_gcov, esp-idf) against upstream and exits 1 if any are outdated. |

### Test execution

| Tool | What it does |
| --- | --- |
| `check_tests.sh` | Runs the QEMU test suites (doctest and coverage) for every component that ships a `test/` directory, skipping the vendored components and `utils/`. |
| `check_tests_device.sh` | Same as `check_tests.sh`, but flashes the test app to a real device and runs the suites over the serial port (`PORT=` required). |
| `test_jtag.sh` | Verifies the JTAG connection to a real device by issuing read/write checks through OpenOCD's telnet server. |

### Vendoring

| Tool | What it does |
| --- | --- |
| `fetch_doctest.sh <commit>` | Downloads `doctest.h` at the given commit into `doctest/doctest/`. |
| `fetch_esp_dsp.sh <commit>` | Pulls the given commit of espressif/esp-dsp into `esp-dsp/` as a git subtree. |
| `fetch_esp_gcov.sh <commit>` | Copies the `esp_gcov/` subdirectory at the given commit from espressif/idf-extra-components into `esp_gcov/`. |
| `fetch_esp_mqtt.sh <commit>` | Copies the given commit of espressif/esp-mqtt into `esp-mqtt/`. |

### `utils/` layout

The subdirectories hold supporting code used by the tools above (their
contents are not standalone tools):

| Directory | Purpose |
| --- | --- |
| `mocks/` | Mocks to allow tests to run without real hardware. |
| `plumbing/` | Helper scripts invoked by the tools above. |
| `pytest/` | Shared pytest configuration (`pytest.ini`) and entry points (`pytest_test_app.py`, `pytest_generic_app.py`). |
| `test_app/` | A shared ESP-IDF test application used by all `run-*`/`check_tests*` scripts; it auto-discovers components with a `test/` directory (selectable with `-DTEST_COMPONENTS=...`) and runs their doctest suites on the target. |

## License

Copyright 2026 Salvatore Mesoraca and contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

   https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
