# multidevice_test

Tests for the two-board bring-up rig: two ESP32-S3 boards wired to each other
are used to check that every inter-board connection and protocol works before
the rig is trusted to test components.

Two doctest suites live here, one per role:

| Suite | Source | Role |
| --- | --- | --- |
| `multidevice_a` | `multidevice_a.cpp` | leader: drives the buses |
| `multidevice_b` | `multidevice_b.cpp` | follower: answers the leader |

They are compiled into two separate firmwares. Only the source file of the
selected suite is compiled, so each board gets its own image:

```sh
TARGET=esp32s3
PORT=/dev/ttyUSB0

idf.py -C components/utils/test_app -DIDF_TARGET=$TARGET \
    -DMULTIDEVICE_SUITE=multidevice_a -B build_a -p $PORT flash monitor   # board A
idf.py -C components/utils/test_app -DIDF_TARGET=$TARGET \
    -DMULTIDEVICE_SUITE=multidevice_b -B build_b -p $PORT flash monitor   # board B
```

Each firmware runs only its suite (the shared runner selects it with the
doctest `--test-suite` filter) and prints the usual doctest output on the
console, so the operator can check both boards independently.

## Wiring

UART pins are cross connected (TX of one board to RX of the other); all other
signals are connected pin to pin. The I2C lines additionally carry an external
10K pull-up to 3.3V on each board, so the internal pull-ups are left disabled.

| Bus | Signal | GPIO |
| --- | --- | --- |
| UART1 | TX/RX | 3 / 4 |
| UART2 | TX/RX | 5 / 6 |
| I2C | SDA/SCL | 7 / 8 |
| SPI | SCK/MOSI/MISO/CS | 9 / 10 / 11 / 12 |
| I2S0 | BCLK/WS/SD | 13 / 14 / 15 |
| RMT | data | 16 |
| GPIO | handshake | 17 |
| PWM | output/measure | 18 |

## Tests

Each suite runs the same case names in the same order; the two firmwares stay
in lockstep with a small framed rendezvous over UART1:

- `the gpio link carries both levels`
- `the uart1 control link carries a payload`
- `the uart2 link carries a payload both ways`
- `the spi master and slave exchange data`
- `the i2s master sends samples to the slave`
- `the rmt link reproduces the transmitted waveform`
- `the pwm output is measured by the peer`
- `the i2c master and slave exchange data`

The leader (A) is the I2C/SPI/I2S master and the RMT/PWM transmitter; the
follower (B) is the slave and measures the PWM duty with an RMT RX channel.

## Status LED

Both boards carry a WS2812 on GPIO 38 driven by the shared runner:

| Colour | Meaning |
| --- | --- |
| blue | booting |
| yellow | suite running |
| green | suite completed successfully |
| red | suite failed |
