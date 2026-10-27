# ES8311

C++ driver for the ES8311 low-power audio codec with integrated I2S interface.

The codec is controlled over I2C, while audio data flows over I2S. The driver
is header-only; the complete API is documented in
[`include/es8311.hpp`](include/es8311.hpp).

## Requirements

- ESP-IDF >= 6.0
- `esp_driver_i2c`, `esp_driver_i2s`

## License

Apache-2.0
