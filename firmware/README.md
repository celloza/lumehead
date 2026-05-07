# lumehead firmware — ESP32-S3

PlatformIO project that runs on an ESP32-S3 mounted on the toolhead. It:

- Receives display commands over I2C as a **slave at address `0x42`**.
- Drives a **16×8 WS2812 matrix** (two chained 8×8 panels, 128 LEDs total)
  via FastLED.
- Mirrors the visualization logic in [`simulator/index.html`](../simulator/index.html)
  and [`klipper/led_matrix_display.py`](../klipper/led_matrix_display.py).

## Build & flash

```sh
pio run -t upload          # build + flash
pio device monitor         # serial @ 115200
```

## Pin map (defaults — edit in `src/main.cpp`)

| Signal     | GPIO |
| ---------- | ---- |
| I2C SDA    | 8    |
| I2C SCL    | 9    |
| LED data   | 4    |

## I2C command protocol (placeholder)

| Cmd  | Name           | Payload                    |
| ---- | -------------- | -------------------------- |
| 0x01 | SET_MODE       | 1 byte mode id (0..7)      |
| 0x02 | SET_PROGRESS   | 1 byte 0..255              |
| 0x03 | SET_COLOR      | 3 bytes R, G, B            |
| 0x04 | SET_BRIGHTNESS | 1 byte 0..255              |
| 0x05 | SET_TEXT       | N bytes ASCII (max 63)     |
| 0xFF | CLEAR          | —                          |

Mode IDs match `enum Mode` in `src/main.cpp`:

```
0 OFF, 1 MARQUEE, 2 STATIC, 3 PROGRESS, 4 PULSE,
5 HEATING, 6 PRINTING, 7 LEVELING
```

A read from the slave returns 3 bytes: `[mode, progress, brightness]`.

## Status

`render()` currently implements `OFF`, `PROGRESS`, and `PULSE` as a starting
point. Port the remaining visualizations (marquee, static text, heating,
printing, leveling) from the simulator — the frame-buffer layout and
coordinate mapper are already in place.
