# led_matrix_display.py
#
# Klipper extras module that drives a 16x8 WS2812 LED matrix
# (two chained 8x8 Waveshare panels) with the same visualizations
# as the web simulator in demo/index.html.
#
# Copy to: ~/klipper/klippy/extras/led_matrix_display.py
#
# Logic mirrors the JavaScript simulator so visualizations stay in sync.

import logging
import math


# ---------------------------------------------------------------------------
# 5x7 dot-matrix font (column-major, LSB = top row).
# Identical bytes to FONT5x7 in demo/index.html.
# ` (backtick) is aliased to the degree glyph for easy typing in g-code.
# ---------------------------------------------------------------------------
FONT5x7 = {
    ' ': [0x00, 0x00, 0x00, 0x00, 0x00],
    '!': [0x00, 0x00, 0x5F, 0x00, 0x00],
    '"': [0x00, 0x07, 0x00, 0x07, 0x00],
    '#': [0x14, 0x7F, 0x14, 0x7F, 0x14],
    '$': [0x24, 0x2A, 0x7F, 0x2A, 0x12],
    '%': [0x23, 0x13, 0x08, 0x64, 0x62],
    '&': [0x36, 0x49, 0x55, 0x22, 0x50],
    "'": [0x00, 0x05, 0x03, 0x00, 0x00],
    '(': [0x00, 0x1C, 0x22, 0x41, 0x00],
    ')': [0x00, 0x41, 0x22, 0x1C, 0x00],
    '*': [0x14, 0x08, 0x3E, 0x08, 0x14],
    '+': [0x08, 0x08, 0x3E, 0x08, 0x08],
    ',': [0x00, 0x50, 0x30, 0x00, 0x00],
    '-': [0x08, 0x08, 0x08, 0x08, 0x08],
    '.': [0x00, 0x60, 0x60, 0x00, 0x00],
    '/': [0x20, 0x10, 0x08, 0x04, 0x02],
    '0': [0x3E, 0x51, 0x49, 0x45, 0x3E],
    '1': [0x00, 0x42, 0x7F, 0x40, 0x00],
    '2': [0x42, 0x61, 0x51, 0x49, 0x46],
    '3': [0x21, 0x41, 0x45, 0x4B, 0x31],
    '4': [0x18, 0x14, 0x12, 0x7F, 0x10],
    '5': [0x27, 0x45, 0x45, 0x45, 0x39],
    '6': [0x3C, 0x4A, 0x49, 0x49, 0x30],
    '7': [0x01, 0x71, 0x09, 0x05, 0x03],
    '8': [0x36, 0x49, 0x49, 0x49, 0x36],
    '9': [0x06, 0x49, 0x49, 0x29, 0x1E],
    ':': [0x00, 0x36, 0x36, 0x00, 0x00],
    ';': [0x00, 0x56, 0x36, 0x00, 0x00],
    '<': [0x00, 0x08, 0x14, 0x22, 0x41],
    '=': [0x14, 0x14, 0x14, 0x14, 0x14],
    '>': [0x41, 0x22, 0x14, 0x08, 0x00],
    '?': [0x02, 0x01, 0x51, 0x09, 0x06],
    '@': [0x32, 0x49, 0x79, 0x41, 0x3E],
    'A': [0x7E, 0x11, 0x11, 0x11, 0x7E],
    'B': [0x7F, 0x49, 0x49, 0x49, 0x36],
    'C': [0x3E, 0x41, 0x41, 0x41, 0x22],
    'D': [0x7F, 0x41, 0x41, 0x22, 0x1C],
    'E': [0x7F, 0x49, 0x49, 0x49, 0x41],
    'F': [0x7F, 0x09, 0x09, 0x09, 0x01],
    'G': [0x3E, 0x41, 0x49, 0x49, 0x7A],
    'H': [0x7F, 0x08, 0x08, 0x08, 0x7F],
    'I': [0x00, 0x41, 0x7F, 0x41, 0x00],
    'J': [0x20, 0x40, 0x41, 0x3F, 0x01],
    'K': [0x7F, 0x08, 0x14, 0x22, 0x41],
    'L': [0x7F, 0x40, 0x40, 0x40, 0x40],
    'M': [0x7F, 0x02, 0x0C, 0x02, 0x7F],
    'N': [0x7F, 0x04, 0x08, 0x10, 0x7F],
    'O': [0x3E, 0x41, 0x41, 0x41, 0x3E],
    'P': [0x7F, 0x09, 0x09, 0x09, 0x06],
    'Q': [0x3E, 0x41, 0x51, 0x21, 0x5E],
    'R': [0x7F, 0x09, 0x19, 0x29, 0x46],
    'S': [0x46, 0x49, 0x49, 0x49, 0x31],
    'T': [0x01, 0x01, 0x7F, 0x01, 0x01],
    'U': [0x3F, 0x40, 0x40, 0x40, 0x3F],
    'V': [0x1F, 0x20, 0x40, 0x20, 0x1F],
    'W': [0x3F, 0x40, 0x38, 0x40, 0x3F],
    'X': [0x63, 0x14, 0x08, 0x14, 0x63],
    'Y': [0x07, 0x08, 0x70, 0x08, 0x07],
    'Z': [0x61, 0x51, 0x49, 0x45, 0x43],
    '[': [0x00, 0x7F, 0x41, 0x41, 0x00],
    '\\':[0x02, 0x04, 0x08, 0x10, 0x20],
    ']': [0x00, 0x41, 0x41, 0x7F, 0x00],
    '^': [0x04, 0x02, 0x01, 0x02, 0x04],
    '_': [0x40, 0x40, 0x40, 0x40, 0x40],
    '`': [0x02, 0x05, 0x02, 0x00, 0x00],   # degree glyph alias
    '\u00b0': [0x02, 0x05, 0x02, 0x00, 0x00],
}


def _get_glyph(ch):
    return FONT5x7.get(ch.upper(), FONT5x7['?'])


def _trim_glyph(ch):
    if ch == ' ':
        return [0, 0, 0]
    g = _get_glyph(ch)
    start, end = 0, len(g) - 1
    while start < len(g) and g[start] == 0:
        start += 1
    while end >= 0 and g[end] == 0:
        end -= 1
    if start > end:
        return [0]
    return g[start:end + 1]


def _glyph_width(ch):
    return len(_trim_glyph(ch))


# ---------------------------------------------------------------------------
# Frame buffer + sprite helpers
# ---------------------------------------------------------------------------
class Frame(object):
    __slots__ = ('cols', 'rows', 'on', 'b')

    def __init__(self, cols, rows):
        self.cols = cols
        self.rows = rows
        n = cols * rows
        self.on = bytearray(n)        # 0 / 1
        self.b = [0.0] * n            # brightness 0..1

    def set(self, x, y, on=True, brightness=1.0):
        if 0 <= x < self.cols and 0 <= y < self.rows:
            i = y * self.cols + x
            self.on[i] = 1 if on else 0
            self.b[i] = brightness if on else 0.0


def draw_glyph(frame, ch, gx, gy):
    cols = _trim_glyph(ch)
    for cx, bits in enumerate(cols):
        for ry in range(7):
            if bits & (1 << ry):
                frame.set(gx + cx, gy + ry, True)
    return len(cols)


def draw_sprite(frame, sprite, sx, sy):
    """sprite = list[str]; '#'=on, ' '/'.'=off, '0'-'9' = on @ digit/9."""
    for y, row in enumerate(sprite):
        for x, ch in enumerate(row):
            if ch in (' ', '.'):
                continue
            b = 1.0
            if '0' <= ch <= '9':
                b = int(ch) / 9.0
            frame.set(sx + x, sy + y, True, b)


# ---------------------------------------------------------------------------
# Color overlay engine (mirrors getColor in the simulator)
# ---------------------------------------------------------------------------
def _hex_to_rgb(hex_str):
    h = hex_str.lstrip('#')
    if len(h) != 6:
        return (255, 0, 0)
    v = int(h, 16)
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def _hsv_to_rgb(h, s, v):
    h = h % 360.0
    c = v * s
    x = c * (1.0 - abs(((h / 60.0) % 2.0) - 1.0))
    m = v - c
    if   h < 60:  r, g, b = c, x, 0
    elif h < 120: r, g, b = x, c, 0
    elif h < 180: r, g, b = 0, c, x
    elif h < 240: r, g, b = 0, x, c
    elif h < 300: r, g, b = x, 0, c
    else:         r, g, b = c, 0, x
    return (int((r + m) * 255), int((g + m) * 255), int((b + m) * 255))


# ---------------------------------------------------------------------------
# Visualizations  (each returns a Frame)
# ---------------------------------------------------------------------------
def viz_progress(cols, rows, t, progress):
    f = Frame(cols, rows)
    filled = int(round(progress * cols))
    flash_on = int(t * 8) % 2 == 0     # ~4 Hz blink for the leading edge
    for x in range(filled):
        leading = (x == filled - 1)
        if leading and not flash_on:
            continue
        for y in range(rows):
            f.set(x, y, True)
    return f


def viz_marquee(cols, rows, text, scroll_px):
    f = Frame(cols, rows)
    gap = 1
    widths = [_glyph_width(c) for c in text]
    cursor = cols - int(scroll_px)
    for i, ch in enumerate(text):
        w = widths[i]
        if cursor + w >= 0 and cursor < cols:
            draw_glyph(f, ch, cursor, 0)
        cursor += w + gap
    return f


def viz_static(cols, rows, text):
    f = Frame(cols, rows)
    gap = 1
    visible = text[:3]
    widths = [_glyph_width(c) for c in visible]
    total = sum(widths) + gap * max(0, len(visible) - 1)
    cursor = max(0, (cols - total) // 2)
    for i, ch in enumerate(visible):
        draw_glyph(f, ch, cursor, 0)
        cursor += widths[i] + gap
    return f


def viz_pulse(cols, rows, t, speed):
    f = Frame(cols, rows)
    phase = t * (speed / 25.0)
    b = 0.15 + 0.85 * (math.sin(phase) * 0.5 + 0.5)
    for y in range(rows):
        for x in range(cols):
            f.set(x, y, True, b)
    return f


def viz_heating(cols, rows, t):
    f = Frame(cols, rows)
    thermo = [
        "  ###  ",
        "  # #  ",
        "  # #  ",
        "  # #  ",
        "  # #  ",
        " ##0## ",
        " #000# ",
        " ##### ",
    ]
    draw_sprite(f, thermo, 0, 0)
    fill_level = math.sin(t / 0.8) * 0.5 + 0.5
    stem_top, stem_bot = 1, 5
    fill_rows = int(round(fill_level * (stem_bot - stem_top + 1)))
    for i in range(fill_rows):
        y = stem_bot - i
        f.set(3, y, True)
        f.set(4, y, True)
    tt = t * 5.0
    for w in range(3):
        base_y = 1 + w * 2
        for x in range(8, cols):
            phase = (x - 8) * 0.9 + tt + w * 1.2
            y = base_y + int(round(math.sin(phase)))
            f.set(x, y, True)
    return f


def viz_printing(cols, rows, t):
    f = Frame(cols, rows)
    for x in range(cols):
        f.set(x, 7, True, 0.6)
    sweep = math.sin(t / 0.7) * 0.5 + 0.5
    head_x = int(round(sweep * (cols - 3)))
    head_y = 0
    draw_sprite(f, ["###", "###"], head_x, head_y)
    f.set(head_x + 1, head_y + 2, True)
    layer_y = 5
    for y in range(head_y + 3, layer_y):
        f.set(head_x + 1, y, True, 0.5)
    for x in range(cols):
        dist = abs(x - (head_x + 1))
        if dist < 6:
            b = max(0.15, 1.0 - dist * 0.18)
            f.set(x, layer_y, True, b)
    return f


def viz_leveling(cols, rows, t):
    f = Frame(cols, rows)
    for x in range(cols):
        f.set(x, 3, True, 0.4)
        f.set(x, 4, True, 0.4)
    f.set(0, 2, True);          f.set(0, 5, True)
    f.set(cols - 1, 2, True);   f.set(cols - 1, 5, True)
    cx = cols // 2
    f.set(cx - 1, 1, True, 0.7); f.set(cx, 1, True, 0.7)
    f.set(cx - 1, 6, True, 0.7); f.set(cx, 6, True, 0.7)
    drift = math.sin(t / 0.9) * 5
    wobble = math.sin(t / 0.13) * 0.4
    bx = int(round(cx - 1 + drift + wobble))
    f.set(bx,     3, True, 1)
    f.set(bx + 1, 3, True, 1)
    f.set(bx,     4, True, 1)
    f.set(bx + 1, 4, True, 1)
    f.set(bx,     2, True, 0.8)
    f.set(bx + 1, 2, True, 0.8)
    return f


# ---------------------------------------------------------------------------
# Main plugin
# ---------------------------------------------------------------------------
class LedMatrixDisplay(object):
    def __init__(self, config):
        self.printer = config.get_printer()
        self.reactor = self.printer.get_reactor()
        self.gcode = self.printer.lookup_object('gcode')

        self.neopixel_name = config.get('neopixel')
        self.cols = config.getint('cols', 16)
        self.rows = config.getint('rows', 8)
        self.panel_w = config.getint('panel_width', 8)
        self.panels = config.getint('panels', 2)
        self.serpentine = config.getboolean('serpentine', False)

        self.fps = config.getfloat('fps', 30.0, above=1.0, maxval=120.0)
        self.brightness = config.getfloat('brightness', 0.25,
                                          minval=0.0, maxval=1.0)
        self.color = _hex_to_rgb(config.get('default_color', '#ff3344'))
        self.rainbow = config.getboolean('rainbow', False)
        self.speed = config.getfloat('speed', 40.0, above=0.0)
        self.auto_mode = config.getboolean('auto_mode', True)
        self.marquee_text = config.get('marquee_text', 'KLIPPER READY')

        self.mode = config.get('default_mode', 'idle')
        self.static_text = ''
        self.progress_value = 0.0
        self._scroll_px = 0.0
        self._last_t = 0.0
        self._neopixel = None
        self._timer = None

        self.printer.register_event_handler('klippy:ready', self._on_ready)
        if self.auto_mode:
            self._wire_auto_events()

        self.gcode.register_command(
            'MATRIX_SHOW', self.cmd_MATRIX_SHOW,
            desc='Set the LED matrix display mode')
        self.gcode.register_command(
            'MATRIX_SET', self.cmd_MATRIX_SET,
            desc='Set color/brightness/speed for the LED matrix')
        self.gcode.register_command(
            'MATRIX_TEXT', self.cmd_MATRIX_TEXT,
            desc='Update marquee/static text')

    # ---------------- lifecycle ----------------
    def _on_ready(self):
        try:
            self._neopixel = self.printer.lookup_object(
                'neopixel ' + self.neopixel_name)
        except Exception as e:
            logging.exception(
                "led_matrix_display: cannot find neopixel '%s': %s",
                self.neopixel_name, e)
            return
        self._last_t = self.reactor.monotonic()
        self._timer = self.reactor.register_timer(
            self._render_tick, self.reactor.NOW)

    def _wire_auto_events(self):
        h = self.printer.register_event_handler
        h('print_stats:start_printing',
          lambda *a: self._set_mode('printing'))
        h('print_stats:complete_printing',
          lambda *a: self._set_mode('idle'))
        h('print_stats:cancelled_printing',
          lambda *a: self._set_mode('idle'))
        h('homing:home_rails_begin',
          lambda *a, **k: self._set_mode('leveling'))
        h('homing:home_rails_end',
          lambda *a, **k: self._set_mode('idle'))

    def _set_mode(self, mode):
        self.mode = mode

    # ---------------- render loop ----------------
    def _render_tick(self, eventtime):
        dt = eventtime - self._last_t
        self._last_t = eventtime
        try:
            frame = self._build_frame(eventtime, dt)
            self._push(frame, eventtime)
        except Exception:
            logging.exception("led_matrix_display: render failed")
        return eventtime + 1.0 / self.fps

    def _build_frame(self, t, dt):
        mode = self.mode
        if mode == 'idle':
            mode = 'marquee'

        if mode == 'progress':
            return viz_progress(self.cols, self.rows, t,
                                max(0.0, min(1.0, self.progress_value)))
        if mode == 'marquee':
            text = self.static_text or self.marquee_text or ' '
            self._scroll_px += (self.speed / 20.0) * (dt * 60.0 / 16.0)
            gap = 1
            total_w = sum(_glyph_width(c) + gap for c in text)
            if self._scroll_px > total_w + self.cols:
                self._scroll_px = 0.0
            return viz_marquee(self.cols, self.rows, text, self._scroll_px)
        if mode == 'static':
            return viz_static(self.cols, self.rows,
                              self.static_text or ' ')
        if mode == 'pulse':
            return viz_pulse(self.cols, self.rows, t, self.speed)
        if mode == 'heating':
            return viz_heating(self.cols, self.rows, t * (self.speed / 40.0))
        if mode == 'printing':
            return viz_printing(self.cols, self.rows, t * (self.speed / 40.0))
        if mode == 'leveling':
            return viz_leveling(self.cols, self.rows, t * (self.speed / 40.0))
        # 'off' / unknown
        return Frame(self.cols, self.rows)

    # ---------------- output ----------------
    def _color_at(self, x, y, t, b):
        if self.rainbow:
            r, g, bl = _hsv_to_rgb((x * 10 + t * 60) % 360, 1.0, 1.0)
        else:
            r, g, bl = self.color
        scale = b * self.brightness
        return (r * scale / 255.0,
                g * scale / 255.0,
                bl * scale / 255.0)

    def _coord_to_chain_index(self, x, y):
        """Map (x, y) -> position in the WS2812 chain."""
        panel = x // self.panel_w
        local_x = x % self.panel_w
        if self.serpentine and (y % 2 == 1):
            local_x = self.panel_w - 1 - local_x
        within_panel = y * self.panel_w + local_x
        return panel * (self.panel_w * self.rows) + within_panel

    def _push(self, frame, t):
        np = self._neopixel
        if np is None:
            return
        # Klipper's neopixel exposes update_color_data(red, green, blue,
        # white, index, transmit). We update each LED, transmit once at end.
        n = self.cols * self.rows
        for y in range(self.rows):
            for x in range(self.cols):
                i = y * self.cols + x
                if frame.on[i]:
                    r, g, b = self._color_at(x, y, t, frame.b[i])
                else:
                    r = g = b = 0.0
                idx = self._coord_to_chain_index(x, y)
                try:
                    np.update_color_data(r, g, b, 0.0, idx, transmit=False)
                except TypeError:
                    # Older API: positional only
                    np.update_color_data(r, g, b, 0.0, idx, False)
        try:
            np.send_data()
        except AttributeError:
            # Some Klipper versions transmit on the last update_color_data.
            try:
                np.update_color_data(0.0, 0.0, 0.0, 0.0, n - 1, True)
            except Exception:
                pass

    # ---------------- g-code ----------------
    def cmd_MATRIX_SHOW(self, gcmd):
        mode = gcmd.get('MODE', self.mode).lower()
        valid = ('off', 'idle', 'marquee', 'static', 'progress',
                 'pulse', 'heating', 'printing', 'leveling')
        if mode not in valid:
            raise gcmd.error("Invalid MODE '%s'. Valid: %s"
                             % (mode, ', '.join(valid)))
        self.mode = mode
        text = gcmd.get('TEXT', None)
        if text is not None:
            self.static_text = text
            self._scroll_px = 0.0
        value = gcmd.get_float('VALUE', None, minval=0.0, maxval=1.0)
        if value is not None:
            self.progress_value = value

    def cmd_MATRIX_SET(self, gcmd):
        color = gcmd.get('COLOR', None)
        if color is not None:
            self.color = _hex_to_rgb(color)
        rainbow = gcmd.get_int('RAINBOW', None, minval=0, maxval=1)
        if rainbow is not None:
            self.rainbow = bool(rainbow)
        b = gcmd.get_float('BRIGHTNESS', None, minval=0.0, maxval=1.0)
        if b is not None:
            self.brightness = b
        s = gcmd.get_float('SPEED', None, above=0.0)
        if s is not None:
            self.speed = s

    def cmd_MATRIX_TEXT(self, gcmd):
        self.static_text = gcmd.get('TEXT', '')
        self._scroll_px = 0.0


def load_config(config):
    return LedMatrixDisplay(config)
