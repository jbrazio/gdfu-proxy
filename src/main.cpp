#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <Wire.h>

#include <array>
#include <string>

#include "gdfu_config.h"
#include "gdfu_file.h"
#include "gdfu_frame.h"
#include "gdfu_proxy_state.h"
#include "gdfu_radio_settings.h"
#include "oled_schedule.h"
#include "standby_button.h"
#if defined(PROXY_RADIO_SX1276)
#include "sx1276.h"
#define RADIO_STATUS_OK SX1276_OK
#define RADIO_STATUS_TIMEOUT SX1276_ERR_TIMEOUT
#define RADIO_STATUS_BUSY SX1276_ERR_BUSY
#define RADIO_STATUS_CRC SX1276_ERR_CRC
#else
#include "sx1262.h"
#define RADIO_STATUS_OK SX1262_OK
#define RADIO_STATUS_TIMEOUT SX1262_ERR_TIMEOUT
#define RADIO_STATUS_BUSY SX1262_ERR_BUSY
#define RADIO_STATUS_CRC SX1262_ERR_CRC
#endif

#define SERVICE_UUID "7e4c2a10-8b31-4f9d-a600-000000000000"
#define CHAR_VERSION_UUID "7e4c2a10-8b31-4f9d-a600-000000000001" // Core: 0001-000f
#define CHAR_RELEASE_UUID "7e4c2a10-8b31-4f9d-a600-000000000002"
#define CHAR_CONFIG_UUID "7e4c2a10-8b31-4f9d-a600-000000000010"  // Radio: 0010-001f
#define CHAR_TX_POWER_UUID "7e4c2a10-8b31-4f9d-a600-000000000011"
#define CHAR_DISCOVERY_UUID "7e4c2a10-8b31-4f9d-a600-000000000020" // Discovery: 0020-002f
#define CHAR_COMMAND_UUID "7e4c2a10-8b31-4f9d-a600-000000000030"   // DFU transport: 0030-003f
#define CHAR_DATA_UUID "7e4c2a10-8b31-4f9d-a600-000000000031"
#define CHAR_STATUS_UUID "7e4c2a10-8b31-4f9d-a600-000000000032"
#define CHAR_FILE_COMMAND_UUID "7e4c2a10-8b31-4f9d-a600-000000000040" // File management: 0040-004f
#define CHAR_FILE_DATA_UUID "7e4c2a10-8b31-4f9d-a600-000000000041"
#define CHAR_FILE_STATUS_UUID "7e4c2a10-8b31-4f9d-a600-000000000042"
#define CHAR_FILE_STORAGE_UUID "7e4c2a10-8b31-4f9d-a600-000000000043"

#ifndef DEBUG
#define DEBUG 0
#endif

#ifndef PROXY_RELEASE_VERSION
#define PROXY_RELEASE_VERSION "0.0.0-unknown"
#endif

#if DEBUG
#define DEBUG_SERIAL(...) Serial.printf(__VA_ARGS__)
#define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
#define DEBUG_SERIAL(...) do {} while (0)
#define DEBUG_PRINTLN(...) do {} while (0)
#endif

#if defined(PROXY_RADIO_SX1276)
#define PIN_NSS 18
#define PIN_BUSY SX1276_PIN_NC
#define PIN_DIO1 26
#define PIN_NRST 14
#define PIN_SCK 5
#define PIN_MOSI 27
#define PIN_MISO 19
#define PIN_VEXT SX1276_PIN_NC
#define PIN_OLED_RST -1 // ESP32-PICO-D4 GPIO16/17 are its flash pins; driving 16 trips TG1WDT.
#define PIN_OLED_SCL 22
#define PIN_OLED_SDA 21
#define PIN_USER_BUTTON 0 // TTGO LoRa32 V1.6 user button, active low.
#else
#define PIN_NSS 8
#define PIN_BUSY 13
#define PIN_DIO1 14
#define PIN_NRST SX1262_PIN_NC
#define PIN_SCK 9
#define PIN_MOSI 10
#define PIN_MISO 11
#define PIN_VEXT 36
#define PIN_OLED_RST 21
#define PIN_OLED_SCL 18
#define PIN_OLED_SDA 17
#define PIN_USER_BUTTON 0 // MeshCore Heltec V3 user button, active low.
#endif
#define OLED_ADDR 0x3C

#if defined(PROXY_RADIO_SX1276)
#define RADIO_SET_CONFIG sx1276_set_config
#define RADIO_INIT sx1276_init
#define RADIO_SLEEP sx1276_sleep
#define RADIO_SEND_START sx1276_send_start
#define RADIO_SEND_POLL sx1276_send_poll
#define RADIO_START_RECEIVE sx1276_start_receive
#define RADIO_POLL_IRQ sx1276_poll_irq
#define RADIO_READ_RECEIVED sx1276_read_received
#define RADIO_CONFIG_TYPE SX1276Config
#else
#define RADIO_SET_CONFIG sx1262_set_config
#define RADIO_INIT sx1262_init
#define RADIO_SLEEP sx1262_sleep
#define RADIO_SEND_START sx1262_send_start
#define RADIO_SEND_POLL sx1262_send_poll
#define RADIO_START_RECEIVE sx1262_start_receive
#define RADIO_POLL_IRQ sx1262_poll_irq
#define RADIO_READ_RECEIVED sx1262_read_received
#define RADIO_CONFIG_TYPE SX1262Config
#endif

namespace {
constexpr uint8_t STATUS_IDLE = 0x01;
constexpr uint8_t STATUS_BUSY = 0x02;
constexpr uint8_t STATUS_STREAMING = 0x03;
constexpr uint8_t STATUS_ERROR = 0x04;
constexpr uint8_t ERROR_NONE = 0x00;
constexpr uint8_t ERROR_RADIO = 0x0D;
constexpr size_t MAX_BLE_CHUNK = GDFU_MAX_PAYLOAD;
constexpr size_t MAX_QUEUE_CHUNKS = 8;
constexpr size_t MAX_RF_FRAME = GDFU_MAX_FRAME_SIZE;
constexpr uint32_t STANDBY_HOLD_MS = 3000;

struct PendingFrame {
    std::array<uint8_t, MAX_BLE_CHUNK> data{};
    size_t length = 0;
};

BLEServer *pServer = nullptr;
BLECharacteristic *pCharStatus = nullptr;
BLECharacteristic *pCharDiscovery = nullptr;
BLECharacteristic *pCharConfig = nullptr;
BLECharacteristic *pCharCommand = nullptr;
BLECharacteristic *pCharTxPower = nullptr;

bool deviceConnected = false;
volatile bool abortOnDisconnect = false;
bool radioReady = false;
bool rxActive = false;
uint32_t rxFailStreak = 0;
std::array<PendingFrame, MAX_QUEUE_CHUNKS> queue{};
volatile size_t queueCount = 0;
// BLE task pushes, loop() pops/shifts: unlocked, a push during the shift
// duplicates one chunk and drops the next (same length, CRC16 NACK at FINISH).
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

// Non-blocking RF operation state (see sendGdfuFrame / serviceRfOps).
enum RfOpKind : uint8_t { RF_OP_NONE = 0, RF_OP_DATA, RF_OP_CONTROL };
enum RfOpStage : uint8_t { RF_STAGE_TX = 0, RF_STAGE_ACK, RF_STAGE_RETRY };
enum CmdState : uint8_t {
    CMD_NONE = 0,
    CMD_START_WAIT,
    CMD_SETPOWER_WAIT,
    CMD_FINISH_WAIT,
    CMD_ABORT_WAIT,
};
RfOpKind rfOpKind = RF_OP_NONE;
RfOpStage rfOpStage = RF_STAGE_TX;
uint8_t rfOpFrame[GDFU_MAX_FRAME_SIZE];
size_t rfOpFrameLen = 0;
uint8_t rfOpType = 0;
uint8_t rfOpPayload[GDFU_MAX_PAYLOAD];
uint16_t rfOpPayloadLen = 0;
uint32_t rfOpTimeoutMs = 0;
uint8_t rfOpMaxRetries = 1;
uint8_t rfOpAttempt = 0;
uint32_t rfOpStageDeadline = 0;
uint32_t rfOpRetryDelayUntil = 0;
CmdState cmdState = CMD_NONE;
// Error to republish once a best-effort RF ABORT completes (ERROR_NONE: report IDLE).
uint8_t abortKeepError = 0;
// OLED-only latch: the app's cleanup ABORT clears last_error (BLE IDLE
// handshake), but the operator must still see why the run failed. Cleared on
// the next START_DFU.
uint8_t shownError = 0;
// gFSK RSSI (dBm) of the last ACK/NACK from the selected target; 0 = none yet
// (real readings are clamped to <= -1 so 0 stays unambiguous). Written from
// loop() and the BLE task; single byte, so volatile is enough on ESP32.
volatile int8_t lastAckRssi = 0;
// True once the target ACKed SET_TX_POWER: INIT/DATA writes are queued only then.
bool powerSynced = false;
volatile uint8_t rfDoneCmd = 0;
volatile bool rfDoneOk = false;
// DATA op completion, consumed by serviceRadioQueue().
volatile bool rfDataDonePending = false;
volatile bool rfDataDoneOk = false;
uint32_t accepted_bytes = 0;
uint32_t confirmed_bytes = 0;
uint32_t session_total_bytes =
    0; // image size parsed from the START payload; drives the progress bar

gdfu_proxy_state_t proxy_state;
uint8_t active_profile = GDFU_DEFAULT_PROFILE;
uint8_t txPowerDbm = GDFU_DEFAULT_TX_POWER_DBM;
Preferences radioPreferences;
bool oledReady = false;
uint32_t oledLastUpdateMs = 0;
bool oledDirty = true;
StandbyButton standbyButton(STANDBY_HOLD_MS);

volatile uint32_t errorCount = 0;

uint8_t oledFrame[8][128]; // RAM framebuffer; Wire is touched only in pushOled().

void oledCommand(uint8_t command) {
    Wire.beginTransmission(OLED_ADDR);
    Wire.write(0x00);
    Wire.write(command);
    Wire.endTransmission();
    delay(0); // Feed task watchdog while OLED bus recovers from a failed transaction.
}

void oledData(const uint8_t *data, size_t len) {
    while (len > 0) {
        const size_t count = len > 32 ? 32 : len;
        Wire.beginTransmission(OLED_ADDR);
        Wire.write(0x40);
        Wire.write(data, count);
        Wire.endTransmission();
#if defined(PROXY_RADIO_SX1276)
        uint16_t irq; // Keep draining the 66-byte RX FIFO while the I2C frame push blocks loop().
        sx1276_poll_irq(&irq);
#endif
        delay(0); // A complete 128x64 frame needs 32 I2C transactions.
        data += count;
        len -= count;
    }
}

const uint8_t *oledGlyph(char ch) {
    static const uint8_t blank[5] = {};
    static const uint8_t digits[10][5] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
        {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
    };
    static const uint8_t letters[26][5] = {
        {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
        {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
        {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
        {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
        {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
        {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
        {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
        {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
        {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
        {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
        {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
    };
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};
    static const uint8_t percent[5] = {0x63, 0x64, 0x08, 0x16, 0x23};
    static const uint8_t middot[5] = {0x00, 0x18, 0x18, 0x00, 0x00}; // '*' -> centered dot
    if (ch >= '0' && ch <= '9')
        return digits[ch - '0'];
    if (ch >= 'A' && ch <= 'Z')
        return letters[ch - 'A'];
    static const uint8_t lowercase[26][5] = {
        {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38},
        {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
        {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02},
        {0x0C,0x52,0x52,0x52,0x3E}, {0x7F,0x08,0x04,0x04,0x78},
        {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
        {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00},
        {0x7C,0x04,0x18,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78},
        {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
        {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08},
        {0x48,0x54,0x54,0x54,0x20}, {0x04,0x3F,0x44,0x40,0x20},
        {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
        {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44},
        {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44},
    };
    static const uint8_t plus[5] = {0x08,0x08,0x3E,0x08,0x08};
    if (ch == '+')
        return plus;
    if (ch >= 'a' && ch <= 'z')
        return lowercase[ch - 'a'];
    if (ch == ':')
        return colon;
    if (ch == '-')
        return dash;
    if (ch == '.')
        return dot;
    if (ch == '/')
        return slash;
    if (ch == '%')
        return percent;
    if (ch == '*')
        return middot;
    return blank;
}

// Draw 5x8 text into the framebuffer at pixel column x, page `page`.
// Glyphs are XORed on top of whatever is already there.
void fbText(uint8_t x, uint8_t page, const char *text) {
    while (*text && x < 127) {
        const uint8_t *glyph = oledGlyph(*text++);
        for (uint8_t i = 0; i < 5 && x < 128; ++i, ++x) {
            oledFrame[page][x] ^= glyph[i];
        }
        ++x; // 1px letter spacing (column already blank)
    }
}

// Draw the title bar: lit band with 2px dark padding above and below,
// dark text inside (inverted). Spans y=2..13 (pages 0-1).
void fbTitleBar(const char *text) {
    for (uint8_t x = 0; x < 128; ++x) {
        oledFrame[0][x] = 0xFC; // y2..y7 lit
        oledFrame[1][x] = 0x3F; // y8..y13 lit: 2px padding under the y4..y11 text
    }
    uint16_t len = 0;
    for (const char *p = text; *p; ++p)
        len += 6;
    uint8_t x = (len < 128) ? static_cast<uint8_t>((128 - len) / 2) : 0;
    // Glyph at y=4: page0 bits 4..7 = glyph bits 0..3, page1 bits 0..3 = glyph bits 4..7.
    // Sits inside the lit band y2..y10 with 2px padding above and below the text.
    while (*text) {
        const uint8_t *glyph = oledGlyph(*text++);
        for (uint8_t i = 0; i < 5 && x < 128; ++i, ++x) {
            oledFrame[0][x] ^= static_cast<uint8_t>((glyph[i] & 0x0F) << 4);
            oledFrame[1][x] ^= static_cast<uint8_t>(glyph[i] >> 4);
        }
        x += 1; // letter spacing
    }
}

// Progress bar with end caps: rails for the empty span, solid for the filled
// span. fill = confirmed/total of the whole session.
void fbBar(uint8_t x, uint8_t page, uint8_t w, uint32_t confirmed, uint32_t total) {
    if (x + w > 128 || w < 4)
        return;
    const uint8_t last = x + w - 1;
    for (uint8_t i = 0; i < w; ++i) {
        oledFrame[page][x + i] = 0x81; // top/bottom rails
    }
    oledFrame[page][x] = 0xFF; // end caps
    oledFrame[page][last] = 0xFF;
    uint8_t fill = 0;
    if (total > 0) {
        fill = static_cast<uint8_t>((confirmed * (w - 2u)) / total);
        if (fill > w - 2u)
            fill = w - 2u;
    }
    for (uint8_t i = 0; i < fill; ++i) {
        oledFrame[page][x + 1 + i] = 0xFF;
    }
}

// Push the whole framebuffer in one pass from loop() context ONLY.
void pushOled() {
    for (uint8_t page = 0; page < 8; ++page) {
        oledCommand(0xB0 + page);
        oledCommand(0x00);
        oledCommand(0x10);
        oledData(oledFrame[page], 128);
    }
}

void updateOled(bool force = false) {
    if (rfOpKind != RF_OP_NONE)
        return;
    const bool streaming = proxy_state.phase == GDFU_PROXY_STREAMING;
    if (!oledShouldUpdate(oledReady, force, streaming, oledDirty,
                          millis() - oledLastUpdateMs))
        return;
    oledLastUpdateMs = millis();
    oledDirty = false;

    memset(oledFrame, 0, sizeof(oledFrame));

    fbTitleBar("GFSK DFU PROXY");

    char line[24];

    // State machine + target id.
    char state[12];
    const uint8_t oledError = proxy_state.last_error != 0 ? proxy_state.last_error : shownError;
    if (oledError != 0) {
        snprintf(state, sizeof(state), "ERR %u", oledError);
    } else if (proxy_state.phase == GDFU_PROXY_STREAMING) {
        snprintf(state, sizeof(state), "FLASHING");
    } else if (proxy_state.phase == GDFU_PROXY_SELECTED) {
        snprintf(state, sizeof(state), "READY");
    } else {
        snprintf(state, sizeof(state), "IDLE");
    }
    if (proxy_state.selected_target != 0) {
        snprintf(line, sizeof(line), "%s*%08lX", state,
                 static_cast<unsigned long>(proxy_state.selected_target));
    } else {
        snprintf(line, sizeof(line), "%s*NONE", state);
    }
    fbText(0, 2, line);

    // Radio profile (page 4 left blank as a spacer under the state row).
    const uint32_t frequency = active_profile == GDFU_PROFILE_ID_868 ? 864100000u : 433175000u;
    const uint32_t bandwidth = active_profile == GDFU_PROFILE_ID_868 ? GDFU_RX_BANDWIDTH_868_HZ : GDFU_RX_BANDWIDTH_433_HZ;
    snprintf(line, sizeof(line), "%u.%03uMHZ %uKHZ", frequency / 1000000u,
             (frequency / 1000u) % 1000u, bandwidth / 1000u);
    fbText(0, 4, line);

    // Queue depth (of the 8-chunk window) and error counter.
    snprintf(line, sizeof(line), "QUEUE:%u/8 ERR:%u", static_cast<unsigned>(queueCount),
             static_cast<unsigned>(errorCount));
    fbText(0, 5, line);

    fbBar(0, 7, 128, confirmed_bytes, session_total_bytes);

    pushOled();
}

void showBootSplash() {
    if (!oledReady)
        return;
    memset(oledFrame, 0, sizeof(oledFrame));
    const char *lines[] = {"GFSK DFU PROXY", PROXY_RELEASE_VERSION, "by kasvoton * meshcore.pt"};
    for (uint8_t row = 0; row < 3; ++row) {
        const size_t length = strlen(lines[row]);
        const uint8_t spacing = length * 6 > 128 ? 5 : 6;
        const size_t width = length * spacing - 1;
        // ponytail: versions wider than the OLED are clipped; use scrolling if needed.
        uint16_t x = width < 128 ? (128 - width) / 2 : 0;
        const uint8_t y = 16 + row * 12;
        for (const char *p = lines[row]; *p && x < 128; ++p, x += spacing) {
            const uint8_t *glyph = oledGlyph(*p);
            for (uint8_t column = 0; column < 5 && x + column < 128; ++column) {
                for (uint8_t bit = 0; bit < 8; ++bit) {
                    if (glyph[column] & (1u << bit))
                        oledFrame[(y + bit) / 8][x + column] |= 1u << ((y + bit) % 8);
                }
            }
        }
    }
    pushOled();
    delay(5000);
}

void initOled() {
    DEBUG_PRINTLN("[OLED] reset pin");
#if PIN_OLED_RST >= 0
    pinMode(PIN_OLED_RST, OUTPUT);
    DEBUG_PRINTLN("[OLED] reset low");
    digitalWrite(PIN_OLED_RST, LOW);
    delay(10);
    DEBUG_PRINTLN("[OLED] reset high");
    digitalWrite(PIN_OLED_RST, HIGH);
#endif
    DEBUG_PRINTLN("[OLED] i2c begin");
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 400000);
    Wire.setTimeOut(20);
    Wire.beginTransmission(OLED_ADDR);
    [[maybe_unused]] const int oledAck = Wire.endTransmission();
    DEBUG_SERIAL("[OLED] ack=%d\n", oledAck); // 0 = panel answered.
    DEBUG_PRINTLN("[OLED] init commands");
    oledCommand(0xAE);
    oledCommand(0xD5);
    oledCommand(0x80);
    oledCommand(0xA8);
    oledCommand(0x3F);
    oledCommand(0xD3);
    oledCommand(0x00);
    oledCommand(0x40);
    oledCommand(0x8D);
    oledCommand(0x14);
    oledCommand(0x20);
    oledCommand(0x00);
    oledCommand(0xA1);
    oledCommand(0xC8);
    oledCommand(0xDA);
    oledCommand(0x12);
    oledCommand(0x81);
    oledCommand(0x7F);
    oledCommand(0xA4);
    oledCommand(0xA6);
    // Clear the whole GDDR once (fixes garbled leftovers on the bottom rows).
    oledCommand(0x22);
    oledCommand(0x00);
    oledCommand(0x07); // rows 0-7
    oledCommand(0x21);
    oledCommand(0x00);
    oledCommand(0x7F); // cols 0-127
    static const uint8_t zeros[32] = {};
    for (uint16_t i = 0; i < 1024; i += 32)
        oledData(zeros, 32);
    oledCommand(0x20);
    oledCommand(0x02); // back to page addressing
    oledCommand(0xAF);
    oledReady = true;
    oledDirty = true;
    showBootSplash();
}

void writeU32(uint8_t *dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value >> 24);
    dst[1] = static_cast<uint8_t>(value >> 16);
    dst[2] = static_cast<uint8_t>(value >> 8);
    dst[3] = static_cast<uint8_t>(value);
}

void writeU16(uint8_t *dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value >> 8);
    dst[1] = static_cast<uint8_t>(value);
}

uint32_t readU32(const uint8_t *src) {
    return (static_cast<uint32_t>(src[0]) << 24) | (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) | static_cast<uint32_t>(src[3]);
}

void saveRadioSettings() {
    if (radioPreferences.putUChar("profile", active_profile) != 1 ||
        radioPreferences.putUChar("txPower", txPowerDbm) != 1) {
        DEBUG_PRINTLN("[NVS] radio settings save failed");
    }
}

void loadRadioSettings() {
    if (!radioPreferences.begin("radio", false)) {
        DEBUG_PRINTLN("[NVS] radio settings unavailable; using defaults");
        return;
    }
    const uint8_t storedProfile = radioPreferences.getUChar("profile", GDFU_DEFAULT_PROFILE);
    const uint8_t storedPower = radioPreferences.getUChar("txPower", GDFU_DEFAULT_TX_POWER_DBM);
    const gdfu_radio_settings_t settings = gdfu_radio_settings_normalize(storedProfile, storedPower);
    active_profile = settings.profile;
    txPowerDbm = settings.tx_power_dbm;
    if (!radioPreferences.isKey("profile") || !radioPreferences.isKey("txPower") ||
        storedProfile != active_profile || storedPower != txPowerDbm) {
        saveRadioSettings();
    }
}

uint64_t generate_session_id() {
    uint64_t high = static_cast<uint64_t>(esp_random());
    uint64_t low = static_cast<uint64_t>(esp_random());
    uint64_t sid = (high << 32) | low;
    return (sid == 0) ? 1u : sid;
}

void publishStatus(uint8_t state) {
    if (pCharStatus == nullptr)
        return;
    // [0] state [1..4] accepted BE [5..8] confirmed BE [9] last_error [10] ACK RSSI (int8, 0 = none).
    // Byte 10 is a trailing extension; gdfu-application parseStatus() requires >= 10 bytes.
    uint8_t status[11] = {
        state, 0, 0, 0, 0, 0, 0, 0, 0, proxy_state.last_error, static_cast<uint8_t>(lastAckRssi),
    };
    writeU32(&status[1], accepted_bytes);
    writeU32(&status[5], confirmed_bytes);
    pCharStatus->setValue(status, sizeof(status));
    if (deviceConnected) {
        pCharStatus->notify();
    }
    if (state == STATUS_ERROR)
        shownError = proxy_state.last_error;
    oledDirty = true;
}

void publishDiscovery() {
    if (pCharDiscovery == nullptr)
        return;
    uint32_t now = millis();
    uint8_t payload[1 + GDFU_PROXY_DISCOVERY_CAPACITY * 8] = {};
    uint8_t live = 0;

    for (size_t i = 0; i < proxy_state.beacon_count; ++i) {
        uint32_t age = now - proxy_state.beacons[i].last_seen_ms;
        if (age <= GDFU_PROXY_DISCOVERY_TTL_MS) {
            size_t offset = 1 + live * 8;
            writeU32(&payload[offset], proxy_state.beacons[i].target_id);
            payload[offset + 4] = proxy_state.beacons[i].profile;
            payload[offset + 5] = static_cast<uint8_t>(proxy_state.beacons[i].rssi);
            uint16_t ttl_s = static_cast<uint16_t>((GDFU_PROXY_DISCOVERY_TTL_MS - age) / 1000u);
            writeU16(&payload[offset + 6], ttl_s);
            ++live;
        }
    }

    payload[0] = live;
    pCharDiscovery->setValue(payload, 1 + live * 8);
    if (deviceConnected) {
        pCharDiscovery->notify();
    }
    oledDirty = true;
}

void publishConfig() {
    if (pCharConfig == nullptr)
        return;
    uint8_t cfg[18] = {};
    cfg[0] = active_profile;
    writeU32(&cfg[1], (active_profile == GDFU_PROFILE_ID_868) ? 864100000u : 433175000u);
    cfg[5] = txPowerDbm;
    writeU32(&cfg[6], GDFU_BITRATE_BPS);
    writeU32(&cfg[10], GDFU_DEVIATION_HZ);
    writeU32(&cfg[14], active_profile == GDFU_PROFILE_ID_868 ? GDFU_RX_BANDWIDTH_868_HZ : GDFU_RX_BANDWIDTH_433_HZ);
    pCharConfig->setValue(cfg, sizeof(cfg));
}

void publishTxPower() {
    if (pCharTxPower != nullptr)
        pCharTxPower->setValue(&txPowerDbm, 1);
}

void cancelRfOperation() {
    // A replacement send starts by putting SX1262 in standby, stopping an
    // in-flight TX/RX without discarding proxy_state's target/session IDs.
    rfOpKind = RF_OP_NONE;
    rfOpRetryDelayUntil = 0;
    rfDataDonePending = false;
    cmdState = CMD_NONE;
    abortKeepError = ERROR_NONE;
    rfDoneCmd = 0;
    rxActive = false;
}

void clearTransfer() {
    portENTER_CRITICAL(&queueMux);
    queueCount = 0;
    portEXIT_CRITICAL(&queueMux);
    accepted_bytes = 0;
    confirmed_bytes = 0;
    session_total_bytes = 0;
    powerSynced = false;
    cancelRfOperation();
    gdfu_proxy_abort(&proxy_state);
}

bool sendGdfuFrame(uint8_t type, const uint8_t *payload, uint16_t length, uint32_t timeout_ms,
                   uint8_t max_retries);

/** Best-effort RF ABORT, then clear the session. START may already be accepted and
 *  SET_TX_POWER applied while only its ACK was lost; without ABORT the target stays in its
 *  receiving session (NACKing a fresh START, at the raised power) until the idle timeout.
 *  `error` (ERROR_NONE for a plain abort) is published once the ABORT op completes. */
static void abortSessionOverRf(uint8_t error) {
    cancelRfOperation(); // keeps proxy_state's target/session IDs for the ABORT frame
    if (sendGdfuFrame(GDFU_PKT_ABORT, nullptr, 0, GDFU_CONTROL_TIMEOUT_MS, 1)) {
        abortKeepError = error;
        cmdState = CMD_ABORT_WAIT;
        return;
    }
    clearTransfer();
    proxy_state.last_error = error;
    publishStatus(error == ERROR_NONE ? STATUS_IDLE : STATUS_ERROR);
}

// --- Target air-format handling -------------------------------------------------
// Plain SX1262 chip framing on both ends after the bootloader HAL fix: whitening
// OFF, chip CRC(2, CCITT 0x1D0F/0x1021, non-inverted), variable-length mode with a
// length-prefixed FIFO buffer on TX. The proxy therefore uses ordinary RadioLib
// calls; frames are additionally validated by magic + in-frame CRC32. decodeAirFrame
// keeps a small offset scan so short noise prefixes cannot desynchronize framing.

// gdfu_frame_decode demands the exact frame size; derive it from the header.
static bool tryDecode(const uint8_t *buf, size_t avail, gdfu_frame_t *out) {
    if (avail < GDFU_HEADER_SIZE + 4)
        return false;
    if (buf[0] != 'G' || buf[1] != 'D' || buf[2] != 'F' || buf[3] != 'U')
        return false;
    uint16_t plen = static_cast<uint16_t>((buf[28] << 8) | buf[29]);
    if (plen > GDFU_MAX_PAYLOAD)
        return false;
    size_t need = GDFU_HEADER_SIZE + plen + 4;
    if (avail < need)
        return false;
    return gdfu_frame_decode(buf, need, out);
}

// Extract a GDFU frame from the FIFO. Variable-length RX already strips the length
// byte; scan the first offsets for resilience against stale FIFO prefixes.
bool decodeAirFrame(const uint8_t *fifo, size_t len, gdfu_frame_t *out) {
    if (len > GDFU_MAX_FRAME_SIZE)
        len = GDFU_MAX_FRAME_SIZE;
    for (size_t off = 0; off + GDFU_HEADER_SIZE + 4 <= len && off <= 2; ++off) {
        if (tryDecode(fifo + off, len - off, out))
            return true;
    }
    return false;
}

int reinitRadio(uint8_t profile) {
    const uint32_t frequency = (profile == GDFU_PROFILE_ID_868) ? 864100000u : 433175000u;
    const RADIO_CONFIG_TYPE config = {
        {PIN_NSS, PIN_NRST, PIN_BUSY, PIN_DIO1, PIN_SCK, PIN_MOSI, PIN_MISO},
        frequency,
        profile == GDFU_PROFILE_ID_868 ? GDFU_RX_BANDWIDTH_868_HZ : GDFU_RX_BANDWIDTH_433_HZ,
        static_cast<int8_t>(txPowerDbm)};
    RADIO_SET_CONFIG(config);
    const int state = RADIO_INIT();
    // Radio init leaves the chip in standby: force serviceRadioQueue to
    // re-arm receive, otherwise beacons stop after a config write.
    rxActive = false;
    radioReady = (state == RADIO_STATUS_OK);
    if (radioReady) {
        active_profile = profile;
        oledDirty = true;
        publishConfig();
    }
    return state;
}

// Non-blocking RF operation state. sendGdfuFrame arms a transmit and returns
// immediately; serviceRfOps() — called every loop() from serviceRadioQueue —
// advances TX_PENDING -> AWAIT_ACK, applies the retry budget, and records the
// outcome in rfDoneCmd/rfDoneOk (control) or rfDataDoneOk (DATA). loop()
// therefore never sits inside a blocking RF wait. State variables live at the
// top of the file: clearTransfer() and serviceRadioQueue() reference them
// before their definitions here.

/** Arm a transmit for an already-registered op (rfOp* fields set). */
static bool rfOpArm() {
    rxActive = false;
    gdfu_frame_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.type = static_cast<gdfu_packet_type_t>(rfOpType);
    tx.profile = static_cast<gdfu_profile_id_t>(active_profile);
    tx.target_id = proxy_state.selected_target;
    tx.session_id = proxy_state.session_id;
    tx.sequence = proxy_state.next_sequence;
    tx.offset = proxy_state.confirmed_offset;
    tx.length = rfOpPayloadLen;
    if (rfOpPayloadLen > 0) {
        memcpy(tx.payload, rfOpPayload, rfOpPayloadLen);
    }

    // Variable-length TX: the chip reads FIFO[0] as the length byte. The T114's
    // bootloader writes the raw frame starting at FIFO[0], so FIFO[0] = 'G' (0x47 = 71).
    // The chip sends 71 as length, then 71 bytes of payload. Match this exactly.
    if (!gdfu_frame_encode(&tx, rfOpFrame, sizeof(rfOpFrame), &rfOpFrameLen))
        return false;
    if (RADIO_SEND_START(rfOpFrame, rfOpFrameLen) != RADIO_STATUS_OK)
        return false;
    rfOpStage = RF_STAGE_TX;
    rfOpStageDeadline = millis() + rfOpTimeoutMs;
    oledDirty = true;
    DEBUG_SERIAL("[TX] len=%u attempt=%u\n", (unsigned)rfOpFrameLen, rfOpAttempt);
    return true;
}

bool sendGdfuFrame(uint8_t type, const uint8_t *payload, uint16_t length, uint32_t timeout_ms,
                   uint8_t max_retries) {
    if (rfOpKind != RF_OP_NONE || rfOpRetryDelayUntil > millis())
        return false;
    rfOpKind = (type == GDFU_PKT_DATA) ? RF_OP_DATA : RF_OP_CONTROL;
    rfOpType = type;
    if (length > 0 && payload != nullptr) {
        memcpy(rfOpPayload, payload, length);
        rfOpPayloadLen = length;
    } else {
        rfOpPayloadLen = 0;
    }
    rfOpTimeoutMs = timeout_ms;
    rfOpMaxRetries = max_retries;
    rfOpAttempt = 1;
    if (!rfOpArm()) {
        rfOpKind = RF_OP_NONE;
        return false;
    }
    return true;
}

void rfOpFinish(bool ok) {
    const RfOpKind kind = rfOpKind;
    const uint8_t type = rfOpType;
    rfOpKind = RF_OP_NONE;
    if (kind == RF_OP_CONTROL) {
        rfDoneCmd = type;
        rfDoneOk = ok;
    } else if (kind == RF_OP_DATA) {
        rfDataDoneOk = ok;
        rfDataDonePending = true;
    }
}

bool rfOpStartAckStage() {
    if (RADIO_START_RECEIVE() != RADIO_STATUS_OK)
        return false;
    rfOpStage = RF_STAGE_ACK;
    rfOpStageDeadline = millis() + rfOpTimeoutMs;
    return true;
}

/** Apply the common retry/exhaustion policy from either TX or ACK stage. */
void rfOpFailAttempt() {
    // Missed ACK / failed TX: each attempt counts, retried or not.
    errorCount++;
    oledDirty = true;
    if (rfOpAttempt < rfOpMaxRetries) {
        rfOpAttempt++;
        if (rfOpType == GDFU_PKT_START) {
            // Session re-arm mirrors the old blocking loop. Keep the op alive
            // through the 200 ms backoff; re-arm it from RF_STAGE_RETRY.
            gdfu_proxy_start_session(&proxy_state, proxy_state.session_id, millis());
            rfOpRetryDelayUntil = millis() + 200;
            rfOpStage = RF_STAGE_RETRY;
            return;
        }
        // DATA retries rebuild from queue[0]: ACK progress may have shifted
        // next_sequence/confirmed_offset. Same op continues.
        if (!rfOpArm()) {
            rfOpFinish(false);
        }
        return;
    }
    gdfu_proxy_note_retx(&proxy_state);
    rfOpFinish(false);
}

void serviceRfOps() {
    if (rfOpKind == RF_OP_NONE)
        return;

    if (rfOpStage == RF_STAGE_RETRY) {
        if (rfOpRetryDelayUntil > millis())
            return;
        if (!rfOpArm()) {
            rfOpFinish(false);
            return;
        }
    }

    if (rfOpStage == RF_STAGE_TX) {
        const int poll = RADIO_SEND_POLL();
        if (poll == RADIO_STATUS_BUSY) {
            if (static_cast<int32_t>(millis() - rfOpStageDeadline) < 0)
                return; // frame still on air (~20 ms at 100 kbit/s)
            rfOpFailAttempt();
            return;
        }
        if (poll != RADIO_STATUS_OK || !rfOpStartAckStage()) {
            rfOpFailAttempt();
        }
        return;
    } else if (rfOpStage == RF_STAGE_ACK) {
        uint16_t irq = 0;
        if (RADIO_POLL_IRQ(&irq) != RADIO_STATUS_OK) {
            rfOpStageDeadline = 0;
        } else if (irq != 0) {
            uint8_t rx_buf[MAX_RF_FRAME];
            size_t rx_length = 0;
            int8_t rssi = 0;
            const int state = RADIO_READ_RECEIVED(rx_buf, sizeof(rx_buf), &rx_length, &rssi);
            if (state == RADIO_STATUS_CRC) {
                RADIO_START_RECEIVE();
                // CRC frames never consume the reply deadline (as before).
            } else if (state != RADIO_STATUS_OK) {
                rfOpStageDeadline = 0;
            } else {
                gdfu_frame_t rx_frame;
                if (decodeAirFrame(rx_buf, rx_length, &rx_frame)) {
                    oledDirty = true;
                    DEBUG_SERIAL("[RX] type=%u tid=%08X seq=%u\n", rx_frame.type,
                                   rx_frame.target_id, rx_frame.sequence);
                    if (rx_frame.target_id == proxy_state.selected_target &&
                        rx_frame.session_id == proxy_state.session_id &&
                        rx_frame.sequence == proxy_state.next_sequence) {
                        lastAckRssi = rssi < 0 ? rssi : -1;
                        if (rx_frame.type == GDFU_PKT_ACK) {
                            gdfu_proxy_on_ack(&proxy_state, rx_frame.session_id,
                                              rx_frame.sequence, rx_frame.offset, millis());
                            rfOpFinish(true);
                            return;
                        }
                        if (rx_frame.type == GDFU_PKT_NACK) {
                            errorCount++;
                            DEBUG_SERIAL("[RX] NACK code=%u\n", rx_frame.offset);
                            gdfu_proxy_on_nack(&proxy_state, static_cast<uint8_t>(rx_frame.offset));
                            rfOpFinish(false);
                            return;
                        }
                    }
                    // Stray frames: keep waiting, deadline preserved (as before).
                } else {
                    static uint32_t lastUndecoded = 0;
                    if (millis() - lastUndecoded > 10000) {
                        lastUndecoded = millis();
                        DEBUG_SERIAL("[RF] undecoded len=%u\n", (unsigned)rx_length);
                    }
                }
            }
        }

        if (rfOpKind != RF_OP_NONE &&
            static_cast<int32_t>(millis() - rfOpStageDeadline) >= 0) {
            rfOpFailAttempt();
            if (rfOpKind != RF_OP_NONE && rfOpStage == RF_STAGE_TX)
                serviceRfOps();
        }
    }
}

void serviceRadioQueue() {
    if (!radioReady)
        return;

    if (proxy_state.phase == GDFU_PROXY_STREAMING &&
        gdfu_proxy_session_expired(&proxy_state, millis())) {
        proxy_state.last_error = 4; // session expired
        publishStatus(STATUS_ERROR);
        clearTransfer();
        return;
    }

    if (rfOpKind != RF_OP_NONE || rfDataDonePending) {
        serviceRfOps();
        if (rfDataDonePending) {
            rfDataDonePending = false;
            // DATA completion: success pops the head; failure leaves it queued
            // (the pre-refactor code also retried the head in 5-attempt rounds).
            if (rfDataDoneOk) {
                portENTER_CRITICAL(&queueMux);
                PendingFrame &frame = queue[0];
                confirmed_bytes += frame.length;
                for (size_t i = 1; i < queueCount; ++i)
                    queue[i - 1] = queue[i];
                --queueCount;
                portEXIT_CRITICAL(&queueMux);
                publishStatus(queueCount == MAX_QUEUE_CHUNKS ? STATUS_BUSY : STATUS_STREAMING);
            } else {
                if (proxy_state.last_error == ERROR_NONE) {
                    proxy_state.last_error = ERROR_RADIO;
                }
                publishStatus(STATUS_ERROR);
            }
        }
        return;
    }

    if (cmdState != CMD_NONE)
        return; // command chain owns the radio until its op completes

    if (queueCount > 0 && cmdState == CMD_NONE) {
        PendingFrame &frame = queue[0];
        if (sendGdfuFrame(GDFU_PKT_DATA, frame.data.data(), static_cast<uint16_t>(frame.length),
                          GDFU_DATA_ACK_TIMEOUT_MS, GDFU_MAX_RETRIES)) {
            serviceRfOps();
            return; // completion handled above once rfDoneCmd fires
        }
    }

    // Keep receiver active between transmissions so short preambles are not missed.
    if (!rxActive) {
        if (RADIO_START_RECEIVE() == RADIO_STATUS_OK) {
            rxFailStreak = 0;
            rxActive = true;
        } else {
            // A cancelled op can leave the chip wedged (e.g. mid-SetTx with a
            // hung BUSY line state); a full re-init restores RX.
            if (++rxFailStreak >= 50) {
                rxFailStreak = 0;
                DEBUG_PRINTLN("[RF] RX restart failed; reinitializing radio");
                reinitRadio(active_profile);
            }
        }
        return;
    }

    // Poll the IRQ flags over SPI: DIO1 has been unreliable on this board.
    uint16_t irq = 0;
    if (RADIO_POLL_IRQ(&irq) != RADIO_STATUS_OK) {
        rxActive = false;
        return;
    }
    if (irq == 0)
        return;

    uint8_t rx_buf[MAX_RF_FRAME] = {};
    size_t len = 0;
    int8_t rssi = 0;
    const int state = RADIO_READ_RECEIVED(rx_buf, sizeof(rx_buf), &len, &rssi);
    rxActive = false;
    if (state == RADIO_STATUS_CRC)
        return;
    if (state != RADIO_STATUS_OK) {
        DEBUG_SERIAL("[RF] read=%d\n", state);
        return;
    }
    gdfu_frame_t bcn;
    if (decodeAirFrame(rx_buf, len, &bcn) && bcn.type == GDFU_PKT_BEACON) {
        oledDirty = true;
        DEBUG_SERIAL("[RX] beacon target=%08lX profile=%u rssi=%d\n", (unsigned long)bcn.target_id,
                     (unsigned)bcn.profile, (int)rssi);
        gdfu_proxy_on_beacon(&proxy_state, bcn.target_id, bcn.profile, rssi, millis());
        publishDiscovery();
    } else {
        static uint32_t lastDump = 0;
        if (millis() - lastDump > 10000) {
            lastDump = millis();
            DEBUG_SERIAL("[RF] miss len=%u:", (unsigned)len);
            for (size_t i = 0; i < len && i < 36; ++i)
                DEBUG_SERIAL(" %02X", rx_buf[i]);
            DEBUG_PRINTLN();
        }
    }
}

// ---- FILE MANAGEMENT (docs/PROTOCOL_BLE.md "File management") -------------
// Every operation runs synchronously inside the GATT write callback, so the
// app gets its result by reading FILE_STATUS / FILE_DATA after the write
// completes. Rules (names, capacity, ordering, CRC) live in gdfu_file.c.
constexpr const char *FILE_TMP_PATH = "/.upload.tmp";
constexpr const char *FILE_BACKUP_PATH = "/.upload.backup";
constexpr const char *FILE_NAME_PATH = "/.upload.name"; // final name of an in-flight replacement
constexpr uint8_t FILE_CMD_LIST_ENTRY = 1;
constexpr uint8_t FILE_CMD_UPLOAD_BEGIN = 2;
constexpr uint8_t FILE_CMD_UPLOAD_FINISH = 3;
constexpr uint8_t FILE_CMD_UPLOAD_ABORT = 4;
constexpr uint8_t FILE_CMD_DELETE = 5;
constexpr uint8_t FILE_CMD_DOWNLOAD_BEGIN = 6;
constexpr uint8_t FILE_CMD_DOWNLOAD_READ = 7;
constexpr uint8_t FILE_CMD_DOWNLOAD_END = 8;

BLECharacteristic *pCharFileData = nullptr;
BLECharacteristic *pCharFileStatus = nullptr;
BLECharacteristic *pCharFileStorage = nullptr;
bool fileReady = false;
gdfu_upload_t upload;
File uploadFile;
File downloadFile;
char uploadName[GDFU_FILE_MAX_NAME + 2]; // "/" + name + NUL
uint32_t downloadSize = 0;
uint32_t downloadPos = 0;
uint8_t fileLastErr = GDFU_FILE_OK;
// Defined before fileReset(), which drains a stale completion on disconnect.
SemaphoreHandle_t fileDoneSem = nullptr;

void fileSetErr(uint8_t err) {
    if (fileLastErr == GDFU_FILE_OK)
        fileLastErr = err; // sticky: the first failure is the one reported
}

uint16_t fileCount() {
    uint16_t n = 0;
    File root = LittleFS.open("/");
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (!f.isDirectory() && f.name()[0] != '.')
            ++n;
    }
    return n;
}

void publishFileStatus() {
    if (pCharFileStatus == nullptr)
        return;
    uint8_t s[10] = {0};
    s[0] = upload.active ? 1 : (downloadFile ? 2 : 0);
    s[1] = fileLastErr;
    writeU32(&s[2], upload.active ? upload.received : downloadPos);
    writeU32(&s[6], upload.active ? upload.size : downloadSize);
    pCharFileStatus->setValue(s, sizeof(s));
}

void publishFileStorage() {
    if (pCharFileStorage == nullptr)
        return;
    uint8_t s[10] = {0};
    if (fileReady) {
        writeU32(&s[0], static_cast<uint32_t>(LittleFS.totalBytes()));
        writeU32(&s[4], static_cast<uint32_t>(LittleFS.usedBytes()));
        uint16_t n = fileCount();
        s[8] = static_cast<uint8_t>(n >> 8);
        s[9] = static_cast<uint8_t>(n);
    }
    pCharFileStorage->setValue(s, sizeof(s));
}

void fileAbortUpload() {
    if (uploadFile)
        uploadFile.close();
    if (fileReady && LittleFS.exists(FILE_TMP_PATH))
        LittleFS.remove(FILE_TMP_PATH);
    gdfu_upload_init(&upload);
}

void fileCloseDownload() {
    if (downloadFile)
        downloadFile.close();
    downloadSize = 0;
    downloadPos = 0;
}

// Disconnect (or boot) leaves nothing half-open.
void fileReset() {
    fileAbortUpload();
    fileCloseDownload();
    fileLastErr = GDFU_FILE_OK;
    if (fileDoneSem != nullptr) {
        // A worker overrun from the dying connection must not hand its
        // completion credit to the next connection's first command.
        while (xSemaphoreTake(fileDoneSem, 0) == pdTRUE) {
        }
    }
    publishFileStatus();
    publishFileStorage();
}

// payload at `off` is [len:1][name]; it must end the payload.
uint8_t fileParseName(const uint8_t *p, size_t n, size_t off, char *path /*[GDFU_FILE_MAX_NAME+2]*/) {
    if (n < off + 1 || n != off + 1u + p[off])
        return GDFU_FILE_ERR_COMMAND;
    size_t len = p[off];
    if (!gdfu_file_name_valid(reinterpret_cast<const char *>(p + off + 1), len))
        return GDFU_FILE_ERR_NAME;
    path[0] = '/';
    memcpy(path + 1, p + off + 1, len);
    path[len + 1] = 0;
    return GDFU_FILE_OK;
}

uint8_t fileListEntry(uint16_t index) {
    uint8_t out[1 + 4 + GDFU_FILE_MAX_NAME];
    out[0] = 0; // present = 0 marks the end of the list
    size_t outLen = 1;
    uint16_t i = 0;
    File root = LittleFS.open("/");
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        if (f.isDirectory() || f.name()[0] == '.')
            continue;
        if (i++ != index)
            continue;
        size_t nameLen = strlen(f.name());
        if (nameLen > GDFU_FILE_MAX_NAME)
            return GDFU_FILE_ERR_IO;
        out[0] = 1;
        writeU32(&out[1], static_cast<uint32_t>(f.size()));
        memcpy(&out[5], f.name(), nameLen);
        outLen = 5 + nameLen;
        break;
    }
    pCharFileData->setValue(out, outLen);
    return GDFU_FILE_OK;
}

void fileRecoverReplacement();

uint8_t fileUploadBegin(const uint8_t *p, size_t n) {
    // [size:4][crc32:4][replace:1][name_len:1][name]
    if (n < 10)
        return GDFU_FILE_ERR_COMMAND;
    char path[GDFU_FILE_MAX_NAME + 2];
    uint8_t err = fileParseName(p, n, 9, path);
    if (err != GDFU_FILE_OK)
        return err;
    if (upload.active || downloadFile)
        return GDFU_FILE_ERR_BUSY;
    if (LittleFS.exists(FILE_NAME_PATH)) {
        // Unresolved replacement state from a stuck/failed promotion: the
        // hidden backup is some file's only copy. Recover first (next
        // disconnect/boot) or delete the file via the app; refuse until then.
        fileRecoverReplacement();
        if (LittleFS.exists(FILE_NAME_PATH)) {
            return GDFU_FILE_ERR_BUSY;
        }
    }
    uint32_t size = readU32(p);
    if (!gdfu_file_fits(static_cast<uint32_t>(LittleFS.totalBytes()),
                        static_cast<uint32_t>(LittleFS.usedBytes()), size))
        return GDFU_FILE_ERR_NO_SPACE;
    if (LittleFS.exists(path) && p[8] == 0)
        return GDFU_FILE_ERR_EXISTS;
    LittleFS.remove(FILE_TMP_PATH);
    uploadFile = LittleFS.open(FILE_TMP_PATH, "w");
    if (!uploadFile)
        return GDFU_FILE_ERR_IO;
    err = gdfu_upload_begin(&upload, size, readU32(p + 4));
    if (err != GDFU_FILE_OK) {
        uploadFile.close();
        return err;
    }
    strncpy(uploadName, path, sizeof(uploadName) - 1);
    uploadName[sizeof(uploadName) - 1] = 0;
    return GDFU_FILE_OK;
}

// Bounded reader the ZIP validator uses to scan the completed upload on
// LittleFS without loading it into RAM. All ranges are pre-checked by the
// validator against the file size we pass in.
bool uploadFileReader(void *context, uint32_t offset, uint8_t *buffer, size_t length) {
    File *file = static_cast<File *>(context);
    if (!*file || !file->seek(offset)) {
        return false;
    }
    return file->read(buffer, length) == static_cast<int>(length);
}

// LittleFS adapter for gdfu_file_promote/recover. FINAL is the destination of
// the replacement in progress (or the one named in the sidecar at boot).
struct PromoteCtx {
    char finalPath[GDFU_FILE_MAX_NAME + 2];
};

const char *promotePath(PromoteCtx *c, int id) {
    return id == GDFU_PATH_TEMP ? FILE_TMP_PATH : id == GDFU_PATH_BACKUP ? FILE_BACKUP_PATH : c->finalPath;
}

bool promoteRename(void *ctx, int from, int to) {
    PromoteCtx *c = static_cast<PromoteCtx *>(ctx);
    return LittleFS.rename(promotePath(c, from), promotePath(c, to));
}

bool promoteRemove(void *ctx, int path) {
    return LittleFS.remove(promotePath(static_cast<PromoteCtx *>(ctx), path));
}

bool promoteExists(void *ctx, int path) {
    return LittleFS.exists(promotePath(static_cast<PromoteCtx *>(ctx), path));
}

const gdfu_fs_ops_t promoteOps = {promoteRename, promoteRemove, promoteExists};

// Finish a replacement that power loss or a reset interrupted. Runs on the
// file worker path; the sidecar says which name the backup belongs to.
void fileRecoverReplacement() {
    if (!fileReady || !LittleFS.exists(FILE_NAME_PATH)) {
        return;
    }
    PromoteCtx ctx = {};
    File f = LittleFS.open(FILE_NAME_PATH, "r");
    const size_t n = f ? f.read(reinterpret_cast<uint8_t *>(ctx.finalPath), sizeof(ctx.finalPath) - 1) : 0;
    if (f) {
        f.close();
    }
    ctx.finalPath[n] = 0;
    if (n == 0 || ctx.finalPath[0] != '/' ||
        !gdfu_file_name_valid(ctx.finalPath + 1, n - 1)) {
        // A torn or corrupt sidecar would otherwise promote the backup to an
        // undeletable name. Drop the record; the backup, if any, stays for a
        // manual recovery instead of becoming a phantom file.
        LittleFS.remove(FILE_NAME_PATH);
        return;
    }
    const bool ok = gdfu_file_recover(&promoteOps, &ctx, LittleFS.exists(FILE_BACKUP_PATH),
                                      LittleFS.exists(ctx.finalPath));
    if (ok) {
        LittleFS.remove(FILE_NAME_PATH);
    }
    DEBUG_SERIAL("[FILE] replacement recovery %s\n", ok ? "ok" : "FAILED");
}

uint8_t fileUploadFinish() {
    uint8_t err = gdfu_upload_finish(&upload);
    if (err != GDFU_FILE_OK) {
        fileAbortUpload();
        return err;
    }
    uploadFile.close();
    // Only a complete GFSK package may become a stored file. Anything else is
    // deleted here, before the rename, so it never appears in the listing.
    File tmp = LittleFS.open(FILE_TMP_PATH, "r");
    if (!tmp) {
        fileAbortUpload();
        return GDFU_FILE_ERR_IO;
    }
    const uint32_t tmpSize = static_cast<uint32_t>(tmp.size());
    err = gdfu_zip_validate_reader(uploadFileReader, &tmp, tmpSize);
    tmp.close();
    if (err != GDFU_FILE_OK) {
        fileAbortUpload();
        return GDFU_FILE_ERR_PACKAGE;
    }
    char path[GDFU_FILE_MAX_NAME + 2];
    strncpy(path, uploadName, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    PromoteCtx ctx = {};
    strncpy(ctx.finalPath, path, sizeof(ctx.finalPath) - 1);
    const bool replacing = LittleFS.exists(path);
    if (replacing) {
        // Record the destination first so boot recovery can restore the
        // backup to the right name if power is lost mid-replacement.
        File sidecar = LittleFS.open(FILE_NAME_PATH, "w");
        if (!sidecar || sidecar.write(reinterpret_cast<const uint8_t *>(path), strlen(path)) != strlen(path)) {
            if (sidecar) {
                sidecar.close();
            }
            LittleFS.remove(FILE_NAME_PATH);
            fileAbortUpload();
            return GDFU_FILE_ERR_IO;
        }
        sidecar.close();
    }
    const gdfu_promote_result_t result = gdfu_file_promote(&promoteOps, &ctx, replacing);
    gdfu_upload_init(&upload);
    // DONE is only final once the backup is really gone; otherwise the sidecar
    // drives recovery (backup cleanup or restore) at boot/disconnect.
    if (result != GDFU_PROMOTE_STUCK && replacing &&
        (!LittleFS.exists(FILE_BACKUP_PATH) || LittleFS.remove(FILE_BACKUP_PATH))) {
        LittleFS.remove(FILE_NAME_PATH);
    }
    if (result != GDFU_PROMOTE_DONE) {
        if (LittleFS.exists(FILE_TMP_PATH)) {
            LittleFS.remove(FILE_TMP_PATH);
        }
        return GDFU_FILE_ERR_IO;
    }
    return GDFU_FILE_OK;
}

uint8_t fileDownloadBegin(const uint8_t *p, size_t n) {
    char path[GDFU_FILE_MAX_NAME + 2];
    uint8_t err = fileParseName(p, n, 0, path);
    if (err != GDFU_FILE_OK)
        return err;
    if (upload.active || downloadFile)
        return GDFU_FILE_ERR_BUSY;
    downloadFile = LittleFS.open(path, "r");
    if (!downloadFile)
        return GDFU_FILE_ERR_NOT_FOUND;
    downloadSize = static_cast<uint32_t>(downloadFile.size());
    downloadPos = 0;
    return GDFU_FILE_OK;
}

uint8_t fileDownloadRead(const uint8_t *p, size_t n) {
    // [offset:4][len:1]
    if (n != 5 || p[4] == 0 || p[4] > GDFU_FILE_MAX_CHUNK)
        return GDFU_FILE_ERR_COMMAND;
    if (!downloadFile)
        return GDFU_FILE_ERR_NOT_FOUND;
    uint32_t offset = readU32(p);
    if (offset > downloadSize)
        return GDFU_FILE_ERR_OFFSET;
    uint8_t buf[GDFU_FILE_MAX_CHUNK];
    size_t want = p[4];
    if (want > downloadSize - offset)
        want = downloadSize - offset;
    size_t got = 0;
    if (want > 0) {
        if (!downloadFile.seek(offset))
            return GDFU_FILE_ERR_IO;
        got = downloadFile.read(buf, want);
        if (got != want)
            return GDFU_FILE_ERR_IO;
    }
    downloadPos = offset + static_cast<uint32_t>(got);
    pCharFileData->setValue(buf, got);
    return GDFU_FILE_OK;
}

// File I/O recurses deep into LittleFS and overflows the 3 KB BTC_TASK stack
// if run inside the GATT write callback (observed: stack canary panic). A
// worker task with its own stack runs the operation; the callback blocks on a
// semaphore so the documented "result ready when the write returns" holds.
struct FileRequest {
    uint8_t buf[256];
    size_t len;
    bool isCommand;
};

SemaphoreHandle_t fileReqSem = nullptr;
volatile bool fileResetPending = false; // set by onDisconnect, consumed by fileWorker
FileRequest fileRequest;
volatile uint32_t fileReqSeq = 0; // bumped by fileMarshal, latched by the worker
volatile uint32_t fileDoneSeq = 0; // set by the worker when that request finished

void fileMarshal(const std::string &v, bool isCommand) {
    FileRequest r;
    r.len = v.size() <= sizeof(r.buf) ? v.size() : sizeof(r.buf);
    memcpy(r.buf, v.data(), r.len);
    r.isCommand = isCommand;
    // A disconnect's cleanup runs before the next connection's first command:
    // the worker handles the reset flag ahead of this request.
    const uint32_t seq = ++fileReqSeq;
    fileRequest = r;
    xSemaphoreGive(fileReqSem);
    // Wait for THIS request's completion. A worker that overruns the window
    // gives a late credit labelled with the old sequence; the next command
    // cannot consume it as its own, so the protocol never runs one write
    // ahead of execution.
    for (;;) {
        if (fileDoneSeq == seq) {
            return;
        }
        if (xSemaphoreTake(fileDoneSem, pdMS_TO_TICKS(30000)) != pdTRUE) {
            break;
        }
        if (fileDoneSeq >= seq) {
            return;
        }
    }
    fileSetErr(GDFU_FILE_ERR_IO); // worker wedged or slow; surface it rather than hang BLE
    publishFileStatus();
}

void fileRunCommand(const uint8_t *p, size_t n) {
    uint8_t cmd = p[0];
        if (cmd != FILE_CMD_UPLOAD_FINISH)
            fileLastErr = GDFU_FILE_OK;
        uint8_t err = GDFU_FILE_OK;
        // File I/O stalls the flash cache; never overlap it with a DFU session.
        if (!fileReady) {
            err = GDFU_FILE_ERR_IO;
        } else if (cmdState != CMD_NONE || proxy_state.phase == GDFU_PROXY_STREAMING) {
            err = GDFU_FILE_ERR_BUSY;
        } else {
            switch (cmd) {
            case FILE_CMD_LIST_ENTRY:
                err = n == 3 ? fileListEntry(static_cast<uint16_t>((p[1] << 8) | p[2]))
                             : static_cast<uint8_t>(GDFU_FILE_ERR_COMMAND);
                break;
            case FILE_CMD_UPLOAD_BEGIN:
                err = fileUploadBegin(p + 1, n - 1);
                break;
            case FILE_CMD_UPLOAD_FINISH:
                err = fileUploadFinish();
                break;
            case FILE_CMD_UPLOAD_ABORT:
                fileAbortUpload();
                break;
            case FILE_CMD_DELETE: {
                char path[GDFU_FILE_MAX_NAME + 2];
                err = fileParseName(p + 1, n - 1, 0, path);
                if (err != GDFU_FILE_OK)
                    break;
                if (upload.active || downloadFile)
                    err = GDFU_FILE_ERR_BUSY;
                else if (!LittleFS.exists(path))
                    err = GDFU_FILE_ERR_NOT_FOUND;
                else if (!LittleFS.remove(path))
                    err = GDFU_FILE_ERR_IO;
                break;
            }
            case FILE_CMD_DOWNLOAD_BEGIN:
                err = fileDownloadBegin(p + 1, n - 1);
                break;
            case FILE_CMD_DOWNLOAD_READ:
                err = fileDownloadRead(p + 1, n - 1);
                break;
            case FILE_CMD_DOWNLOAD_END:
                fileCloseDownload();
                break;
            default:
                err = GDFU_FILE_ERR_COMMAND;
            }
        }
        if (err != GDFU_FILE_OK)
            fileSetErr(err);
        publishFileStatus();
        if (cmd != FILE_CMD_LIST_ENTRY && cmd != FILE_CMD_DOWNLOAD_READ)
            publishFileStorage();
}

void fileRunData(const uint8_t *p, size_t n) {
        // Upload chunk: [offset:4][bytes<=160]. Errors are sticky in FILE_STATUS.
        if (n <= 4) {
            fileSetErr(GDFU_FILE_ERR_COMMAND);
        } else {
            uint8_t err = gdfu_upload_chunk(&upload, readU32(p), p + 4, n - 4);
            if (err == GDFU_FILE_OK && uploadFile.write(p + 4, n - 4) != n - 4) {
                err = GDFU_FILE_ERR_IO;
                fileAbortUpload(); // a short write leaves the temp file unusable
            }
            if (err != GDFU_FILE_OK)
                fileSetErr(err);
        }
        publishFileStatus();
}

void fileWorker(void *) {
    for (;;) {
        xSemaphoreTake(fileReqSem, portMAX_DELAY);
        if (fileResetPending) {
            fileResetPending = false;
            fileRecoverReplacement();
            fileReset();
            if (fileReqSeq == fileDoneSeq) {
                continue; // wake-up was only the disconnect signal
            }
        }
        FileRequest r = fileRequest;
        const uint32_t seq = fileReqSeq;
        if (r.isCommand)
            fileRunCommand(r.buf, r.len);
        else
            fileRunData(r.buf, r.len);
        fileDoneSeq = seq;
        xSemaphoreGive(fileDoneSem);
    }
}

class FileCommandCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        std::string v = characteristic->getValue();
        if (v.empty())
            return;
        fileMarshal(v, true);
    }
};

class FileDataCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        std::string v = characteristic->getValue();
        if (v.empty())
            return;
        fileMarshal(v, false);
    }
};

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *server) override {
        (void)server;
        deviceConnected = true;
    }

    void onConnect(BLEServer *server, esp_ble_gatts_cb_param_t *param) override {
        deviceConnected = true;
        server->updateConnParams(param->connect.remote_bda, 6, 12, 0, 400);
    }

    void onDisconnect(BLEServer *server) override {
        (void)server;
        deviceConnected = false;
        lastAckRssi = 0;
        abortOnDisconnect = proxy_state.phase == GDFU_PROXY_STREAMING;
        if (!abortOnDisconnect)
            clearTransfer();
        // LittleFS recursion overflows this 3 KB BTC_TASK stack; the 8 KiB
        // file worker does the cleanup (see fileWorker).
        fileResetPending = true;
        if (fileReqSem != nullptr) {
            xSemaphoreGive(fileReqSem);
        }
        BLEDevice::startAdvertising();
    }
};

class DataCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        std::string value = characteristic->getValue();
        if (value.empty())
            return;
        // Capacity check, slot claim and copy are one critical section so
        // loop() never shifts or transmits a half-written slot.
        portENTER_CRITICAL(&queueMux);
        const bool rejected = value.length() > MAX_BLE_CHUNK || queueCount >= MAX_QUEUE_CHUNKS;
        // Hold INIT/DATA until the target ACKed SET_TX_POWER (powerSynced):
        // phase is already STREAMING while the START/SET_TX_POWER chain runs.
        const bool held = proxy_state.phase != GDFU_PROXY_STREAMING || !powerSynced;
        const bool accept = !rejected && !held;
        size_t count = queueCount;
        if (accept) {
            PendingFrame &frame = queue[queueCount];
            frame.length = value.length();
            memcpy(frame.data.data(), value.data(), frame.length);
            count = ++queueCount;
            accepted_bytes += frame.length;
        }
        portEXIT_CRITICAL(&queueMux);
        if (rejected) {
            proxy_state.last_error = ERROR_RADIO;
            publishStatus(STATUS_BUSY);
            return;
        }
        if (!accept)
            return;
        publishStatus(count == MAX_QUEUE_CHUNKS ? STATUS_BUSY : STATUS_STREAMING);
    }
};

// Commands are executed from loop(), never in the BLE callback. RF ops are
// nonblocking: pendingCmd is consumed only when no command chain is active
// (cmdState == CMD_NONE); completion flags rfDoneCmd/rfDoneOk are declared
// with the RF op state at the top of the file.
volatile uint8_t pendingCmd = 0;
uint8_t pendingPayload[GDFU_MAX_PAYLOAD];
volatile size_t pendingPayloadLen = 0;

class CommandCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        std::string val = characteristic->getValue();
        // The pendingCmd slot is a single-entry queue. While a command chain is
        // mid-op it must not be overwritten: a SELECT landing there during an
        // ABORT op was observed live as a lost SELECT. ABORT is the exception:
        // it is queued so loop() can cancel a stuck chain immediately.
        if (val.empty() || pendingCmd != 0 || (cmdState != CMD_NONE && val[0] != 3)) {
            if (!val.empty()) {
                publishStatus(STATUS_BUSY);
            }
            return;
        }
        uint8_t cmd = static_cast<uint8_t>(val[0]);
        if (cmd == 2 && val.length() - 1 > GDFU_MAX_PAYLOAD) {
            proxy_state.last_error = 5; // malformed BLE command
            publishStatus(STATUS_ERROR);
            return;
        }
        if (cmd == 1 && val.length() >= 5) { // SELECT_TARGET
            uint32_t tid = readU32(reinterpret_cast<const uint8_t *>(&val[1]));
            proxy_state.last_error = 0;
            [[maybe_unused]] uint32_t age = 0xFFFFFFFF;
            for (size_t i = 0; i < proxy_state.beacon_count; ++i) {
                if (proxy_state.beacons[i].target_id == tid)
                    age = millis() - proxy_state.beacons[i].last_seen_ms;
            }
            DEBUG_SERIAL("[CMD] select tid=%08X count=%u phase=%d age=%u\n", tid,
                           proxy_state.beacon_count, proxy_state.phase, age);
            if (gdfu_proxy_select_target(&proxy_state, tid, millis())) {
                lastAckRssi = 0; // don't report the previous target's link
                publishStatus(STATUS_IDLE);
            } else {
                proxy_state.last_error = 3; // wrong/expired target
                publishStatus(STATUS_ERROR);
            }
        } else if (cmd == 2) { // START_DFU
            pendingPayloadLen = val.length() > 1 ? val.length() - 1 : 0;
            for (size_t i = 0; i < pendingPayloadLen; ++i)
                pendingPayload[i] = static_cast<uint8_t>(val[i + 1]);
            pendingCmd = 2;
            publishStatus(STATUS_BUSY);
        } else if (cmd == 3) { // ABORT
            pendingCmd = 3;
            publishStatus(STATUS_BUSY);
        } else if (cmd == 4) { // RESET_RADIO
            pendingCmd = 4;
        } else if (cmd == 5) { // FINISH_DFU
            pendingCmd = 5;
        }
    }
};

// reinitRadio() resets the SX1262 from the BLE task, and the target's session
// TX power is synced only once per session: reject radio config writes while
// a command is queued or running, or a session is active (same gate as file commands).
static bool radioConfigBusy() {
    return pendingCmd != 0 || cmdState != CMD_NONE || proxy_state.phase == GDFU_PROXY_STREAMING;
}

class ConfigCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        std::string val = characteristic->getValue();
        if (val.empty())
            return;
        if (radioConfigBusy()) {
            publishConfig(); // restore the read value after a rejected write
            publishStatus(STATUS_BUSY);
            return;
        }
        uint8_t profile = static_cast<uint8_t>(val[0]);
        if (profile != GDFU_PROFILE_ID_433 && profile != GDFU_PROFILE_ID_868) {
            proxy_state.last_error = 2; // profile mismatch
            publishStatus(STATUS_ERROR);
            return;
        }
        if (reinitRadio(profile) == RADIO_STATUS_OK) {
            saveRadioSettings();
            return;
        }
        proxy_state.last_error = ERROR_RADIO;
        publishStatus(STATUS_ERROR);
    }
};

class TxPowerCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        std::string val = characteristic->getValue();
        if (radioConfigBusy()) {
            publishTxPower(); // restore the read value after a rejected write
            publishStatus(STATUS_BUSY);
            return;
        }
        if (val.length() != 1 || !gdfu_proxy_tx_power_valid(static_cast<uint8_t>(val[0]))) {
            publishTxPower();           // restore the read value after an invalid write
            proxy_state.last_error = 2; // invalid radio configuration
            publishStatus(STATUS_ERROR);
            return;
        }

        const uint8_t oldPowerDbm = txPowerDbm;
        txPowerDbm = static_cast<uint8_t>(val[0]);
        if (reinitRadio(active_profile) == RADIO_STATUS_OK) {
            saveRadioSettings();
            publishTxPower();
            return;
        }

        // Keep the advertised setting truthful if the new radio initialization fails.
        txPowerDbm = oldPowerDbm;
        reinitRadio(active_profile);
        proxy_state.last_error = ERROR_RADIO;
        publishStatus(STATUS_ERROR);
    }
};

[[noreturn]] void enterStandby() {
    // No wake source: only physical reset may leave standby.
    clearTransfer();
    pendingCmd = 0;
    pendingPayloadLen = 0;

    // Stop both RF paths before pin isolation or power removal.
    RADIO_SLEEP();
    radioReady = false;
    digitalWrite(PIN_NSS, HIGH);
    gpio_hold_en(static_cast<gpio_num_t>(PIN_NSS));
#if !defined(PROXY_RADIO_SX1276)
    pinMode(35, OUTPUT); // MeshCore Heltec V3 LoRa TX LED.
    digitalWrite(35, LOW);
    gpio_hold_en(GPIO_NUM_35);
#endif

    // SSD1306 command must be sent before its supply is cut.
    if (oledReady)
        oledCommand(0xAE);
    oledReady = false;
#if PIN_OLED_RST >= 0
    digitalWrite(PIN_OLED_RST, LOW);
    gpio_hold_en(static_cast<gpio_num_t>(PIN_OLED_RST));
#endif
    Wire.end();
#if !defined(PROXY_RADIO_SX1276)
    digitalWrite(PIN_VEXT, HIGH); // Heltec V3 VEXT is active low.
    gpio_hold_en(static_cast<gpio_num_t>(PIN_VEXT));
#endif
    gpio_deep_sleep_hold_en();

    // This firmware never starts Wi-Fi. BLEDevice::deinit stops BLE and releases its memory.
    BLEDevice::deinit(true);
    fileAbortUpload();
    fileCloseDownload();
    if (fileReady) {
        LittleFS.end();
        fileReady = false;
    }
    radioPreferences.end();

#if DEBUG
    Serial.flush();
#endif
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_deep_sleep_start();
    for (;;) {
    }
}

void serviceStandbyButton() {
    if (standbyButton.update(digitalRead(PIN_USER_BUTTON) == LOW, millis()))
        enterStandby();
}
} // namespace

void setup() {
#if DEBUG
    Serial.begin(115200); // Heltec V3 UART0 -> CP210x bridge (no USB CDC)
    DEBUG_PRINTLN("[BOOT] start");
#endif
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(static_cast<gpio_num_t>(PIN_NSS));
#if !defined(PROXY_RADIO_SX1276)
    gpio_hold_dis(GPIO_NUM_35);
#endif
#if PIN_OLED_RST >= 0
    gpio_hold_dis(static_cast<gpio_num_t>(PIN_OLED_RST));
#endif
#if !defined(PROXY_RADIO_SX1276)
    gpio_hold_dis(static_cast<gpio_num_t>(PIN_VEXT));
#endif
    DEBUG_PRINTLN("[BOOT] holds released");

    pinMode(PIN_USER_BUTTON, INPUT_PULLUP);
#if !defined(PROXY_RADIO_SX1276)
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);
    delay(10);
#endif
    initOled();
    DEBUG_PRINTLN("[BOOT] NVS");
    loadRadioSettings();
    gdfu_proxy_init(&proxy_state);
    DEBUG_PRINTLN("[BOOT] radio");
    [[maybe_unused]] int initState = reinitRadio(active_profile);
    DEBUG_SERIAL("[BOOT] radio init: %d\n", initState);
    updateOled(true);

    BLEDevice::init("GFSK DFU PROXY");
    BLEDevice::setMTU(247); // file blocks are up to 164 B; avoid relying on long reads/writes
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());
    // Handle budget: 1 service decl + 12 real characteristics * 2 + 2 CCCDs = 27; the library
    // default of 15 silently truncates the table after the STATUS CCCD (bluedroid rejects the
    // remaining attribute registrations). Reserve 30 so every characteristic reaches the air.
    BLEService *service = pServer->createService(BLEUUID(SERVICE_UUID), 30);

    BLECharacteristic *version =
        service->createCharacteristic(CHAR_VERSION_UUID, BLECharacteristic::PROPERTY_READ);
    uint8_t versionData[] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0xC0};
    version->setValue(versionData, sizeof(versionData));
    BLECharacteristic *release =
        service->createCharacteristic(CHAR_RELEASE_UUID, BLECharacteristic::PROPERTY_READ);
    release->setValue(PROXY_RELEASE_VERSION);

    pCharConfig = service->createCharacteristic(
        CHAR_CONFIG_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    pCharConfig->setCallbacks(new ConfigCallbacks());
    publishConfig();

    pCharTxPower = service->createCharacteristic(
        CHAR_TX_POWER_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    pCharTxPower->setCallbacks(new TxPowerCallbacks());
    publishTxPower();

    pCharDiscovery = service->createCharacteristic(
        CHAR_DISCOVERY_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    pCharDiscovery->addDescriptor(new BLE2902());
    publishDiscovery();

    pCharCommand =
        service->createCharacteristic(CHAR_COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE);
    pCharCommand->setCallbacks(new CommandCallbacks());

    pCharStatus = service->createCharacteristic(
        CHAR_STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    pCharStatus->addDescriptor(new BLE2902());
    publishStatus(STATUS_IDLE);

    BLECharacteristic *data = service->createCharacteristic(
        CHAR_DATA_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    data->setCallbacks(new DataCallbacks());

    BLECharacteristic *fileCommand = service->createCharacteristic(
        CHAR_FILE_COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE);
    fileCommand->setCallbacks(new FileCommandCallbacks());
    pCharFileData = service->createCharacteristic(
        CHAR_FILE_DATA_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    pCharFileData->setCallbacks(new FileDataCallbacks());
    pCharFileStatus =
        service->createCharacteristic(CHAR_FILE_STATUS_UUID, BLECharacteristic::PROPERTY_READ);
    pCharFileStorage =
        service->createCharacteristic(CHAR_FILE_STORAGE_UUID, BLECharacteristic::PROPERTY_READ);
    fileReady = LittleFS.begin(true, "/littlefs", 4, "spiffs");
    DEBUG_SERIAL("[BOOT] file storage: %s\n", fileReady ? "ok" : "mount failed");
    gdfu_upload_init(&upload);
    fileRecoverReplacement();
    fileReset();
    fileReqSem = xSemaphoreCreateBinary();
    fileDoneSem = xSemaphoreCreateBinary();
    // 8 KiB: LittleFS path lookups + VFS stat run several KB deep.
    xTaskCreatePinnedToCore(fileWorker, "gdfu_file", 8192, nullptr, 1, nullptr, tskNO_AFFINITY);

    service->start();
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setScanResponse(true);
    BLEDevice::startAdvertising();
    DEBUG_PRINTLN("BLE advertising GFSK DFU PROXY started");
}

void loop() {
    serviceStandbyButton();

    // Refresh the DISCOVERY characteristic every second: its payload bakes in
    // per-entry TTLs at publish time, so without a periodic republish a BLE
    // client reading it between beacons sees a stale snapshot whose TTLs no
    // longer match reality (entries expire, ids rotate after target resets).
    static uint32_t lastDiscoveryRefreshMs = 0;
    if (millis() - lastDiscoveryRefreshMs >= 1000u) {
        lastDiscoveryRefreshMs = millis();
        publishDiscovery();
    }

    if (abortOnDisconnect) {
        abortOnDisconnect = false;
        cancelRfOperation();
        if (sendGdfuFrame(GDFU_PKT_ABORT, nullptr, 0, GDFU_CONTROL_TIMEOUT_MS, GDFU_MAX_RETRIES)) {
            cmdState = CMD_ABORT_WAIT;
        } else {
            clearTransfer();
        }
    }

    // Nonblocking control-op completions land here, before new commands are
    // accepted, so chains advance one stage per loop pass.
    if (cmdState != CMD_NONE && rfDoneCmd != 0) {
        bool ok = rfDoneOk;
        rfDoneCmd = 0;
        switch (cmdState) {
        case CMD_START_WAIT:
            if (ok) {
                // START established the RF session. Synchronize the target's
                // volatile TX power before accepting init/image data.
                if (sendGdfuFrame(GDFU_PKT_SET_TX_POWER, &txPowerDbm, 1,
                                  GDFU_CONTROL_TIMEOUT_MS, GDFU_MAX_RETRIES)) {
                    cmdState = CMD_SETPOWER_WAIT;
                } else {
                    // Target holds a live session: release it over RF.
                    abortSessionOverRf(ERROR_RADIO);
                    break;
                }
            }
            if (!ok) {
                // Drop the half-open session: the phase gate must reject
                // INIT/DATA while target power is unsynchronized.
                const uint8_t error = proxy_state.last_error;
                clearTransfer();
                proxy_state.last_error = error == ERROR_NONE ? ERROR_RADIO : error;
                publishStatus(STATUS_ERROR);
                cmdState = CMD_NONE;
            }
            break;
        case CMD_SETPOWER_WAIT:
            DEBUG_SERIAL("[CMD] SET_TX_POWER rf result=%d power=%u\n", ok, txPowerDbm);
            cmdState = CMD_NONE;
            if (ok) {
                powerSynced = true; // INIT/DATA writes may now be queued
                publishStatus(STATUS_STREAMING);
            } else {
                // The target may have applied the power and lost only the ACK:
                // release its session over RF so it reverts to 10 dBm.
                const uint8_t error = proxy_state.last_error;
                abortSessionOverRf(error == ERROR_NONE ? ERROR_RADIO : error);
            }
            break;
        case CMD_ABORT_WAIT: {
            cmdState = CMD_NONE;
            // Result ignored: ABORT is best-effort and returns the proxy to IDLE.
            const uint8_t error = abortKeepError; // clearTransfer() resets it
            clearTransfer();
            if (error != ERROR_NONE) {
                proxy_state.last_error = error;
                publishStatus(STATUS_ERROR);
            } else {
                publishStatus(STATUS_IDLE);
            }
            break;
        }
        case CMD_FINISH_WAIT:
            cmdState = CMD_NONE;
            if (ok) {
                clearTransfer();
                publishStatus(STATUS_IDLE);
            } else {
                if (proxy_state.last_error == ERROR_NONE) {
                    proxy_state.last_error = ERROR_RADIO;
                }
                publishStatus(STATUS_ERROR);
            }
            break;
        case CMD_NONE:
            break;
        }
    }

    uint8_t cmd = pendingCmd;
    // ABORT is always accepted, even mid-chain: it cancels the active command
    // chain and any in-flight op so a stuck START/FINISH is recoverable over
    // BLE without a proxy reset.
    if (cmd == 3 && cmdState != CMD_NONE) {
        pendingCmd = 0;
        const bool targetMayHoldSession =
            cmdState == CMD_START_WAIT || cmdState == CMD_SETPOWER_WAIT;
        cmdState = CMD_NONE;
        rfDoneCmd = 0;
        if (targetMayHoldSession) {
            abortSessionOverRf(ERROR_NONE); // START/SET_TX_POWER may have reached the target
        } else {
            clearTransfer();
            publishStatus(STATUS_IDLE);
        }
        DEBUG_PRINTLN("[CMD] abort during active chain; chain cancelled");
        cmd = 0;
    }
    if (cmd != 0 && cmdState == CMD_NONE) {
        pendingCmd = 0;
        if (cmd == 2) { // START_DFU
            shownError = ERROR_NONE;
            errorCount = 0;
            lastAckRssi = 0;
            // dfu_start_packet_t from the flasher is packed little-endian
            // ("<B3xIII"): the application size sits at bytes 12..15 and drives
            // the progress bar. The proxy's own fallback payload (below) carries a
            // dummy size, so the bar only fills for real flasher sessions.
            if (pendingPayloadLen >= 16) {
                session_total_bytes = static_cast<uint32_t>(pendingPayload[12]) |
                                      (static_cast<uint32_t>(pendingPayload[13]) << 8) |
                                      (static_cast<uint32_t>(pendingPayload[14]) << 16) |
                                      (static_cast<uint32_t>(pendingPayload[15]) << 24);
            } else {
                session_total_bytes = 0;
            }
            uint64_t sid = generate_session_id();
            // dfu_start_packet_t (app-only, word-aligned nonzero size) so the target's
            // zero-size guard passes; real image size arrives with the image metadata.
            static const uint8_t startPayload[16] = {
                0x04, 0x00, 0x00, 0x00, // application-only mode
                0x00, 0x00, 0x00, 0x00, // SoftDevice size
                0x00, 0x00, 0x00, 0x00, // bootloader size
                0x04, 0x00, 0x00, 0x00, // nonzero, word-aligned application size
            };
            const uint8_t *start = pendingPayloadLen > 0 ? pendingPayload : startPayload;
            uint16_t startLen = pendingPayloadLen > 0 ? static_cast<uint16_t>(pendingPayloadLen)
                                                      : sizeof(startPayload);
            DEBUG_SERIAL("[CMD] start phase=%d\n", proxy_state.phase);
            if (gdfu_proxy_start_session(&proxy_state, sid, millis())) {
                // Nonblocking: START retries across the target's blind beacon
                // window (5 attempts, 200 ms apart) inside the RF op; the
                // SET_TX_POWER chain continues from the completion block above.
                // START ACK arrives only after the target erases the full
                // image region (~9 s for 109 pages on nRF52840), so the ACK
                // window is 15 s — beyond GDFU_CONTROL_TIMEOUT_MS.
                if (sendGdfuFrame(GDFU_PKT_START, start, startLen, 15000,
                                  GDFU_MAX_RETRIES)) {
                    cmdState = CMD_START_WAIT;
                } else {
                    proxy_state.last_error = ERROR_RADIO;
                    publishStatus(STATUS_ERROR);
                }
            } else {
                proxy_state.last_error = 3; // no target
                publishStatus(STATUS_ERROR);
            }
        } else if (cmd == 3) { // ABORT
            // The abort frame is best-effort: the session reset happens on
            // completion regardless of the RF result. With no active session
            // there is nothing to abort on-air; resetting immediately keeps
            // the 10 s ACK window from blocking follow-up commands.
            if (proxy_state.phase != GDFU_PROXY_IDLE &&
                sendGdfuFrame(GDFU_PKT_ABORT, nullptr, 0, GDFU_CONTROL_TIMEOUT_MS, 1)) {
                cmdState = CMD_ABORT_WAIT;
            } else {
                clearTransfer();
                publishStatus(STATUS_IDLE);
            }
        } else if (cmd == 4) { // RESET_RADIO
            reinitRadio(active_profile);
            clearTransfer();
            publishStatus(radioReady ? STATUS_IDLE : STATUS_ERROR);
        } else if (cmd == 5) { // FINISH_DFU
            if (sendGdfuFrame(GDFU_PKT_FINISH, nullptr, 0, GDFU_FINISH_ACK_TIMEOUT_MS,
                              GDFU_MAX_RETRIES)) {
                cmdState = CMD_FINISH_WAIT;
            } else {
                proxy_state.last_error = ERROR_RADIO;
                publishStatus(STATUS_ERROR);
            }
        }
    }
    serviceRadioQueue();
    updateOled();
    delay(1);
}
