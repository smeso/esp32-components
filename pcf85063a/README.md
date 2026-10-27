# PCF85063A

C++ driver for the NXP PCF85063A real-time clock (RTC) over I2C.

The driver is header-only; the complete API is documented in
[`include/pcf85063a.hpp`](include/pcf85063a.hpp).

## Requirements

- ESP-IDF >= 6.0
- `esp_driver_i2c`

## Example

`examples/rtc_datetime` initializes the RTC, sets the current date and time
from the ESP32 system clock, and then prints the RTC time once per second. The
I2C GPIOs and clock speed are configurable through Kconfig. The example only
needs the `pcf85063a` component itself, so it does not depend on any other
component.

## License

Apache-2.0
