# LED Matrix Display - Klipper Plugin

A Klipper extras module that drives a 16×8 WS2812 LED matrix (two 8×8 Waveshare
panels chained side-by-side) with the same visualizations as the web simulator
in `demo/index.html` — marquee, static text, progress bar, pulsing box,
heating, printing, leveling.

## Hardware

- **MCU:** any Klipper-supported board with a free GPIO that can drive WS2812
  data. ESP32-S3 with the Klipper port works well; an RP2040/SKR/Octopus is
  fine too.
- **Panels:** 2× WS2812-based 8×8 RGB matrices (e.g. Waveshare RGB-Matrix-P3-64).
  Wire them in a single chain: panel A `DOUT` → panel B `DIN`. 5 V power, with
  a level shifter on data if your MCU runs at 3.3 V (most do).
- **Total LEDs:** 128 (64 per panel).

## Install

1. Copy `led_matrix_display.py` into your Klipper installation:
   ```
   ~/klipper/klippy/extras/led_matrix_display.py
   ```
2. Restart Klipper.

## Configure

Add to `printer.cfg`:

```ini
# 1) Declare the WS2812 chain via Klipper's built-in neopixel module
[neopixel matrix]
pin: PA8                  # change to your data pin
chain_count: 128          # 2 panels × 64
color_order: GRB          # most WS2812 are GRB
initial_RED: 0.0
initial_GREEN: 0.0
initial_BLUE: 0.0

# 2) Bind the matrix display plugin to that chain
[led_matrix_display]
neopixel: matrix          # name of the [neopixel ...] section above
cols: 16
rows: 8
panel_width: 8            # width of each physical panel
panels: 2                 # number of chained panels horizontally
# Per-panel wiring. Common Waveshare layout = row-major, no serpentine.
# Override if your panel snakes: serpentine: true
serpentine: false
# Visual behaviour
fps: 30
brightness: 0.25          # 0..1, scales final RGB
default_mode: idle
default_color: #ff3344
# Optional: auto-switch mode based on printer state
auto_mode: true
marquee_text: KLIPPER READY
```

## G-code commands

```
MATRIX_SHOW MODE=marquee   TEXT="HELLO ESP32-S3"
MATRIX_SHOW MODE=static    TEXT="210"
MATRIX_SHOW MODE=progress  VALUE=0.42
MATRIX_SHOW MODE=pulse
MATRIX_SHOW MODE=heating
MATRIX_SHOW MODE=printing
MATRIX_SHOW MODE=leveling
MATRIX_SHOW MODE=off

MATRIX_SET COLOR=#00ff88 RAINBOW=0 BRIGHTNESS=0.4 SPEED=40
MATRIX_TEXT TEXT="72`F"        # ` is rendered as the degree glyph
```

## Auto mode (optional)

When `auto_mode: true`, the plugin listens to standard Klipper events and
switches modes automatically:

- Heating to target → `heating`
- Print started → `printing` then `progress` driven by `display_status.progress`
- Probe / bed mesh / Z-tilt running → `leveling`
- Idle → marquee with `marquee_text`

You can always override with `MATRIX_SHOW`; the next event will resume auto.

## Notes on porting from the web simulator

The frame buffer and `getColor(x, y, t)` overlay engine in
`led_matrix_display.py` mirror `demo/index.html` 1:1 — same font table, same
sprite format, same coordinate mapper. If you change a visualization in the
simulator, copy the function body straight across.
