# SPI RAM allocation

Header-only C++ helpers to allocate memory in SPI RAM (PSRAM):

- `SpiramAllocator<T>` is a standard-library-compatible allocator that
  allocates from the SPI RAM heap via `heap_caps_aligned_alloc()` with
  `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`, falling back to `std::bad_alloc` when
  no memory is available.
- `spiram_vector<T>` is `std::vector<T, SpiramAllocator<T>>`, a vector whose
  storage always lives in SPI RAM.

The complete API is documented in
[`include/spiram_allocation.hpp`](include/spiram_allocation.hpp).

## Requirements

- ESP-IDF >= 6.0
- C++ exceptions enabled (`CONFIG_COMPILER_CXX_EXCEPTIONS`): allocation
  failures are reported by throwing `std::bad_alloc`. Note that, when
  `CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS` is enabled, the ESP-IDF heap
  aborts before `std::bad_alloc` can be thrown.
- A chip with SPI RAM (`esp_psram`); on targets without PSRAM support the
  allocator still compiles but `spiram_vector` will throw `std::bad_alloc` at
  runtime.

## Example

`examples/spiram_vector` fills a `spiram_vector` histogram with pseudo-random
data and prints a summary proving that the vector's storage lives in external
RAM. The example only needs `esp_psram` and FreeRTOS, so it does not depend
on any other component.

## License

Apache-2.0
