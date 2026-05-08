# Creating a new animation

Animations live in three places that intentionally mirror each other:

| Layer | File | Language |
| --- | --- | --- |
| Authoring / preview | [`simulator/index.html`](../simulator/index.html) | JavaScript (canvas) |
| Klipper plugin | [`klipper/led_matrix_display.py`](../klipper/led_matrix_display.py) | Python |
| Standalone firmware | [`firmware/src/main.cpp`](../firmware/src/main.cpp) | C++ (FastLED) |

The drawing primitives — `setPixel`, `drawGlyph`, `drawSprite`, `getColor`,
`mapCoord`, the `FONT5x7` table — have the **same names and same maths** in
all three layers. Once an animation works in the simulator, porting it is
mostly mechanical.

## Step 1 — prototype in the simulator

The simulator is a single self-contained HTML file with no build step.
Open it in any modern browser:

```sh
start simulator/index.html        # Windows
xdg-open simulator/index.html     # Linux
open simulator/index.html         # macOS
```

Find the visualization registry (look for the existing functions named
`viz_progress`, `viz_marquee`, `viz_heating`, …). Each one has the same
signature:

```js
function viz_myanim(t, params) {
  // t is seconds since the visualization started.
  // params come from the UI controls (color, speed, brightness, text, value).
  // Write into the global frame buffer using the helpers below.
}
```

### Available primitives

```js
setPixel(x, y, color);              // 0 <= x < 16, 0 <= y < 8
drawGlyph(ch, x, y, color);         // 5x7 char from FONT5x7
drawSprite(sprite, x, y, palette);  // ASCII art: '#' '.' ' ' '0'..'9'
getColor(x, y, t, baseHex);         // solid or rainbow overlay
clearFrame();                       // fills with black
```

`drawSprite` accepts ASCII art with these meanings:

| Char | Meaning |
| --- | --- |
| `#` | full brightness |
| `.` | dim (~30%) |
| `0`–`9` | numeric brightness (`9` = full, `0` = off) |
| ` ` (space) | transparent (skip) |

### Register the animation

1. Add `myanim` to the mode `<select>` in the HTML.
2. Add a `case 'myanim':` to the dispatch switch in the render loop.
3. Reload the page. The new animation should appear in the dropdown.

## Step 2 — port to the Klipper plugin

Open [`klipper/led_matrix_display.py`](../klipper/led_matrix_display.py).

1. Translate `viz_myanim` into a method on `LedMatrixDisplay` named
   `_render_myanim(self, t, frame)`. The frame primitives have the same
   names — `_set_pixel`, `_draw_glyph`, `_draw_sprite`, `_get_color`.
2. Add `'myanim'` to `_MODES` so `MATRIX_SHOW MODE=myanim` validates.
3. Dispatch to it from `_build_frame`.

If the animation should react to a Klipper event (printing, homing,
heating), wire it in `_on_print_stats` / `_on_homing` / etc., guarded by
`auto_mode`.

## Step 3 — port to the standalone firmware (optional)

Open [`firmware/src/main.cpp`](../firmware/src/main.cpp).

1. Translate the function into C++. The frame buffer is `g_frame[128]`
   (`MATRIX_COLS * MATRIX_ROWS`); the helpers `panelIndex`, `drawGlyphRGB`,
   `glyphWidth` already exist.
2. Add a `MODE_MYANIM` constant matching the byte the host will send via
   command `0x01 SET_MODE`.
3. Dispatch to it from `render()` in the master role.
4. Document the new mode id in
   [`firmware/README.md`](../firmware/README.md) under the host I2C
   protocol table.

## Step 4 — record a preview

Capture a short looped GIF of the animation running in the simulator and
save it as `docs/media/<DescriptiveName>.gif`. Add a row to the
**Visualizations** table in the top-level [README](../README.md) so it
shows up alongside the others.

## Style tips

- Aim for ~30 fps; everything stays smooth at that rate.
- Use `getColor(x, y, t, hex)` rather than hard-coding RGB so users get the
  rainbow overlay for free.
- Keep state in the function arguments (`t`, `params`) — avoid module-level
  mutable state so the simulator and Klipper layers stay deterministic.
- Use `drawSprite` with ASCII art for anything iconic. It's far easier to
  iterate on than coordinate maths.

## Checklist

- [ ] Works in the simulator.
- [ ] Ported to the Klipper plugin and listed in `_MODES`.
- [ ] (Optional) Ported to the firmware and given a `MODE_*` byte.
- [ ] GIF preview committed under `docs/media/`.
- [ ] Row added to the **Visualizations** table in the top-level README.
