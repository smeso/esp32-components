# TCA9555

C++ driver for the TCA9555 16-bit I2C GPIO expander, plus an
interrupt-driven button manager for all 16 pins.

The expander is controlled over I2C (addresses 0x20..0x27, default 0x20).
The driver is header-only; the complete API is documented in
[`include/tca9555.hpp`](include/tca9555.hpp). The `tca9555_button` template
uses FreeRTOS timers for debouncing and long-press detection, and can post
callbacks to a [`task_queue`](../task_queue) instead of running them in the
timer context.

## Requirements

- ESP-IDF >= 6.0
- `esp_driver_i2c`, `freertos`

## Example

`examples/gpio_blink` drives an LED and reads a button through a TCA9555.
It configures one output pin (LED) and one input pin (button), then blinks
the LED; holding the button speeds the blink up and a long press turns the
LED solid on. The button manager is polled through its tick timer, so no
interrupt line (INT#) has to be wired up. The example only needs the
`tca9555` component itself, so it does not depend on any other component.
The I2C GPIOs, the TCA9555 address and the I2C clock speed are configurable
through Kconfig.

## License

Apache-2.0
