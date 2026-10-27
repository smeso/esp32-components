# WS2812

C++ driver for WS2812/WS2812B (and pin/timing-compatible 3-byte parts)
addressable RGB LED strips driven over the ESP32 RMT peripheral.

The driver is header-only; the complete API is documented in
[`include/ws2812.hpp`](include/ws2812.hpp). The number of LEDs and the color order
(RGB or GRB) are template parameters; the data GPIO is passed to the constructor.

## Memory placement

The pixel buffer (3 bytes per LED) is allocated once, by the constructor,
through an allocator that is also a template parameter (and a constructor
argument). It defaults to the internal-RAM `std::allocator`, so the driver on
its own still depends on no other component. For long strips the buffer can be
placed in SPI RAM instead by passing `SpiramAllocator<uint8_t>` from the
sibling `spiram_allocation` component:

```cpp
#include "spiram_allocation.hpp"
#include "ws2812.hpp"

ws2812<300, order_rgb, SpiramAllocator<uint8_t>> strip(GPIO_NUM_2);
```

The RMT TX driver rejects a payload that is not in internal RAM when
`CONFIG_RMT_TX_ISR_CACHE_SAFE` (or the deprecated `CONFIG_RMT_ISR_IRAM_SAFE`)
is enabled, and the pixel buffer is the payload of every update. The SPI RAM
buffer therefore only works while that option is disabled (its default); use
the default `std::allocator` otherwise.

## Requirements

- ESP-IDF >= 6.0
- `CONFIG_COMPILER_CXX_EXCEPTIONS` enabled

The component's public dependencies (`esp_driver_rmt` and `esp_driver_gpio`,
or the Linux simulator stand-ins) are pulled in automatically, so a component
that only `REQUIRES ws2812` can include `ws2812.hpp`.

## Example

`examples/color_cycle` fades every LED of a strip through a red/green/blue
color cycle. The data GPIO, the number of LEDs, the update interval and the
number of steps per color are configurable through Kconfig. The example only
needs the `ws2812` component itself, so it does not depend on any other
component.

## License

Apache-2.0
