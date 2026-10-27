# button

C++ driver for a single debounced push-button connected to a GPIO.

The driver debounces the button in software and provides press, release and
multiple long-press callbacks. The button logic (active-low vs active-high)
and the internal pull configuration are set through constructor arguments.
The GPIO interrupt is installed in `start()` and torn down in `stop()`. The
GPIO ISR service behind it is global and can be installed only once, so the
first `start()` of the process installs it and later ones simply add their
own handler to it.

Callbacks run from the FreeRTOS timer daemon task; pass a task queue as the
`TaskQueue` template argument to run them on a dedicated worker task instead.

`stop()` (and the destructor, which calls it) waits until the timer daemon
task has processed every command queued for the button, so callbacks can
never touch the object after `stop()` returns. Consequently `stop()` must
not be called from the timer daemon task or from a callback; always call it
from the thread that owns the button.

The complete API is documented in
[`include/button.hpp`](include/button.hpp).

## Requirements

- ESP-IDF >= 6.0
- `esp_driver_gpio`, `freertos`

## Example

`examples/press_counter` counts presses, short presses and long presses of a
button connected to a GPIO, and prints the counters every second. The button
GPIO, the active level and the long-press threshold are configurable through
Kconfig. The example only needs the `button` component itself, so it does not
depend on any other component.

## License

Apache-2.0
