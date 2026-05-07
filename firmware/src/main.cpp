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
 * Pin map (Waveshare ESP32-S3-Matrix; adjust if your board differs):
 *   onboard 8x8 panel data : GPIO 14  (hard-wired on the dev board)
 *   host  I2C SDA / SCL    : GPIO 8 / 9   (master only)
 *   inter-board SDA / SCL  : GPIO 1 / 2   (master Wire1 <-> slave Wire)
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

static constexpr int      LED_DATA_PIN = 14;   // Waveshare onboard panel
static CRGB g_panel[PANEL_LEDS];

// I2C addresses
static constexpr uint8_t  I2C_ADDR_MASTER = 0x42;   // host -> master
static constexpr uint8_t  I2C_ADDR_SLAVE  = 0x43;   // master -> slave

// I2C pins
static constexpr int      HOST_I2C_SDA   = 8;
static constexpr int      HOST_I2C_SCL   = 9;
static constexpr int      INTER_I2C_SDA  = 1;
static constexpr int      INTER_I2C_SCL  = 2;
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
    Wire1.beginTransmission(I2C_ADDR_SLAVE);
    Wire1.write(CMD_FRAME_BEGIN);
    Wire1.endTransmission();

    uint8_t row[1 + 1 + PANEL_WIDTH * 3];  // cmd, rowIdx, RGB*8
    row[0] = CMD_FRAME_ROW;
    for (uint8_t y = 0; y < MATRIX_ROWS; y++) {
        row[1] = y;
        for (uint8_t lx = 0; lx < PANEL_WIDTH; lx++) {
            const uint8_t gx = lx + PANEL_WIDTH;        // 8..15 in logical frame
            const CRGB&   c  = g_frame[static_cast<uint16_t>(y) * MATRIX_COLS + gx];
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

    render(millis());
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

static void onSlaveReceive(int numBytes) {
    if (numBytes <= 0) return;
    const uint8_t cmd = Wire.read();
    switch (cmd) {
        case CMD_FRAME_BEGIN:
            // Nothing to reset; we accept any row index.
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
            for (uint8_t x = 0; x < PANEL_WIDTH; x++) {
                const uint8_t r = Wire.read();
                const uint8_t g = Wire.read();
                const uint8_t b = Wire.read();
                g_back[panelIndex(x, y)] = CRGB(r, g, b);
            }
            // discard any extra
            while (Wire.available()) Wire.read();
            break;
        }

        case CMD_FRAME_END:
            g_frameReady = true;
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
    FastLED.setBrightness(255);    // master already scales colors
    FastLED.clear(true);

    Wire.setBufferSize(64);        // > 1 + 1 + 8*3 = 26
    Wire.onReceive(onSlaveReceive);
    Wire.begin(static_cast<uint8_t>(I2C_ADDR_SLAVE), INTER_I2C_SDA, INTER_I2C_SCL,
               INTER_I2C_FREQ);
    Serial.printf("Inter-board I2C slave ready @ 0x%02X (SDA=%d SCL=%d)\n",
                  I2C_ADDR_SLAVE, INTER_I2C_SDA, INTER_I2C_SCL);
}

void loop() {
    if (g_frameReady) {
        noInterrupts();
        memcpy(g_panel, const_cast<const CRGB*>(g_back), sizeof(g_panel));
        g_frameReady = false;
        interrupts();
        FastLED.show();
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
