/*
 * lumehead — ESP32-S3 firmware
 *
 * Drives a 16x8 WS2812 LED matrix (two chained 8x8 Waveshare panels) and
 * receives display commands over I2C as a slave at address 0x42.
 *
 * The visualization logic mirrors simulator/index.html and the Klipper
 * plugin in klipper/led_matrix_display.py — placeholder here, to be ported
 * incrementally.
 */

#include <Arduino.h>
#include <FastLED.h>
#include <Wire.h>

// ---------------------------------------------------------------------------
// Hardware configuration
// ---------------------------------------------------------------------------
static constexpr uint8_t  I2C_ADDR        = 0x42;
static constexpr int      I2C_SDA_PIN     = 8;     // adjust to your wiring
static constexpr int      I2C_SCL_PIN     = 9;     // adjust to your wiring
static constexpr uint32_t I2C_FREQ_HZ     = 100000;

static constexpr int      LED_DATA_PIN    = 4;     // adjust to your wiring
static constexpr uint8_t  MATRIX_COLS     = 16;
static constexpr uint8_t  MATRIX_ROWS     = 8;
static constexpr uint8_t  PANEL_WIDTH     = 8;
static constexpr uint16_t NUM_LEDS        = MATRIX_COLS * MATRIX_ROWS;  // 128

static CRGB g_leds[NUM_LEDS];

// ---------------------------------------------------------------------------
// I2C command protocol (placeholder — extend as visualizations are added)
//
// Frame: [CMD][PAYLOAD...]
//   0x01 SET_MODE      payload: 1 byte mode id
//   0x02 SET_PROGRESS  payload: 1 byte 0..255
//   0x03 SET_COLOR     payload: 3 bytes R,G,B
//   0x04 SET_BRIGHTNESS payload: 1 byte 0..255
//   0x05 SET_TEXT      payload: N bytes ASCII (max 63)
//   0xFF CLEAR
// ---------------------------------------------------------------------------
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
    volatile uint8_t progress   = 0;       // 0..255
    volatile uint8_t brightness = 64;      // 0..255
    CRGB             color      = CRGB(255, 51, 68);
    char             text[64]   = "LUMEHEAD";
    volatile uint8_t textLen    = 8;
};
static DisplayState g_state;

// I2C receive buffer (filled in ISR context)
static volatile uint8_t g_rxBuf[80];
static volatile size_t  g_rxLen = 0;
static volatile bool    g_rxReady = false;

// ---------------------------------------------------------------------------
// Coordinate mapper — mirrors mapCoord() in simulator/index.html.
// (x,y) -> chain index in the WS2812 strip.
// ---------------------------------------------------------------------------
static inline uint16_t coordToIndex(uint8_t x, uint8_t y) {
    const uint8_t  panel  = x / PANEL_WIDTH;
    const uint8_t  localX = x % PANEL_WIDTH;
    const uint16_t within = static_cast<uint16_t>(y) * PANEL_WIDTH + localX;
    return panel * (PANEL_WIDTH * MATRIX_ROWS) + within;
}

static inline void setPixel(uint8_t x, uint8_t y, const CRGB& c) {
    if (x >= MATRIX_COLS || y >= MATRIX_ROWS) return;
    g_leds[coordToIndex(x, y)] = c;
}

// ---------------------------------------------------------------------------
// I2C slave callbacks (run in interrupt context — keep work minimal)
// ---------------------------------------------------------------------------
static void onI2CReceive(int numBytes) {
    size_t n = 0;
    while (Wire.available() && n < sizeof(g_rxBuf)) {
        g_rxBuf[n++] = Wire.read();
    }
    g_rxLen   = n;
    g_rxReady = true;
}

static void onI2CRequest() {
    // Report status: [mode, progress, brightness]
    uint8_t status[3] = {
        g_state.mode, g_state.progress, g_state.brightness,
    };
    Wire.write(status, sizeof(status));
}

// ---------------------------------------------------------------------------
// Command dispatch (runs in loop context)
// ---------------------------------------------------------------------------
static void handleCommand(const uint8_t* buf, size_t len) {
    if (len == 0) return;
    const uint8_t cmd = buf[0];
    switch (cmd) {
        case 0x01:  // SET_MODE
            if (len >= 2) g_state.mode = buf[1];
            break;
        case 0x02:  // SET_PROGRESS
            if (len >= 2) g_state.progress = buf[1];
            break;
        case 0x03:  // SET_COLOR
            if (len >= 4) g_state.color = CRGB(buf[1], buf[2], buf[3]);
            break;
        case 0x04:  // SET_BRIGHTNESS
            if (len >= 2) {
                g_state.brightness = buf[1];
                FastLED.setBrightness(g_state.brightness);
            }
            break;
        case 0x05: {  // SET_TEXT
            const size_t tlen = min<size_t>(len - 1, sizeof(g_state.text) - 1);
            memcpy(const_cast<char*>(g_state.text), buf + 1, tlen);
            const_cast<char*>(g_state.text)[tlen] = '\0';
            g_state.textLen = static_cast<uint8_t>(tlen);
            break;
        }
        case 0xFF:  // CLEAR
            g_state.mode = MODE_OFF;
            break;
        default:
            log_w("unknown I2C cmd 0x%02X", cmd);
            break;
    }
}

// ---------------------------------------------------------------------------
// Render — placeholder. Replace with full visualizations from the simulator.
// ---------------------------------------------------------------------------
static void render(uint32_t nowMs) {
    FastLED.clear();

    switch (g_state.mode) {
        case MODE_PROGRESS: {
            const uint8_t filled = (g_state.progress * MATRIX_COLS + 127) / 255;
            const bool    flash  = (nowMs / 125) & 1;
            for (uint8_t x = 0; x < filled; x++) {
                const bool leading = (x == filled - 1);
                if (leading && !flash) continue;
                for (uint8_t y = 0; y < MATRIX_ROWS; y++) {
                    setPixel(x, y, g_state.color);
                }
            }
            break;
        }
        case MODE_PULSE: {
            const uint8_t b = sin8(nowMs / 4);  // 0..255
            CRGB c = g_state.color;
            c.nscale8_video(b);
            for (uint16_t i = 0; i < NUM_LEDS; i++) g_leds[i] = c;
            break;
        }
        case MODE_OFF:
        default:
            // already cleared
            break;
    }

    FastLED.show();
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println(F("lumehead firmware booting..."));

    FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(g_leds, NUM_LEDS);
    FastLED.setBrightness(g_state.brightness);
    FastLED.clear(true);

    Wire.onReceive(onI2CReceive);
    Wire.onRequest(onI2CRequest);
    if (!Wire.begin(static_cast<uint8_t>(I2C_ADDR), I2C_SDA_PIN, I2C_SCL_PIN,
                    I2C_FREQ_HZ)) {
        Serial.println(F("I2C slave init failed"));
    } else {
        Serial.printf("I2C slave ready @ 0x%02X (SDA=%d SCL=%d)\n",
                      I2C_ADDR, I2C_SDA_PIN, I2C_SCL_PIN);
    }
}

void loop() {
    if (g_rxReady) {
        noInterrupts();
        uint8_t buf[sizeof(g_rxBuf)];
        const size_t len = g_rxLen;
        memcpy(buf, const_cast<const uint8_t*>(g_rxBuf), len);
        g_rxReady = false;
        interrupts();
        handleCommand(buf, len);
    }

    render(millis());
    FastLED.delay(1000 / 60);  // ~60 FPS cap
}
