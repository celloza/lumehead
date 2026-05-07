# lumehead firmware — ESP32-S3

PlatformIO project that runs on **two Waveshare ESP32-S3-Matrix boards**.
Each board owns its onboard 8×8 WS2812 panel; together they form one logical
16×8 toolhead display.

## Roles

| Role     | Build env       | Purpose |
| -------- | --------------- | ------- |
| `master` | `pio run -e master -t upload` | Faces the host (Klipper / Pi) over I2C @ `0x42`. Renders the full 16×8 frame. Drives its own panel (left half) and pushes the right half to the slave over a second I2C bus. |
| `slave`  | `pio run -e slave -t upload`  | Pure pixel sink. Listens on I2C @ `0x43`. Receives RGB rows, blits them to its panel. |

Open the project from the repo root with `pio run -d firmware ...` or open
the `firmware/` folder directly in VS Code.

## Wiring

```
                          +------------------+
  Klipper / Pi  ---SDA--->|  MASTER  GPIO 8 |   host I2C @ 0x42
                ---SCL--->|          GPIO 9 |
                          |                  |
                          |  Wire1 GPIO 4 ------SDA----+
                          |  Wire1 GPIO 5 ------SCL----+   inter-board I2C
                          +------------------+         |   @ 400 kHz
                                                       v
                          +------------------+
                          |  SLAVE   GPIO 4 |<---SDA----
                          |          GPIO 5 |<---SCL----
                          +------------------+
                              I2C @ 0x43
```

Both boards share GND. Each board powers its own panel from 5 V — keep the
power rails common but the panel data lines completely separate (no chain).

> Adjust pin numbers in `src/main.cpp` if your wiring differs. The Waveshare
> ESP32-S3-Matrix's onboard panel is hard-wired to **GPIO 14**.

## Host I2C protocol (master @ 0x42)

| Cmd  | Name           | Payload                    |
| ---- | -------------- | -------------------------- |
| 0x01 | SET_MODE       | 1 byte mode id (0..7)      |
| 0x02 | SET_PROGRESS   | 1 byte 0..255              |
| 0x03 | SET_COLOR      | 3 bytes R, G, B            |
| 0x04 | SET_BRIGHTNESS | 1 byte 0..255              |
| 0x05 | SET_TEXT       | N bytes ASCII (max 63)     |
| 0xFF | CLEAR          | —                          |

A read returns 3 bytes: `[mode, progress, brightness]`.

Mode ids: `0 OFF, 1 MARQUEE, 2 STATIC, 3 PROGRESS, 4 PULSE, 5 HEATING,
6 PRINTING, 7 LEVELING`.

## Inter-board protocol (master → slave @ 0x43)

Sent at ~30 fps over Wire1 / Wire (slave side) at 400 kHz:

| Cmd  | Name        | Payload |
| ---- | ----------- | ------- |
| 0xF0 | FRAME_BEGIN | — |
| 0xF1 | FRAME_ROW   | 1 byte rowIdx (0..7) + 8 × RGB (24 bytes) |
| 0xF2 | FRAME_END   | — (slave latches with `FastLED.show()`) |

One frame = 1× BEGIN + 8× ROW + 1× END = 10 small I2C transactions, ~210
bytes total. Comfortably fits 30 fps with margin.

## Build & flash

```sh
# Master board (host-facing)
pio run -e master -t upload
pio device monitor -e master

# Slave board (panel-only)
pio run -e slave -t upload
pio device monitor -e slave
```

## Status

`render()` on the master currently implements `OFF`, `PROGRESS` (with
flashing leading edge), and `PULSE`. The remaining visualizations
(marquee, static text, heating, printing, leveling) are ready to be ported
from [`simulator/index.html`](../simulator/index.html) — frame buffer,
sprite, and font helpers all map 1:1.
