# lumehead firmware — ESP32-S3

PlatformIO project that runs on a small set of supported ESP32-S3 boards.
Each board owns one onboard 8×8 WS2812 panel; one **master** plus one or more
**slaves** form a larger logical frame (currently 16×8 with a single slave).

## Supported hardware

| Key | Board | Chip | Flash | Onboard panel | Notes |
| --- | ----- | ---- | ----- | ------------- | ----- |
| `waveshare-esp32-s3-matrix` | [Waveshare ESP32-S3-Matrix](https://www.waveshare.com/wiki/ESP32-S3-Matrix) | ESP32-S3 | 4 MB | 8×8 WS2812 on GPIO 14 | Onboard panel data pin is hard-wired; only host/inter-board I2C pins are user-selectable. |

The full source of truth (chip + flash params + roles built by CI) is
[`hardware.json`](hardware.json). Pin assignments per board live in
[`platformio.ini`](platformio.ini) under each `[hw_*]` section as
`-DLUMEHEAD_*` build flags.

## Roles

| Role       | Build env                                       | Purpose |
| ---------- | ----------------------------------------------- | ------- |
| `master`   | `pio run -e <hw>-master -t upload`              | Faces the host (Klipper / Pi) over I2C @ `0x42`. Renders the full frame. Drives its own panel (left half) and pushes the right half to slave 0 over a second I2C bus. |
| `slave-N`  | `pio run -e <hw>-slave-<N> -t upload`           | Pure pixel sink. Listens on I2C @ `0x43 + N`. Receives RGB rows, blits them to its panel. |

The slave's I2C address is `LUMEHEAD_SLAVE_I2C_BASE + LUMEHEAD_SLAVE_ID`
(default base `0x43`), so each slave board is uniquely addressable. The
current master firmware drives **slave 0** only — additional slave envs
exist so future builds can hang more panels on the same inter-board bus.

Envs follow the convention `<hardware>-<role>`. New boards are added by
appending a `[hw_*]` section + matching `[env:*]` entries in
`platformio.ini` and a matching entry in `hardware.json` (which drives
release CI).

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

> Pins are configurable per hardware via `-DLUMEHEAD_*_PIN` /
> `-DLUMEHEAD_*_SDA` build flags in `platformio.ini`. The Waveshare
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
pio run -e waveshare-esp32-s3-matrix-master -t upload
pio device monitor -e waveshare-esp32-s3-matrix-master

# Slave 0 (panel-only, I2C @ 0x43)
pio run -e waveshare-esp32-s3-matrix-slave-0 -t upload
pio device monitor -e waveshare-esp32-s3-matrix-slave-0

# Slave 1 (panel-only, I2C @ 0x44)
pio run -e waveshare-esp32-s3-matrix-slave-1 -t upload
```

## Status

`render()` on the master currently implements `OFF`, `PROGRESS` (with
flashing leading edge), and `PULSE`. The remaining visualizations
(marquee, static text, heating, printing, leveling) are ready to be ported
from [`simulator/index.html`](../simulator/index.html) — frame buffer,
sprite, and font helpers all map 1:1.
