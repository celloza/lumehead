/*
 * lumehead — ESP32-S3 firmware
 *
 * Two boards, two roles, selected at compile time:
 *
 *   LUMEHEAD_ROLE_MASTER
 *     - Host-facing I2C slave on Wire @ 0x42 (commands from Klipper / Pi).
 *     - Renders the full 16x8 frame in RAM.
 *     - Drives its own onboard 8x8 panel (left half, columns 0..7).
 *     - Pushes the right half (columns 8..15) to the slave board over Wire1
 *       in I2C master mode @ 400 kHz, ~30 fps.
 *
 *   LUMEHEAD_ROLE_SLAVE
 *     - Pure pixel sink. Listens on Wire @ 0x43.
 *     - Receives 64 * 3 = 192 bytes of raw RGB per frame and blits to its
 *       onboard 8x8 panel.
 *
 * Pin assignments and the slave I2C address are configurable via build
 * flags in `platformio.ini` (per-hardware section). Defaults below match the
 * Waveshare ESP32-S3-Matrix:
 *   onboard 8x8 panel data : GPIO 14  (hard-wired on the dev board)
 *   host  I2C SDA / SCL    : GPIO 8 / 9   (master only)
 *   inter-board SDA / SCL  : GPIO 4 / 5   (master Wire1 <-> slave Wire)
 *
 * Multiple slaves are supported by giving each one a unique
 * `LUMEHEAD_SLAVE_ID` (0..N). The slave's I2C address becomes
 * `LUMEHEAD_SLAVE_I2C_BASE + LUMEHEAD_SLAVE_ID` (default base 0x43).
 */

#include <Arduino.h>
#include <FastLED.h>
#include <Wire.h>

// ---------------------------------------------------------------------------
// Geometry — each board owns a single 8x8 panel.
// The MASTER additionally tracks a 16x8 logical frame for rendering.
// ---------------------------------------------------------------------------
static constexpr uint8_t  PANEL_WIDTH  = 8;
static constexpr uint8_t  PANEL_HEIGHT = 8;
static constexpr uint16_t PANEL_LEDS   = PANEL_WIDTH * PANEL_HEIGHT;   // 64

// ---------------------------------------------------------------------------
// Build-time configuration. All values are overridable via -D flags in
// platformio.ini so the same source tree compiles for different hardware
// and different slave instances.
// ---------------------------------------------------------------------------
#ifndef LUMEHEAD_LED_DATA_PIN
#define LUMEHEAD_LED_DATA_PIN     14   // Waveshare onboard panel
#endif
#ifndef LUMEHEAD_HOST_I2C_SDA
#define LUMEHEAD_HOST_I2C_SDA     8
#endif
#ifndef LUMEHEAD_HOST_I2C_SCL
#define LUMEHEAD_HOST_I2C_SCL     9
#endif
#ifndef LUMEHEAD_INTER_I2C_SDA
#define LUMEHEAD_INTER_I2C_SDA    4
#endif
#ifndef LUMEHEAD_INTER_I2C_SCL
#define LUMEHEAD_INTER_I2C_SCL    5
#endif
#ifndef LUMEHEAD_HOST_I2C_ADDR
#define LUMEHEAD_HOST_I2C_ADDR    0x42
#endif
#ifndef LUMEHEAD_SLAVE_I2C_BASE
#define LUMEHEAD_SLAVE_I2C_BASE   0x43
#endif
#ifndef LUMEHEAD_SLAVE_ID
#define LUMEHEAD_SLAVE_ID         0
#endif

static constexpr int      LED_DATA_PIN = LUMEHEAD_LED_DATA_PIN;
static CRGB g_panel[PANEL_LEDS];

// I2C addresses
static constexpr uint8_t  I2C_ADDR_MASTER = LUMEHEAD_HOST_I2C_ADDR;
static constexpr uint8_t  I2C_ADDR_SLAVE  = LUMEHEAD_SLAVE_I2C_BASE + LUMEHEAD_SLAVE_ID;

// I2C pins
static constexpr int      HOST_I2C_SDA   = LUMEHEAD_HOST_I2C_SDA;
static constexpr int      HOST_I2C_SCL   = LUMEHEAD_HOST_I2C_SCL;
static constexpr int      INTER_I2C_SDA  = LUMEHEAD_INTER_I2C_SDA;
static constexpr int      INTER_I2C_SCL  = LUMEHEAD_INTER_I2C_SCL;
static constexpr uint32_t INTER_I2C_FREQ = 400000;

// Inter-board protocol
//   Master -> Slave commands on Wire1 / Wire (slave side).
//     0xF0 FRAME_BEGIN  (no payload; resets the slave's row cursor)
//     0xF1 FRAME_ROW    payload: 1 byte row index, 8 * 3 = 24 bytes RGB
//     0xF2 FRAME_END    payload: none; slave calls FastLED.show()
static constexpr uint8_t CMD_FRAME_BEGIN = 0xF0;
static constexpr uint8_t CMD_FRAME_ROW   = 0xF1;
static constexpr uint8_t CMD_FRAME_END   = 0xF2;

// ---------------------------------------------------------------------------
// Coordinate mapper for a single panel (row-major, no serpentine).
// ---------------------------------------------------------------------------
static inline uint16_t panelIndex(uint8_t x, uint8_t y) {
    return static_cast<uint16_t>(y) * PANEL_WIDTH + x;
}

// ---------------------------------------------------------------------------
// "Hello world" boot animation.
// Master scrolls a marquee of "HELLO" across the full 16x8 frame, with a
// slowly cycling hue so the color still drifts. Until the master has sent
// a frame, the slave shows a local hue sweep so a loose data cable still
// gives a visible heartbeat.
// ---------------------------------------------------------------------------
static inline CRGB helloColor(uint32_t nowMs) {
    const uint8_t hue = static_cast<uint8_t>((nowMs / 32) & 0xFF);  // slower
    return CHSV(hue, 255, 255);
}

static inline void fillPanelHello(CRGB* panel, uint32_t nowMs) {
    const CRGB c = helloColor(nowMs);
    for (uint16_t i = 0; i < PANEL_LEDS; i++) panel[i] = c;
}

// ---------------------------------------------------------------------------
// 5x7 dot-matrix font (column-major, LSB = top row). Subset of FONT5x7 from
// simulator/index.html — only what 'HELLO' needs plus space.
// ---------------------------------------------------------------------------
struct Glyph { uint8_t cols[5]; };

static const Glyph FONT_SPACE = {{0x00, 0x00, 0x00, 0x00, 0x00}};
static const Glyph FONT_H     = {{0x7F, 0x08, 0x08, 0x08, 0x7F}};
static const Glyph FONT_E     = {{0x7F, 0x49, 0x49, 0x49, 0x41}};
static const Glyph FONT_L     = {{0x7F, 0x40, 0x40, 0x40, 0x40}};
static const Glyph FONT_O     = {{0x3E, 0x41, 0x41, 0x41, 0x3E}};

static const Glyph* glyphFor(char c) {
    switch (c) {
        case 'H': case 'h': return &FONT_H;
        case 'E': case 'e': return &FONT_E;
        case 'L': case 'l': return &FONT_L;
        case 'O': case 'o': return &FONT_O;
        default:            return &FONT_SPACE;
    }
}

// trimmed glyph width (skips leading/trailing zero columns) for even spacing
static uint8_t glyphWidth(char c) {
    const Glyph* g = glyphFor(c);
    if (c == ' ') return 3;
    int8_t start = 0, end = 4;
    while (start <= 4 && g->cols[start] == 0) start++;
    while (end   >= 0 && g->cols[end]   == 0) end--;
    if (start > end) return 1;
    return static_cast<uint8_t>(end - start + 1);
}

// Draw a glyph at logical (gx, gy). Returns advance width.
static uint8_t drawGlyphRGB(CRGB* frame, uint8_t fw, uint8_t fh,
                            char ch, int16_t gx, uint8_t gy, const CRGB& col) {
    const Glyph* g = glyphFor(ch);
    int8_t start = 0, end = 4;
    if (ch != ' ') {
        while (start <= 4 && g->cols[start] == 0) start++;
        while (end   >= 0 && g->cols[end]   == 0) end--;
        if (start > end) return 1;
    } else {
        return 3;
    }
    const uint8_t w = static_cast<uint8_t>(end - start + 1);
    for (uint8_t cx = 0; cx < w; cx++) {
        const uint8_t bits = g->cols[start + cx];
        const int16_t fx = gx + cx;
        if (fx < 0 || fx >= static_cast<int16_t>(fw)) continue;
        for (uint8_t ry = 0; ry < 7; ry++) {
            if (bits & (1 << ry)) {
                const uint8_t fy = gy + ry;
                if (fy >= fh) continue;
                frame[static_cast<uint16_t>(fy) * fw + fx] = col;
            }
        }
    }
    return w;
}

// Render the "HELLO " marquee scrolling right-to-left into a (fw x fh) frame.
// `scrollPx` increases over time; total cycle = textWidth + fw.
static void renderHelloMarquee(CRGB* frame, uint8_t fw, uint8_t fh,
                               uint32_t nowMs) {
    // clear
    for (uint16_t i = 0; i < static_cast<uint16_t>(fw) * fh; i++) frame[i] = CRGB::Black;

    static const char text[] = "HELLO ";
    const uint8_t  gap = 1;
    uint8_t totalW = 0;
    for (size_t i = 0; i < sizeof(text) - 1; i++) totalW += glyphWidth(text[i]) + gap;

    const uint32_t cycle = static_cast<uint32_t>(totalW + fw);
    const uint32_t scroll = (nowMs / 80) % cycle;     // ~12.5 cols/sec

    int16_t cursor = static_cast<int16_t>(fw) - static_cast<int16_t>(scroll);
    const CRGB col = helloColor(nowMs);
    for (size_t i = 0; i < sizeof(text) - 1; i++) {
        const uint8_t w = glyphWidth(text[i]);
        if (cursor + w >= 0 && cursor < static_cast<int16_t>(fw)) {
            drawGlyphRGB(frame, fw, fh, text[i], cursor, 0, col);
        }
        cursor += w + gap;
    }
}


// ===========================================================================
// MASTER ROLE
// ===========================================================================
#ifdef LUMEHEAD_ROLE_MASTER

static constexpr uint8_t  MATRIX_COLS = 16;
static constexpr uint8_t  MATRIX_ROWS = 8;
static constexpr uint16_t FRAME_LEDS  = MATRIX_COLS * MATRIX_ROWS;  // 128

// Logical frame (rendered, then split between the two panels).
static CRGB g_frame[FRAME_LEDS];

enum Mode : uint8_t {
    MODE_OFF      = 0,
    MODE_MARQUEE  = 1,
    MODE_STATIC   = 2,
    MODE_PROGRESS = 3,
    MODE_PULSE    = 4,
    MODE_HEATING  = 5,
    MODE_PRINTING = 6,
    MODE_LEVELING = 7,
};

struct DisplayState {
    volatile uint8_t mode       = MODE_OFF;
    volatile uint8_t progress   = 0;     // 0..255
    volatile uint8_t brightness = 64;    // 0..255
    CRGB             color      = CRGB(255, 51, 68);
    char             text[64]   = "LUMEHEAD";
    volatile uint8_t textLen    = 8;
};
static DisplayState g_state;

// Host-facing I2C receive buffer (filled in ISR, drained in loop()).
static volatile uint8_t g_rxBuf[80];
static volatile size_t  g_rxLen   = 0;
static volatile bool    g_rxReady = false;

// True until the first host I2C command lands. While true, both panels show
// the boot "hello world" hue sweep instead of any rendered frame.
static volatile bool    g_helloMode = true;

static inline void setFramePixel(uint8_t x, uint8_t y, const CRGB& c) {
    if (x >= MATRIX_COLS || y >= MATRIX_ROWS) return;
    g_frame[static_cast<uint16_t>(y) * MATRIX_COLS + x] = c;
}

// Host I2C callbacks ---------------------------------------------------------
static void onHostReceive(int /*numBytes*/) {
    size_t n = 0;
    while (Wire.available() && n < sizeof(g_rxBuf)) {
        g_rxBuf[n++] = Wire.read();
    }
    g_rxLen   = n;
    g_rxReady = true;
    g_helloMode = false;
}

static void onHostRequest() {
    uint8_t status[3] = {
        g_state.mode, g_state.progress, g_state.brightness,
    };
    Wire.write(status, sizeof(status));
}

static void handleHostCommand(const uint8_t* buf, size_t len) {
    if (len == 0) return;
    switch (buf[0]) {
        case 0x01: if (len >= 2) g_state.mode       = buf[1]; break;
        case 0x02: if (len >= 2) g_state.progress   = buf[1]; break;
        case 0x03: if (len >= 4) g_state.color      = CRGB(buf[1], buf[2], buf[3]); break;
        case 0x04:
            if (len >= 2) {
                g_state.brightness = buf[1];
                FastLED.setBrightness(g_state.brightness);
            }
            break;
        case 0x05: {
            const size_t tlen = min<size_t>(len - 1, sizeof(g_state.text) - 1);
            memcpy(const_cast<char*>(g_state.text), buf + 1, tlen);
            const_cast<char*>(g_state.text)[tlen] = '\0';
            g_state.textLen = static_cast<uint8_t>(tlen);
            break;
        }
        case 0xFF: g_state.mode = MODE_OFF; break;
        default:   log_w("unknown host cmd 0x%02X", buf[0]); break;
    }
}

// Render placeholder ---------------------------------------------------------
// Mirrors simulator/index.html — port more visualizations as needed.
static void render(uint32_t nowMs) {
    memset(g_frame, 0, sizeof(g_frame));

    switch (g_state.mode) {
        case MODE_PROGRESS: {
            const uint8_t filled = (g_state.progress * MATRIX_COLS + 127) / 255;
            const bool    flash  = (nowMs / 125) & 1;
            for (uint8_t x = 0; x < filled; x++) {
                if ((x == filled - 1) && !flash) continue;
                for (uint8_t y = 0; y < MATRIX_ROWS; y++) {
                    setFramePixel(x, y, g_state.color);
                }
            }
            break;
        }
        case MODE_PULSE: {
            const uint8_t b = sin8(nowMs / 4);
            CRGB c = g_state.color;
            c.nscale8_video(b);
            for (uint16_t i = 0; i < FRAME_LEDS; i++) g_frame[i] = c;
            break;
        }
        case MODE_OFF:
        default:
            break;
    }
}

// Push the right half (columns 8..15) to the slave over Wire1.
// Sent row-by-row (24-byte payload each) to stay well under typical I2C
// driver buffer sizes.
static void pushSlaveFrame() {
    uint8_t beginBuf[2] = { CMD_FRAME_BEGIN, FastLED.getBrightness() };
    Wire1.beginTransmission(I2C_ADDR_SLAVE);
    Wire1.write(beginBuf, sizeof(beginBuf));
    Wire1.endTransmission();

    uint8_t row[1 + 1 + PANEL_WIDTH * 3];  // cmd, rowIdx, RGB*8
    row[0] = CMD_FRAME_ROW;
    for (uint8_t y = 0; y < MATRIX_ROWS; y++) {
        row[1] = y;
        for (uint8_t lx = 0; lx < PANEL_WIDTH; lx++) {
            const uint8_t gx = lx + PANEL_WIDTH;        // 8..15 in logical frame
            const CRGB    c  = g_frame[static_cast<uint16_t>(y) * MATRIX_COLS + gx];
            // Send unscaled pixels so the slave preserves full precision for temporal dithering
            row[2 + lx * 3 + 0] = c.r;
            row[2 + lx * 3 + 1] = c.g;
            row[2 + lx * 3 + 2] = c.b;
        }
        Wire1.beginTransmission(I2C_ADDR_SLAVE);
        Wire1.write(row, sizeof(row));
        Wire1.endTransmission();
    }

    Wire1.beginTransmission(I2C_ADDR_SLAVE);
    Wire1.write(CMD_FRAME_END);
    Wire1.endTransmission();
}

// Copy the master's left half (columns 0..7) into its onboard panel buffer.
static void blitLocalPanel() {
    for (uint8_t y = 0; y < MATRIX_ROWS; y++) {
        for (uint8_t x = 0; x < PANEL_WIDTH; x++) {
            g_panel[panelIndex(x, y)] =
                g_frame[static_cast<uint16_t>(y) * MATRIX_COLS + x];
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println(F("lumehead MASTER booting..."));

    FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(g_panel, PANEL_LEDS);
    FastLED.setBrightness(g_state.brightness);
    FastLED.clear(true);

    // Host-facing slave on Wire
    Wire.onReceive(onHostReceive);
    Wire.onRequest(onHostRequest);
    Wire.begin(static_cast<uint8_t>(I2C_ADDR_MASTER), HOST_I2C_SDA, HOST_I2C_SCL,
               100000);
    Serial.printf("Host I2C slave ready @ 0x%02X (SDA=%d SCL=%d)\n",
                  I2C_ADDR_MASTER, HOST_I2C_SDA, HOST_I2C_SCL);

    // Inter-board master on Wire1
    Wire1.begin(INTER_I2C_SDA, INTER_I2C_SCL, INTER_I2C_FREQ);
    Serial.printf("Inter-board I2C master on Wire1 (SDA=%d SCL=%d @ %lu Hz)\n",
                  INTER_I2C_SDA, INTER_I2C_SCL, (unsigned long)INTER_I2C_FREQ);
}

void loop() {
    if (g_rxReady) {
        noInterrupts();
        uint8_t buf[sizeof(g_rxBuf)];
        const size_t len = g_rxLen;
        memcpy(buf, const_cast<const uint8_t*>(g_rxBuf), len);
        g_rxReady = false;
        interrupts();
        handleHostCommand(buf, len);
    }

    const uint32_t now = millis();
    if (g_helloMode) {
        FastLED.setBrightness(32);
        // Cycle animations every 5 seconds (5000 ms)
        const uint32_t cyclePhase = (now / 5000) % 5;
        if (cyclePhase == 0) {
            // 1. Marquee "HELLO"
            renderHelloMarquee(g_frame, MATRIX_COLS, MATRIX_ROWS, now);
        } else if (cyclePhase == 1) {
            // 2. Pulse animation using the dynamic hello hue
            const uint8_t b = sin8(now / 4);
            CRGB          c = helloColor(now);
            c.nscale8_video(b);
            for (uint16_t i = 0; i < FRAME_LEDS; i++) g_frame[i] = c;
        } else if (cyclePhase == 2) {
            // 3. Sweeping Progress bar filling from left to right
            memset(g_frame, 0, sizeof(g_frame));
            const uint8_t filled = ((now % 5000) * MATRIX_COLS) / 5000;
            const bool    flash  = (now / 125) & 1;
            CRGB          col    = helloColor(now);
            for (uint8_t x = 0; x < filled; x++) {
                if ((x == filled - 1) && !flash) continue;
                for (uint8_t y = 0; y < MATRIX_ROWS; y++) {
                    setFramePixel(x, y, col);
                }
            }
        } else if (cyclePhase == 3) {
            // 4. Heating Up animation (Thermometer with rising mercury + radiating heat waves)
            memset(g_frame, 0, sizeof(g_frame));
            CRGB col = helloColor(now);
            // Glass outline
            for (uint8_t y = 1; y <= 4; y++) { setFramePixel(2, y, col); setFramePixel(4, y, col); }
            setFramePixel(2, 0, col); setFramePixel(3, 0, col); setFramePixel(4, 0, col);
            setFramePixel(1, 5, col); setFramePixel(2, 5, col); setFramePixel(4, 5, col); setFramePixel(5, 5, col);
            setFramePixel(1, 6, col); setFramePixel(5, 6, col);
            for (uint8_t x = 1; x <= 5; x++) setFramePixel(x, 7, col);
            
            // Mercury bulb base fill
            setFramePixel(2, 6, col); setFramePixel(3, 6, col); setFramePixel(4, 6, col);
            // Stem fill based on sine wave
            const float tSec = now / 1000.0f;
            const float fillLevel = sin(tSec / 0.8f) * 0.5f + 0.5f;
            const uint8_t fillRows = static_cast<uint8_t>(round(fillLevel * 5.0f));
            for (uint8_t i = 0; i < fillRows; i++) {
                setFramePixel(3, 5 - i, col);
            }
            // Radiating heat waves
            const float tt = tSec * 5.0f;
            for (uint8_t w = 0; w < 3; w++) {
                const int base_y = 1 + w * 2;
                for (uint8_t x = 8; x < MATRIX_COLS; x++) {
                    const float phase = (x - 8) * 0.9f + tt + w * 1.2f;
                    const int y = base_y + static_cast<int>(round(sin(phase)));
                    if (y >= 0 && y < MATRIX_ROWS) {
                        setFramePixel(x, static_cast<uint8_t>(y), col);
                    }
                }
            }
        } else {
            // 5. Leveling animation (Print bed reference line + sliding probe head with drift/wobble)
            memset(g_frame, 0, sizeof(g_frame));
            CRGB col = helloColor(now);
            CRGB colDim = col; colDim.nscale8_video(102); // ~0.4 brightness
            CRGB colMid = col; colMid.nscale8_video(178); // ~0.7 brightness
            
            // Print bed reference line
            for (uint8_t x = 0; x < MATRIX_COLS; x++) {
                setFramePixel(x, 3, colDim);
                setFramePixel(x, 4, colDim);
            }
            setFramePixel(0, 2, col); setFramePixel(0, 5, col);
            setFramePixel(MATRIX_COLS - 1, 2, col); setFramePixel(MATRIX_COLS - 1, 5, col);
            
            const uint8_t cx = MATRIX_COLS / 2; // 8
            setFramePixel(cx - 1, 1, colMid); setFramePixel(cx, 1, colMid);
            setFramePixel(cx - 1, 6, colMid); setFramePixel(cx, 6, colMid);
            
            // Sliding probe head
            const float tSec = now / 1000.0f;
            const float drift = sin(tSec / 0.9f) * 5.0f;
            const float wobble = sin(tSec / 0.13f) * 0.4f;
            const int bx = static_cast<int>(round((cx - 1) + drift + wobble));
            
            CRGB colProbeTop = col; colProbeTop.nscale8_video(204); // ~0.8 brightness
            if (bx >= 0 && bx < MATRIX_COLS) {
                setFramePixel(static_cast<uint8_t>(bx), 3, col);
                setFramePixel(static_cast<uint8_t>(bx), 4, col);
                setFramePixel(static_cast<uint8_t>(bx), 2, colProbeTop);
            }
            if (bx + 1 >= 0 && bx + 1 < MATRIX_COLS) {
                setFramePixel(static_cast<uint8_t>(bx + 1), 3, col);
                setFramePixel(static_cast<uint8_t>(bx + 1), 4, col);
                setFramePixel(static_cast<uint8_t>(bx + 1), 2, colProbeTop);
            }
        }
    } else {
        FastLED.setBrightness(g_state.brightness);
        render(now);
    }
    blitLocalPanel();
    pushSlaveFrame();
    FastLED.show();

    delay(1000 / 30);  // ~30 fps cap (cheap; tune as needed)
}

#endif  // LUMEHEAD_ROLE_MASTER


// ===========================================================================
// SLAVE ROLE
// ===========================================================================
#ifdef LUMEHEAD_ROLE_SLAVE

// Back buffer filled by I2C ISR; flipped to g_panel on FRAME_END.
static volatile CRGB g_back[PANEL_LEDS];
static volatile bool g_frameReady = false;

// True until the first complete frame arrives from the master.
static volatile bool g_helloMode = true;

// Target brightness received from master via CMD_FRAME_BEGIN to keep temporal dithering perfectly symmetric.
static volatile uint8_t g_slaveBrightness = 32;

static void onSlaveReceive(int numBytes) {
    if (numBytes <= 0) return;
    const uint8_t cmd = Wire.read();
    switch (cmd) {
        case CMD_FRAME_BEGIN:
            if (numBytes >= 2) {
                g_slaveBrightness = Wire.read();
            }
            while (Wire.available()) Wire.read();
            break;

        case CMD_FRAME_ROW: {
            if (numBytes < 1 + 1 + PANEL_WIDTH * 3) {
                while (Wire.available()) Wire.read();
                return;
            }
            const uint8_t y = Wire.read();
            if (y >= PANEL_HEIGHT) {
                while (Wire.available()) Wire.read();
                return;
            }
            CRGB* back = const_cast<CRGB*>(g_back);
            for (uint8_t x = 0; x < PANEL_WIDTH; x++) {
                const uint8_t r = Wire.read();
                const uint8_t g = Wire.read();
                const uint8_t b = Wire.read();
                back[panelIndex(x, y)] = CRGB(r, g, b);
            }
            // discard any extra
            while (Wire.available()) Wire.read();
            break;
        }

        case CMD_FRAME_END:
            g_frameReady = true;
            g_helloMode  = false;
            while (Wire.available()) Wire.read();
            break;

        default:
            while (Wire.available()) Wire.read();
            break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println(F("lumehead SLAVE booting..."));

    FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(g_panel, PANEL_LEDS);
    FastLED.setBrightness(32);     // start dim for boot preview; synced to master via CMD_FRAME_BEGIN
    FastLED.clear(true);

    Wire.setBufferSize(64);        // > 1 + 1 + 8*3 = 26
    Wire.onReceive(onSlaveReceive);
    Wire.begin(static_cast<uint8_t>(I2C_ADDR_SLAVE), INTER_I2C_SDA, INTER_I2C_SCL,
               INTER_I2C_FREQ);
    Serial.printf("Inter-board I2C slave id=%d ready @ 0x%02X (SDA=%d SCL=%d)\n",
                  LUMEHEAD_SLAVE_ID, I2C_ADDR_SLAVE, INTER_I2C_SDA, INTER_I2C_SCL);
}

void loop() {
    if (g_frameReady) {
        noInterrupts();
        memcpy(g_panel, const_cast<const CRGB*>(g_back), sizeof(g_panel));
        const uint8_t targetBrightness = g_slaveBrightness;
        g_frameReady = false;
        interrupts();
        FastLED.setBrightness(targetBrightness);
        FastLED.show();
    } else if (g_helloMode) {
        fillPanelHello(g_panel, millis());
        FastLED.show();
        delay(1000 / 30);
        return;
    }
    delay(1);
}

#endif  // LUMEHEAD_ROLE_SLAVE


// ===========================================================================
// Build sanity check
// ===========================================================================
#if !defined(LUMEHEAD_ROLE_MASTER) && !defined(LUMEHEAD_ROLE_SLAVE)
#error "Define LUMEHEAD_ROLE_MASTER or LUMEHEAD_ROLE_SLAVE (set via PlatformIO env)"
#endif
#if defined(LUMEHEAD_ROLE_MASTER) && defined(LUMEHEAD_ROLE_SLAVE)
#error "Only one of LUMEHEAD_ROLE_MASTER / LUMEHEAD_ROLE_SLAVE may be defined"
#endif
