# ES7210

C++ driver for the ES7210 high-performance, quad-channel ADC with I2S output.

The ADC is controlled over I2C, while audio data flows over I2S. The driver
is header-only; the complete API is documented in
[`include/es7210.hpp`](include/es7210.hpp).

## Requirements

- ESP-IDF >= 6.0
- `esp_driver_i2c`, `esp_driver_i2s`

## License

Apache-2.0
