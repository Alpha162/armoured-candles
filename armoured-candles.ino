/**
 * Multi-Exchange Candlestick Chart — E-Paper Display
 * XIAO ESP32-S3 + Waveshare 7.5" V2 (Driver HAT Rev 2.3)
 *
 * Supported exchanges: Hyperliquid, Binance, AsterDEX, Kraken, Poloniex
 *
 * Features:
 *   - Web config UI at http://epdchart.local (or device IP)
 *   - Searchable coin dropdown (pairs fetched client-side)
 *   - All settings persist across reboots via NVS
 *   - Auto candle count scaling per interval
 *   - EMA, RSI, Volume overlays
 *   - Partial/full refresh with ghosting management
 *
 * Wiring (ALL on 3.3V):
 *   PWR  -> 3V3    VCC  -> 3V3    GND  -> GND
 *   CLK  -> D8 (GPIO7/SCK)   DIN  -> D10 (GPIO9/MOSI)
 *   CS   -> D1 (GPIO2)       DC   -> D2  (GPIO3)
 *   RST  -> D3 (GPIO4)       BUSY -> D4  (GPIO5)
 */

#include <WiFi.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <time.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ctype.h>
#include "mbedtls/sha256.h"
#include "epd7in5_V2.h"
#include "armoured_bird_64x64_bitmap.h"

// ─── COMPILE-TIME CONSTANTS ────────────────────────────
#ifndef FW_VERSION
#define FW_VERSION "v1.0.7"
#endif

#ifndef FW_BOARD_ID
#define FW_BOARD_ID "xiao-esp32s3-epdchart"
#endif

#ifndef FW_GIT_SHA
#define FW_GIT_SHA "unknown"
#endif

#ifndef FW_BUILD_TIMESTAMP
#define FW_BUILD_TIMESTAMP "unknown"
#endif

// Optional root CA certificate (PEM) for remote OTA HTTPS verification.
// Leave empty to skip TLS verification (setInsecure).
static const char* REMOTE_OTA_TLS_CA_CERT = "";

#define MAX_CANDLES     200
#define MAX_SLOTS       4
#define SCR_W           800
#define SCR_H           480
#define BUF_SIZE        (SCR_W / 8 * SCR_H)

// Default layout values (used for full-screen single chart)
#define MARGIN_L    10
#define MARGIN_R    85
#define MARGIN_T    35
#define MARGIN_B    25
#define RSI_H       45
#define VOL_H       30
#define GAP         5
#define CHART_W     (SCR_W - MARGIN_L - MARGIN_R)
#define RSI_TOP     (SCR_H - MARGIN_B - VOL_H - GAP - RSI_H)
#define CHART_H     (RSI_TOP - GAP - MARGIN_T)
#define VOL_TOP     (SCR_H - MARGIN_B - VOL_H)

// ─── MULTI-GRAPH DATA STRUCTURES ───────────────────────
struct Candle { float o, h, l, c, v; uint64_t t; };

struct Viewport {
    int x, y, w, h;
};

struct SlotConfig {
    char exchange[16];
    char coin[16];
    char quote[8];
    char interval[8];
    int  numCandles;
    bool autoCandles;
    int  emaFast;
    int  emaSlow;
    int  rsiPeriod;
    bool heikinAshi;
    bool eventCallouts;
};

enum SlotEventType {
    SLOT_EVENT_NONE = 0,
    SLOT_EVENT_EMA_CROSS_UP,
    SLOT_EVENT_EMA_CROSS_DOWN,
    SLOT_EVENT_RSI_ENTER_OVERBOUGHT,
    SLOT_EVENT_RSI_EXIT_OVERBOUGHT,
    SLOT_EVENT_RSI_ENTER_OVERSOLD,
    SLOT_EVENT_RSI_EXIT_OVERSOLD,
    SLOT_EVENT_BREAKOUT_HIGH,
    SLOT_EVENT_BREAKOUT_LOW
};

struct SlotEvent {
    uint64_t ts;
    SlotEventType type;
    char message[64];
};

struct ChartSlot {
    SlotConfig cfg;
    Candle candles[MAX_CANDLES];
    float  emaFastArr[MAX_CANDLES];
    float  emaSlowArr[MAX_CANDLES];
    float  rsiVal[MAX_CANDLES];
    int    candleCount;
    float  lastPrice;
    float  lastPctChange;
    SlotEvent lastEvent;
    bool          lastFetchOk;   // true if last fetch succeeded
    unsigned long lastFetchMs;   // millis() of last fetch attempt (0 = never)
};

// ─── RUNTIME CONFIG (loaded from NVS) ──────────────────
Preferences prefs;

char     cfgSSID[64]      = "YOUR_SSID";
char     cfgPass[64]      = "YOUR_PASSWORD";
int      cfgLayout        = 1;        // 1, 2, 3, or 4
int      cfgRefreshMin    = 5;
bool     cfgAutoRefresh   = true;     // derive refresh interval from active slot timescales
int      cfgTzOffset      = 0;        // seconds from UTC
int      cfgFullRefEvery  = 10;
int      cfgPartialPct    = 40;       // 0-100
char     cfgUiUser[24]    = "";
char     cfgUiPass[32]    = "";
const char* NTP_SERVER    = "pool.ntp.org";
const char* MDNS_HOST     = "epdchart";

// Per-slot chart config globals (used as scratch by fetch functions)
char     cfgExchange[16]  = "hyperliquid";
char     cfgCoin[16]      = "ETH";
char     cfgQuote[8]      = "USDT";
char     cfgInterval[8]   = "5m";
int      cfgNumCandles    = 60;
bool     cfgAutoCandles   = true;
int      cfgEmaFast       = 9;
int      cfgEmaSlow       = 21;
int      cfgRsiPeriod     = 14;
bool     cfgHeikinAshi    = false;
bool     cfgPersonalityEnabled = true;
int      cfgCaptionVerbosity   = 1;  // 0=minimal, 1=short, 2=detailed

enum MoodId {
    MOOD_VERY_BEARISH = 0,
    MOOD_BEARISH      = 1,
    MOOD_NEUTRAL      = 2,
    MOOD_BULLISH      = 3,
    MOOD_VERY_BULLISH = 4
};

struct MoodInfo {
    MoodId id;
    const char* caption;
    const char* style;
    float aggregatePct;
};

// ─── STATE ─────────────────────────────────────────────
unsigned long lastRefreshMs    = 0;
unsigned long lastWifiRetryMs  = 0;
bool     forceRefresh          = false;
bool     firstBoot             = true;
bool     apModeActive          = false;
bool     displayNeedsStatus    = false;
float    lastPrice             = 0;
float    lastPctChange         = 0;
int      partialCount          = 0;
int      consecutiveFails      = 0;
unsigned long bootTime         = 0;
bool     otaActive             = false;
bool     otaNeedsRender        = false;
bool     otaDisplayReady       = false;
bool     otaFailed             = false;
int      otaProgressPct        = 0;
unsigned long otaLastRenderMs  = 0;
const unsigned long WIFI_RETRY_MS = 60000;    // retry STA every 60s when in AP mode
const unsigned long WIFI_CHECK_MS = 15000;    // check WiFi health every 15s
const unsigned long OTA_RENDER_INTERVAL_MS = 800;
unsigned long lastWifiCheckMs  = 0;
const int MAX_CONSECUTIVE_FAILS = 3;          // force WiFi reset after this many

bool     remoteOtaEnabled = false;
char     remoteOtaManifestUrl[257] = "";
int      remoteOtaCheckMin = 60;
char     remoteOtaChannel[16] = "stable";
bool     remoteOtaAutoApply = false;
bool     remoteOtaAllowDowngrade = false;
unsigned long lastRemoteOtaCheckMs = 0;
unsigned long remoteOtaNextAllowedMs = 0;
int      remoteOtaFailureCount = 0;

// ─── QUIET HOURS ────────────────────────────────────────
bool cfgQuietEnabled = false;
int  cfgQuietStart   = 23;  // 0-23 hour (local time)
int  cfgQuietEnd     = 7;   // 0-23 hour (local time)

// ─── FRAMEBUFFER ────────────────────────────────────────
static unsigned char framebuf[BUF_SIZE];
static unsigned char oldbuf[BUF_SIZE];

// ─── CANDLE DATA (scratch arrays used by fetch functions) ──
static Candle candles[MAX_CANDLES];
static int    candleCount = 0;
static float  emaFast[MAX_CANDLES];
static float  emaSlow[MAX_CANDLES];
static float  rsiVal[MAX_CANDLES];

// ─── CHART SLOTS ────────────────────────────────────────
static ChartSlot slots[MAX_SLOTS];
static MoodInfo currentMood = {MOOD_NEUTRAL, "STABLE", "none", 0.0f};

// ─── OBJECTS ────────────────────────────────────────────
Epd       epd;
WebServer server(80);

// ─── FORWARD DECLARATIONS ──────────────────────────────
void startAPMode();
void setupWebServer();
void renderOtaProgressScreen(int pct, bool failed);
void updateOtaDisplay(bool forceFullRefresh);
bool authenticateRequest();
MoodInfo getAggregateMood();
MoodInfo moodFromPct(float pct);
void drawMoodHud(const Viewport& vp, const MoodInfo& mood, bool fullScreen);
void handlePoloniexMarkets();
void remoteOtaTick();

// ═══════════════════════════════════════════════════════
// 5x7 FONT
// ═══════════════════════════════════════════════════════
static const unsigned char font5x7[][5] PROGMEM = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 space
    {0x00,0x00,0x5F,0x00,0x00}, // 33 !
    {0x00,0x07,0x00,0x07,0x00}, // 34 "
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 #
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 $
    {0x23,0x13,0x08,0x64,0x62}, // 37 %
    {0x36,0x49,0x55,0x22,0x50}, // 38 &
    {0x00,0x05,0x03,0x00,0x00}, // 39 '
    {0x00,0x1C,0x22,0x41,0x00}, // 40 (
    {0x00,0x41,0x22,0x1C,0x00}, // 41 )
    {0x08,0x2A,0x1C,0x2A,0x08}, // 42 *
    {0x08,0x08,0x3E,0x08,0x08}, // 43 +
    {0x00,0x50,0x30,0x00,0x00}, // 44 ,
    {0x08,0x08,0x08,0x08,0x08}, // 45 -
    {0x00,0x60,0x60,0x00,0x00}, // 46 .
    {0x20,0x10,0x08,0x04,0x02}, // 47 /
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 0
    {0x00,0x42,0x7F,0x40,0x00}, // 49 1
    {0x42,0x61,0x51,0x49,0x46}, // 50 2
    {0x21,0x41,0x45,0x4B,0x31}, // 51 3
    {0x18,0x14,0x12,0x7F,0x10}, // 52 4
    {0x27,0x45,0x45,0x45,0x39}, // 53 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 6
    {0x01,0x71,0x09,0x05,0x03}, // 55 7
    {0x36,0x49,0x49,0x49,0x36}, // 56 8
    {0x06,0x49,0x49,0x29,0x1E}, // 57 9
    {0x00,0x36,0x36,0x00,0x00}, // 58 :
    {0x00,0x56,0x36,0x00,0x00}, // 59 ;
    {0x00,0x08,0x14,0x22,0x41}, // 60 <
    {0x14,0x14,0x14,0x14,0x14}, // 61 =
    {0x41,0x22,0x14,0x08,0x00}, // 62 >
    {0x02,0x01,0x51,0x09,0x06}, // 63 ?
    {0x32,0x49,0x79,0x41,0x3E}, // 64 @
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 A
    {0x7F,0x49,0x49,0x49,0x36}, // 66 B
    {0x3E,0x41,0x41,0x41,0x22}, // 67 C
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 D
    {0x7F,0x49,0x49,0x49,0x41}, // 69 E
    {0x7F,0x09,0x09,0x01,0x01}, // 70 F
    {0x3E,0x41,0x41,0x51,0x32}, // 71 G
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 H
    {0x00,0x41,0x7F,0x41,0x00}, // 73 I
    {0x20,0x40,0x41,0x3F,0x01}, // 74 J
    {0x7F,0x08,0x14,0x22,0x41}, // 75 K
    {0x7F,0x40,0x40,0x40,0x40}, // 76 L
    {0x7F,0x02,0x04,0x02,0x7F}, // 77 M
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 N
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 O
    {0x7F,0x09,0x09,0x09,0x06}, // 80 P
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 Q
    {0x7F,0x09,0x19,0x29,0x46}, // 82 R
    {0x46,0x49,0x49,0x49,0x31}, // 83 S
    {0x01,0x01,0x7F,0x01,0x01}, // 84 T
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 U
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 V
    {0x7F,0x20,0x18,0x20,0x7F}, // 87 W
    {0x63,0x14,0x08,0x14,0x63}, // 88 X
    {0x03,0x04,0x78,0x04,0x03}, // 89 Y
    {0x61,0x51,0x49,0x45,0x43}, // 90 Z
    {0x00,0x00,0x7F,0x41,0x41}, // 91 [
    {0x02,0x04,0x08,0x10,0x20}, // 92 backslash
    {0x41,0x41,0x7F,0x00,0x00}, // 93 ]
    {0x04,0x02,0x01,0x02,0x04}, // 94 ^
    {0x40,0x40,0x40,0x40,0x40}, // 95 _
    {0x00,0x01,0x02,0x04,0x00}, // 96 `
    {0x20,0x54,0x54,0x54,0x78}, // 97 a
    {0x7F,0x48,0x44,0x44,0x38}, // 98 b
    {0x38,0x44,0x44,0x44,0x20}, // 99 c
    {0x38,0x44,0x44,0x48,0x7F}, // 100 d
    {0x38,0x54,0x54,0x54,0x18}, // 101 e
    {0x08,0x7E,0x09,0x01,0x02}, // 102 f
    {0x08,0x14,0x54,0x54,0x3C}, // 103 g
    {0x7F,0x08,0x04,0x04,0x78}, // 104 h
    {0x00,0x44,0x7D,0x40,0x00}, // 105 i
    {0x20,0x40,0x44,0x3D,0x00}, // 106 j
    {0x00,0x7F,0x10,0x28,0x44}, // 107 k
    {0x00,0x41,0x7F,0x40,0x00}, // 108 l
    {0x7C,0x04,0x18,0x04,0x78}, // 109 m
    {0x7C,0x08,0x04,0x04,0x78}, // 110 n
    {0x38,0x44,0x44,0x44,0x38}, // 111 o
    {0x7C,0x14,0x14,0x14,0x08}, // 112 p
    {0x08,0x14,0x14,0x18,0x7C}, // 113 q
    {0x7C,0x08,0x04,0x04,0x08}, // 114 r
    {0x48,0x54,0x54,0x54,0x20}, // 115 s
    {0x04,0x3F,0x44,0x40,0x20}, // 116 t
    {0x3C,0x40,0x40,0x20,0x7C}, // 117 u
    {0x1C,0x20,0x40,0x20,0x1C}, // 118 v
    {0x3C,0x40,0x30,0x40,0x3C}, // 119 w
    {0x44,0x28,0x10,0x28,0x44}, // 120 x
    {0x0C,0x50,0x50,0x50,0x3C}, // 121 y
    {0x44,0x64,0x54,0x4C,0x44}, // 122 z
    {0x00,0x08,0x36,0x41,0x00}, // 123 {
    {0x00,0x00,0x7F,0x00,0x00}, // 124 |
    {0x00,0x41,0x36,0x08,0x00}, // 125 }
    {0x08,0x08,0x2A,0x1C,0x08}, // 126 ~
};

// ═══════════════════════════════════════════════════════
// DRAWING PRIMITIVES
// ═══════════════════════════════════════════════════════

void bufClear() { memset(framebuf, 0xFF, BUF_SIZE); }

void setPixel(int x, int y, bool black) {
    if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H) return;
    int byteIdx = y * (SCR_W / 8) + x / 8;
    int bitIdx  = 7 - (x % 8);
    if (black) framebuf[byteIdx] &= ~(1 << bitIdx);
    else       framebuf[byteIdx] |=  (1 << bitIdx);
}

void hLine(int x0, int x1, int y) {
    for (int x = x0; x <= x1; x++) setPixel(x, y, true);
}

void vLine(int x, int y0, int y1) {
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) setPixel(x, y, true);
}

void fillRect(int x0, int y0, int w, int h, bool black) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            setPixel(x, y, black);
}

void drawRect(int x0, int y0, int w, int h) {
    hLine(x0, x0 + w - 1, y0);
    hLine(x0, x0 + w - 1, y0 + h - 1);
    vLine(x0, y0, y0 + h - 1);
    vLine(x0 + w - 1, y0, y0 + h - 1);
}

void hLineDash(int x0, int x1, int y, int dashLen = 4, int gapLen = 4) {
    bool draw = true; int count = 0;
    for (int x = x0; x <= x1; x++) {
        if (draw) setPixel(x, y, true);
        count++;
        if (draw && count >= dashLen)  { draw = false; count = 0; }
        if (!draw && count >= gapLen)  { draw = true;  count = 0; }
    }
}

void hLineDot(int x0, int x1, int y) {
    for (int x = x0; x <= x1; x += 2) setPixel(x, y, true);
}

void drawChar(int x, int y, char c, int scale = 1) {
    if (c < 32 || c > 126) c = '?';
    int idx = c - 32;
    for (int col = 0; col < 5; col++) {
        unsigned char line = pgm_read_byte(&font5x7[idx][col]);
        for (int row = 0; row < 7; row++) {
            if (line & (1 << row)) {
                if (scale == 1) setPixel(x + col, y + row, true);
                else fillRect(x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

void drawString(int x, int y, const char* str, int scale = 1) {
    int spacing = 6 * scale;
    while (*str) { drawChar(x, y, *str, scale); x += spacing; str++; }
}

void drawStringR(int x, int y, const char* str, int scale = 1) {
    int spacing = 6 * scale;
    drawString(x - strlen(str) * spacing, y, str, scale);
}

void drawBitmapScaledFromProgmem(const uint8_t* bmp, int bmpW, int bmpH, int x0, int y0, int scale) {
    if (scale < 1) scale = 1;
    int bytesPerRow = (bmpW + 7) / 8;

    for (int y = 0; y < bmpH; y++) {
        for (int x = 0; x < bmpW; x++) {
            int byteIndex = y * bytesPerRow + (x >> 3);
            uint8_t byteVal = pgm_read_byte(bmp + byteIndex);
            bool isBlack = (byteVal & (0x80 >> (x & 7))) != 0;
            if (!isBlack) continue;

            if (scale == 1) {
                setPixel(x0 + x, y0 + y, true);
            } else {
                fillRect(x0 + x * scale, y0 + y * scale, scale, scale, true);
            }
        }
    }
}

void drawLine(int x0, int y0, int x1, int y1, bool dashed = false) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    int step = 0;
    for (;;) {
        if (!dashed || (step / 3) % 2 == 0) setPixel(x0, y0, true);
        step++;
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ═══════════════════════════════════════════════════════
// INDICATORS
// ═══════════════════════════════════════════════════════

void calcEMA(float* out, int period) {
    if (candleCount == 0) return;
    float k = 2.0f / (period + 1);
    out[0] = candles[0].c;
    for (int i = 1; i < candleCount; i++)
        out[i] = candles[i].c * k + out[i - 1] * (1.0f - k);
}

void calcRSI() {
    for (int i = 0; i < candleCount; i++) rsiVal[i] = 50.0f;
    if (candleCount < cfgRsiPeriod + 1) return;

    float avgGain = 0, avgLoss = 0;
    for (int i = 1; i <= cfgRsiPeriod; i++) {
        float delta = candles[i].c - candles[i - 1].c;
        if (delta > 0) avgGain += delta; else avgLoss += (-delta);
    }
    avgGain /= cfgRsiPeriod;
    avgLoss /= cfgRsiPeriod;

    if (avgLoss == 0) rsiVal[cfgRsiPeriod] = 100.0f;
    else {
        float rs = avgGain / avgLoss;
        rsiVal[cfgRsiPeriod] = 100.0f - (100.0f / (1.0f + rs));
    }

    for (int i = cfgRsiPeriod + 1; i < candleCount; i++) {
        float delta = candles[i].c - candles[i - 1].c;
        float gain = (delta > 0) ? delta : 0;
        float loss = (delta < 0) ? (-delta) : 0;
        avgGain = (avgGain * (cfgRsiPeriod - 1) + gain) / cfgRsiPeriod;
        avgLoss = (avgLoss * (cfgRsiPeriod - 1) + loss) / cfgRsiPeriod;
        if (avgLoss == 0) rsiVal[i] = 100.0f;
        else {
            float rs = avgGain / avgLoss;
            rsiVal[i] = 100.0f - (100.0f / (1.0f + rs));
        }
    }
}



const char* slotEventTypeToStr(SlotEventType type) {
    switch (type) {
        case SLOT_EVENT_EMA_CROSS_UP: return "ema_cross_up";
        case SLOT_EVENT_EMA_CROSS_DOWN: return "ema_cross_down";
        case SLOT_EVENT_RSI_ENTER_OVERBOUGHT: return "rsi_enter_overbought";
        case SLOT_EVENT_RSI_EXIT_OVERBOUGHT: return "rsi_exit_overbought";
        case SLOT_EVENT_RSI_ENTER_OVERSOLD: return "rsi_enter_oversold";
        case SLOT_EVENT_RSI_EXIT_OVERSOLD: return "rsi_exit_oversold";
        case SLOT_EVENT_BREAKOUT_HIGH: return "breakout_high";
        case SLOT_EVENT_BREAKOUT_LOW: return "breakout_low";
        case SLOT_EVENT_NONE:
        default: return "none";
    }
}

void setSlotEvent(ChartSlot& slot, SlotEventType type, uint64_t ts, const char* msg) {
    slot.lastEvent.ts = ts;
    slot.lastEvent.type = type;
    const char* safeMsg = msg ? msg : "";
    strncpy(slot.lastEvent.message, safeMsg, sizeof(slot.lastEvent.message) - 1);
    slot.lastEvent.message[sizeof(slot.lastEvent.message) - 1] = '\0';
}

void detectSlotEvents(ChartSlot& slot) {
    if (!slot.cfg.eventCallouts || slot.candleCount < 3) return;

    int i = slot.candleCount - 1;
    int prev = i - 1;
    uint64_t ts = slot.candles[i].t;

    float fastPrev = slot.emaFastArr[prev];
    float slowPrev = slot.emaSlowArr[prev];
    float fastNow = slot.emaFastArr[i];
    float slowNow = slot.emaSlowArr[i];

    if (fastPrev <= slowPrev && fastNow > slowNow) {
        setSlotEvent(slot, SLOT_EVENT_EMA_CROSS_UP, ts, "EMA fast crossed above slow");
        return;
    }
    if (fastPrev >= slowPrev && fastNow < slowNow) {
        setSlotEvent(slot, SLOT_EVENT_EMA_CROSS_DOWN, ts, "EMA fast crossed below slow");
        return;
    }

    float rsiPrev = slot.rsiVal[prev];
    float rsiNow = slot.rsiVal[i];
    if (rsiPrev <= 70.0f && rsiNow > 70.0f) {
        setSlotEvent(slot, SLOT_EVENT_RSI_ENTER_OVERBOUGHT, ts, "RSI entered overbought (>70)");
        return;
    }
    if (rsiPrev > 70.0f && rsiNow <= 70.0f) {
        setSlotEvent(slot, SLOT_EVENT_RSI_EXIT_OVERBOUGHT, ts, "RSI exited overbought");
        return;
    }
    if (rsiPrev >= 30.0f && rsiNow < 30.0f) {
        setSlotEvent(slot, SLOT_EVENT_RSI_ENTER_OVERSOLD, ts, "RSI entered oversold (<30)");
        return;
    }
    if (rsiPrev < 30.0f && rsiNow >= 30.0f) {
        setSlotEvent(slot, SLOT_EVENT_RSI_EXIT_OVERSOLD, ts, "RSI exited oversold");
        return;
    }

    int lookback = min(20, slot.candleCount - 1);
    float recentHi = -1e9f;
    float recentLo = 1e9f;
    for (int j = i - lookback; j < i; j++) {
        if (slot.candles[j].h > recentHi) recentHi = slot.candles[j].h;
        if (slot.candles[j].l < recentLo) recentLo = slot.candles[j].l;
    }
    float closeNow = slot.candles[i].c;
    if (closeNow > recentHi) {
        setSlotEvent(slot, SLOT_EVENT_BREAKOUT_HIGH, ts, "Breakout above recent high");
        return;
    }
    if (closeNow < recentLo) {
        setSlotEvent(slot, SLOT_EVENT_BREAKOUT_LOW, ts, "Breakdown below recent low");
        return;
    }
}
// ═══════════════════════════════════════════════════════
// INTERVAL HELPERS
// ═══════════════════════════════════════════════════════

uint64_t intervalToMs(const char* iv) {
    int val = atoi(iv);
    if (val == 0) val = 1;
    char unit = iv[strlen(iv) - 1];
    switch (unit) {
        case 'm': return (uint64_t)val * 60000ULL;
        case 'h': return (uint64_t)val * 3600000ULL;
        case 'd': return (uint64_t)val * 86400000ULL;
        case 'w': return (uint64_t)val * 604800000ULL;
        case 'M': return (uint64_t)val * 2592000000ULL;
        default:  return 300000ULL;
    }
}

// Returns the effective display refresh interval in ms.
// When cfgAutoRefresh is on, uses the smallest interval across active slots.
// Falls back to cfgRefreshMin if clock isn't ready or slots have no valid interval.
unsigned long effectiveRefreshMs() {
    if (!cfgAutoRefresh) {
        return (unsigned long)cfgRefreshMin * 60UL * 1000UL;
    }
    uint64_t minMs = UINT64_MAX;
    for (int i = 0; i < cfgLayout && i < MAX_SLOTS; i++) {
        uint64_t ms = intervalToMs(slots[i].cfg.interval);
        if (ms > 0 && ms < minMs) minMs = ms;
    }
    if (minMs == UINT64_MAX) {
        return (unsigned long)cfgRefreshMin * 60UL * 1000UL;
    }
    // Clamp to minimum 1 minute (avoid hammering the display/API)
    if (minMs < 60000ULL) minMs = 60000ULL;
    return (unsigned long)minMs;
}

int autoCandles(const char* iv) {
    if (strcmp(iv, "1m")  == 0) return 120;
    if (strcmp(iv, "3m")  == 0) return 80;
    if (strcmp(iv, "5m")  == 0) return 60;
    if (strcmp(iv, "15m") == 0) return 48;
    if (strcmp(iv, "30m") == 0) return 48;
    if (strcmp(iv, "1h")  == 0) return 48;
    if (strcmp(iv, "2h")  == 0) return 42;
    if (strcmp(iv, "4h")  == 0) return 42;
    if (strcmp(iv, "8h")  == 0) return 30;
    if (strcmp(iv, "12h") == 0) return 30;
    if (strcmp(iv, "1d")  == 0) return 30;
    if (strcmp(iv, "3d")  == 0) return 20;
    if (strcmp(iv, "1w")  == 0) return 26;
    if (strcmp(iv, "1M")  == 0) return 12;
    return 60;
}

// Returns the effective candle count (auto or manual)
int getNumCandles() {
    int n = cfgAutoCandles ? autoCandles(cfgInterval) : cfgNumCandles;
    return constrain(n, 5, MAX_CANDLES);
}

// Human-readable time span for a given interval + count
void timeSpanStr(const char* iv, int count, char* out, int outLen) {
    uint64_t totalMin = (intervalToMs(iv) / 60000ULL) * count;
    if (totalMin < 60)         snprintf(out, outLen, "%llum", totalMin);
    else if (totalMin < 1440)  snprintf(out, outLen, "%.1fh", totalMin / 60.0);
    else                       snprintf(out, outLen, "%.1fd", totalMin / 1440.0);
}

// ═══════════════════════════════════════════════════════
// EXCHANGE HELPERS — interval & symbol conversion
// ═══════════════════════════════════════════════════════

// Convert common interval format to Kraken minutes string
void krakenInterval(const char* iv, char* out, int outLen) {
    if (strcmp(iv, "1m") == 0)       strncpy(out, "1", outLen);
    else if (strcmp(iv, "5m") == 0)  strncpy(out, "5", outLen);
    else if (strcmp(iv, "15m") == 0) strncpy(out, "15", outLen);
    else if (strcmp(iv, "30m") == 0) strncpy(out, "30", outLen);
    else if (strcmp(iv, "1h") == 0)  strncpy(out, "60", outLen);
    else if (strcmp(iv, "4h") == 0)  strncpy(out, "240", outLen);
    else if (strcmp(iv, "1d") == 0)  strncpy(out, "1440", outLen);
    else if (strcmp(iv, "1w") == 0)  strncpy(out, "10080", outLen);
    else                             strncpy(out, "60", outLen);  // fallback
}

// Convert common interval format to Poloniex format
void poloniexInterval(const char* iv, char* out, int outLen) {
    if (strcmp(iv, "1m") == 0)       strncpy(out, "MINUTE_1", outLen);
    else if (strcmp(iv, "5m") == 0)  strncpy(out, "MINUTE_5", outLen);
    else if (strcmp(iv, "15m") == 0) strncpy(out, "MINUTE_15", outLen);
    else if (strcmp(iv, "30m") == 0) strncpy(out, "MINUTE_30", outLen);
    else if (strcmp(iv, "1h") == 0)  strncpy(out, "HOUR_1", outLen);
    else if (strcmp(iv, "2h") == 0)  strncpy(out, "HOUR_2", outLen);
    else if (strcmp(iv, "4h") == 0)  strncpy(out, "HOUR_4", outLen);
    else if (strcmp(iv, "6h") == 0)  strncpy(out, "HOUR_6", outLen);
    else if (strcmp(iv, "12h") == 0) strncpy(out, "HOUR_12", outLen);
    else if (strcmp(iv, "1d") == 0)  strncpy(out, "DAY_1", outLen);
    else if (strcmp(iv, "3d") == 0)  strncpy(out, "DAY_3", outLen);
    else if (strcmp(iv, "1w") == 0)  strncpy(out, "WEEK_1", outLen);
    else if (strcmp(iv, "1M") == 0)  strncpy(out, "MONTH_1", outLen);
    else                             strncpy(out, "HOUR_1", outLen);  // fallback
}

// ═══════════════════════════════════════════════════════
// CONFIG HELPERS + NVS PERSISTENCE
// ═══════════════════════════════════════════════════════

void copyBounded(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

void toUpperInPlace(char* s) {
    if (!s) return;
    for (size_t i = 0; s[i] != '\0'; i++) s[i] = toupper((unsigned char)s[i]);
}

bool isValidExchange(const char* s) {
    return s && (strcmp(s, "hyperliquid") == 0 || strcmp(s, "binance") == 0 ||
                 strcmp(s, "asterdex") == 0 || strcmp(s, "kraken") == 0 ||
                 strcmp(s, "poloniex") == 0 || strcmp(s, "okx") == 0);
}

// Map common interval strings to OKX bar parameter format
static void intervalToOkxBar(const char* iv, char* out, size_t outLen) {
    if      (strcmp(iv, "1h")  == 0) strncpy(out, "1H",    outLen);
    else if (strcmp(iv, "2h")  == 0) strncpy(out, "2H",    outLen);
    else if (strcmp(iv, "4h")  == 0) strncpy(out, "4H",    outLen);
    else if (strcmp(iv, "6h")  == 0) strncpy(out, "6H",    outLen);
    else if (strcmp(iv, "8h")  == 0) strncpy(out, "8H",    outLen);
    else if (strcmp(iv, "12h") == 0) strncpy(out, "12H",   outLen);
    else if (strcmp(iv, "1d")  == 0) strncpy(out, "1Dutc", outLen);
    else if (strcmp(iv, "3d")  == 0) strncpy(out, "3Dutc", outLen);
    else if (strcmp(iv, "1w")  == 0) strncpy(out, "1Wutc", outLen);
    else if (strcmp(iv, "1M")  == 0) strncpy(out, "1Mutc", outLen);
    else strncpy(out, iv, outLen);
    out[outLen - 1] = '\0';
}

bool isInQuietHours() {
    if (!cfgQuietEnabled) return false;
    time_t now = time(nullptr);
    if (now < 100000) return false;  // clock not synced yet
    struct tm* t = localtime(&now);
    int h = t->tm_hour;
    if (cfgQuietStart <= cfgQuietEnd) {
        return h >= cfgQuietStart && h < cfgQuietEnd;
    } else {
        // wraps midnight (e.g. 23:00–07:00)
        return h >= cfgQuietStart || h < cfgQuietEnd;
    }
}

bool isValidInterval(const char* s) {
    static const char* allowed[] = {"1m","3m","5m","15m","30m","1h","2h","4h","6h","8h","12h","1d","3d","1w","1M"};
    if (!s) return false;
    for (size_t i = 0; i < sizeof(allowed)/sizeof(allowed[0]); i++) {
        if (strcmp(s, allowed[i]) == 0) return true;
    }
    return false;
}

bool isValidSymbolToken(const char* s, size_t maxLen) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n < 2 || n >= maxLen) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)s[i])) return false;
    }
    return true;
}

bool isHttpUrl(const char* s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n < 10 || n > 256) return false;
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

bool isValidRemoteChannel(const char* s) {
    return s && (strcmp(s, "stable") == 0 || strcmp(s, "beta") == 0);
}

bool parseSemver(const char* v, int out[3]) {
    if (!v || !out) return false;
    const char* p = v;
    if (*p == 'v' || *p == 'V') p++;
    int major = 0, minor = 0, patch = 0;
    if (sscanf(p, "%d.%d.%d", &major, &minor, &patch) != 3) return false;
    out[0] = major; out[1] = minor; out[2] = patch;
    return true;
}

int compareSemver(const char* a, const char* b) {
    int av[3], bv[3];
    if (!parseSemver(a, av) || !parseSemver(b, bv)) return strcmp(a ? a : "", b ? b : "");
    for (int i = 0; i < 3; i++) {
        if (av[i] < bv[i]) return -1;
        if (av[i] > bv[i]) return 1;
    }
    return 0;
}

bool authEnabled() {
    return cfgUiUser[0] != '\0' && cfgUiPass[0] != '\0';
}

bool authenticateRequest() {
    if (!authEnabled()) return true;
    if (server.authenticate(cfgUiUser, cfgUiPass)) return true;
    server.requestAuthentication(BASIC_AUTH, "EPD Chart", "Authentication required");
    return false;
}

void initSlotDefaults(SlotConfig& cfg) {
    copyBounded(cfg.exchange, sizeof(cfg.exchange), "hyperliquid");
    copyBounded(cfg.coin, sizeof(cfg.coin), "ETH");
    copyBounded(cfg.quote, sizeof(cfg.quote), "USDT");
    copyBounded(cfg.interval, sizeof(cfg.interval), "5m");
    cfg.numCandles = 60;
    cfg.autoCandles = true;
    cfg.emaFast = 9;
    cfg.emaSlow = 21;
    cfg.rsiPeriod = 14;
    cfg.heikinAshi = false;
    cfg.eventCallouts = true;
}

void loadSlotConfig(int i) {
    char key[16];
    String s;
    snprintf(key, sizeof(key), "s%dexchange", i);
    s = prefs.getString(key, slots[i].cfg.exchange);
    copyBounded(slots[i].cfg.exchange, sizeof(slots[i].cfg.exchange), s.c_str());

    snprintf(key, sizeof(key), "s%dcoin", i);
    s = prefs.getString(key, slots[i].cfg.coin);
    copyBounded(slots[i].cfg.coin, sizeof(slots[i].cfg.coin), s.c_str());

    snprintf(key, sizeof(key), "s%dquote", i);
    s = prefs.getString(key, slots[i].cfg.quote);
    copyBounded(slots[i].cfg.quote, sizeof(slots[i].cfg.quote), s.c_str());

    snprintf(key, sizeof(key), "s%dinterval", i);
    s = prefs.getString(key, slots[i].cfg.interval);
    copyBounded(slots[i].cfg.interval, sizeof(slots[i].cfg.interval), s.c_str());

    toUpperInPlace(slots[i].cfg.coin);
    toUpperInPlace(slots[i].cfg.quote);
    if (!isValidExchange(slots[i].cfg.exchange))
        copyBounded(slots[i].cfg.exchange, sizeof(slots[i].cfg.exchange), "hyperliquid");
    if (!isValidInterval(slots[i].cfg.interval))
        copyBounded(slots[i].cfg.interval, sizeof(slots[i].cfg.interval), "5m");
    if (!isValidSymbolToken(slots[i].cfg.coin, sizeof(slots[i].cfg.coin)))
        copyBounded(slots[i].cfg.coin, sizeof(slots[i].cfg.coin), "ETH");
    if (!isValidSymbolToken(slots[i].cfg.quote, sizeof(slots[i].cfg.quote)))
        copyBounded(slots[i].cfg.quote, sizeof(slots[i].cfg.quote), "USDT");

    snprintf(key, sizeof(key), "s%dnumCandl", i);
    slots[i].cfg.numCandles = prefs.getInt(key, slots[i].cfg.numCandles);
    snprintf(key, sizeof(key), "s%dautoCandl", i);
    slots[i].cfg.autoCandles = prefs.getBool(key, slots[i].cfg.autoCandles);
    snprintf(key, sizeof(key), "s%demaFast", i);
    slots[i].cfg.emaFast = prefs.getInt(key, slots[i].cfg.emaFast);
    snprintf(key, sizeof(key), "s%demaSlow", i);
    slots[i].cfg.emaSlow = prefs.getInt(key, slots[i].cfg.emaSlow);
    snprintf(key, sizeof(key), "s%drsiPeriod", i);
    slots[i].cfg.rsiPeriod = prefs.getInt(key, slots[i].cfg.rsiPeriod);
    snprintf(key, sizeof(key), "s%dheikinAsh", i);
    slots[i].cfg.heikinAshi = prefs.getBool(key, slots[i].cfg.heikinAshi);
    snprintf(key, sizeof(key), "s%deventCall", i);
    slots[i].cfg.eventCallouts = prefs.getBool(key, slots[i].cfg.eventCallouts);
}

void saveSlotConfig(int i) {
    char key[16];
    snprintf(key, sizeof(key), "s%dexchange", i);
    prefs.putString(key, slots[i].cfg.exchange);
    snprintf(key, sizeof(key), "s%dcoin", i);
    prefs.putString(key, slots[i].cfg.coin);
    snprintf(key, sizeof(key), "s%dquote", i);
    prefs.putString(key, slots[i].cfg.quote);
    snprintf(key, sizeof(key), "s%dinterval", i);
    prefs.putString(key, slots[i].cfg.interval);
    snprintf(key, sizeof(key), "s%dnumCandl", i);
    prefs.putInt(key, slots[i].cfg.numCandles);
    snprintf(key, sizeof(key), "s%dautoCandl", i);
    prefs.putBool(key, slots[i].cfg.autoCandles);
    snprintf(key, sizeof(key), "s%demaFast", i);
    prefs.putInt(key, slots[i].cfg.emaFast);
    snprintf(key, sizeof(key), "s%demaSlow", i);
    prefs.putInt(key, slots[i].cfg.emaSlow);
    snprintf(key, sizeof(key), "s%drsiPeriod", i);
    prefs.putInt(key, slots[i].cfg.rsiPeriod);
    snprintf(key, sizeof(key), "s%dheikinAsh", i);
    prefs.putBool(key, slots[i].cfg.heikinAshi);
    snprintf(key, sizeof(key), "s%deventCall", i);
    prefs.putBool(key, slots[i].cfg.eventCallouts);
}

void loadConfig() {
    prefs.begin("epdchart", true);  // read-only
    String s;
    s = prefs.getString("ssid", cfgSSID);        copyBounded(cfgSSID, sizeof(cfgSSID), s.c_str());
    s = prefs.getString("pass", cfgPass);        copyBounded(cfgPass, sizeof(cfgPass), s.c_str());
    s = prefs.getString("uiUser", cfgUiUser);    copyBounded(cfgUiUser, sizeof(cfgUiUser), s.c_str());
    s = prefs.getString("uiPass", cfgUiPass);    copyBounded(cfgUiPass, sizeof(cfgUiPass), s.c_str());

    cfgRefreshMin   = prefs.getInt("refreshMin", cfgRefreshMin);
    cfgTzOffset     = prefs.getInt("tzOffset", cfgTzOffset);
    cfgFullRefEvery = prefs.getInt("fullRefEv", cfgFullRefEvery);
    cfgPartialPct   = prefs.getInt("partialPct", cfgPartialPct);
    cfgLayout       = prefs.getInt("layout", 1);
    if (cfgLayout < 1 || cfgLayout > 4) cfgLayout = 1;
    cfgAutoRefresh  = prefs.getBool("autoRefresh", true);
    cfgPersonalityEnabled = prefs.getBool("personality", true);
    cfgCaptionVerbosity = constrain(prefs.getInt("captionVerb", 1), 0, 2);
    remoteOtaEnabled = prefs.getBool("rOtaEn", false);
    s = prefs.getString("rOtaUrl", "");
    copyBounded(remoteOtaManifestUrl, sizeof(remoteOtaManifestUrl), s.c_str());
    remoteOtaCheckMin = constrain(prefs.getInt("rOtaChkMin", 60), 5, 1440);
    s = prefs.getString("rOtaChan", "stable");
    copyBounded(remoteOtaChannel, sizeof(remoteOtaChannel), s.c_str());
    if (!(strcmp(remoteOtaChannel, "stable") == 0 || strcmp(remoteOtaChannel, "beta") == 0)) {
        copyBounded(remoteOtaChannel, sizeof(remoteOtaChannel), "stable");
    }
    remoteOtaAutoApply = prefs.getBool("rOtaAuto", false);
    remoteOtaAllowDowngrade = prefs.getBool("rOtaAllowDw", false);
    cfgQuietEnabled = prefs.getBool("quietEn", false);
    cfgQuietStart   = constrain(prefs.getInt("quietStart", 23), 0, 23);
    cfgQuietEnd     = constrain(prefs.getInt("quietEnd",    7), 0, 23);

    // Initialize all slots with defaults
    for (int i = 0; i < MAX_SLOTS; i++) {
        initSlotDefaults(slots[i].cfg);
        slots[i].candleCount = 0;
        slots[i].lastPrice = 0;
        slots[i].lastPctChange = 0;
        slots[i].lastEvent.ts = 0;
        slots[i].lastEvent.type = SLOT_EVENT_NONE;
        slots[i].lastEvent.message[0] = '\0';
        slots[i].lastFetchOk = false;
        slots[i].lastFetchMs = 0;
    }

    // Check for migration from old single-chart format
    bool hasNewFormat = prefs.isKey("s0exchange");
    bool hasOldFormat = prefs.isKey("exchange");

    if (hasNewFormat) {
        // New multi-slot format
        for (int i = 0; i < MAX_SLOTS; i++) {
            loadSlotConfig(i);
        }
    } else if (hasOldFormat) {
        // Migrate from old single-chart format into slot 0
        s = prefs.getString("exchange", "hyperliquid");
        copyBounded(slots[0].cfg.exchange, sizeof(slots[0].cfg.exchange), s.c_str());
        s = prefs.getString("coin", "ETH");
        copyBounded(slots[0].cfg.coin, sizeof(slots[0].cfg.coin), s.c_str());
        s = prefs.getString("quote", "USDT");
        copyBounded(slots[0].cfg.quote, sizeof(slots[0].cfg.quote), s.c_str());
        s = prefs.getString("interval", "5m");
        copyBounded(slots[0].cfg.interval, sizeof(slots[0].cfg.interval), s.c_str());

        toUpperInPlace(slots[0].cfg.coin);
        toUpperInPlace(slots[0].cfg.quote);
        if (!isValidExchange(slots[0].cfg.exchange))
            copyBounded(slots[0].cfg.exchange, sizeof(slots[0].cfg.exchange), "hyperliquid");
        if (!isValidInterval(slots[0].cfg.interval))
            copyBounded(slots[0].cfg.interval, sizeof(slots[0].cfg.interval), "5m");

        slots[0].cfg.numCandles  = prefs.getInt("numCandles", 60);
        slots[0].cfg.autoCandles = prefs.getBool("autoCandl", true);
        slots[0].cfg.emaFast     = prefs.getInt("emaFast", 9);
        slots[0].cfg.emaSlow     = prefs.getInt("emaSlow", 21);
        slots[0].cfg.rsiPeriod   = prefs.getInt("rsiPeriod", 14);
        slots[0].cfg.heikinAshi  = prefs.getBool("heikinAshi", false);
        slots[0].cfg.eventCallouts = true;

        // Copy slot 0 config to slots 1-3
        for (int i = 1; i < MAX_SLOTS; i++) {
            memcpy(&slots[i].cfg, &slots[0].cfg, sizeof(SlotConfig));
        }
        cfgLayout = 1;
        Serial.println("Migrated old single-chart config to slot 0");
    }

    // Copy slot 0 config into scratch globals (for backward compat with fetch functions)
    copyBounded(cfgExchange, sizeof(cfgExchange), slots[0].cfg.exchange);
    copyBounded(cfgCoin, sizeof(cfgCoin), slots[0].cfg.coin);
    copyBounded(cfgQuote, sizeof(cfgQuote), slots[0].cfg.quote);
    copyBounded(cfgInterval, sizeof(cfgInterval), slots[0].cfg.interval);
    cfgNumCandles  = slots[0].cfg.numCandles;
    cfgAutoCandles = slots[0].cfg.autoCandles;
    cfgEmaFast     = slots[0].cfg.emaFast;
    cfgEmaSlow     = slots[0].cfg.emaSlow;
    cfgRsiPeriod   = slots[0].cfg.rsiPeriod;
    cfgHeikinAshi  = slots[0].cfg.heikinAshi;

    prefs.end();
    Serial.printf("Config loaded from NVS (auth:%s, layout:%d)\n", authEnabled() ? "on" : "off", cfgLayout);

    // Save new format if migrated from old
    if (hasOldFormat && !hasNewFormat) {
        saveConfig();
    }
}

void saveConfig() {
    prefs.begin("epdchart", false);  // read-write
    prefs.putString("ssid", cfgSSID);
    prefs.putString("pass", cfgPass);
    prefs.putString("uiUser", cfgUiUser);
    prefs.putString("uiPass", cfgUiPass);
    prefs.putInt("refreshMin", cfgRefreshMin);
    prefs.putBool("autoRefresh", cfgAutoRefresh);
    prefs.putInt("tzOffset", cfgTzOffset);
    prefs.putInt("fullRefEv", cfgFullRefEvery);
    prefs.putInt("partialPct", cfgPartialPct);
    prefs.putInt("layout", cfgLayout);
    prefs.putBool("personality", cfgPersonalityEnabled);
    prefs.putInt("captionVerb", cfgCaptionVerbosity);
    prefs.putBool("rOtaEn", remoteOtaEnabled);
    prefs.putString("rOtaUrl", remoteOtaManifestUrl);
    prefs.putInt("rOtaChkMin", remoteOtaCheckMin);
    prefs.putString("rOtaChan", remoteOtaChannel);
    prefs.putBool("rOtaAuto", remoteOtaAutoApply);
    prefs.putBool("rOtaAllowDw", remoteOtaAllowDowngrade);
    prefs.putBool("quietEn", cfgQuietEnabled);
    prefs.putInt("quietStart", cfgQuietStart);
    prefs.putInt("quietEnd", cfgQuietEnd);

    for (int i = 0; i < MAX_SLOTS; i++) {
        saveSlotConfig(i);
    }

    prefs.end();
    Serial.println("Config saved to NVS");
}

// ═══════════════════════════════════════════════════════
// WEB UI — HTML (served from flash)
// ═══════════════════════════════════════════════════════

const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EPD Chart Config</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;600;700&family=DM+Sans:wght@400;500;600&display=swap');

  *{margin:0;padding:0;box-sizing:border-box}
  :root{
    --bg:#0a0e14;--surface:#11161d;--surface2:#1a2029;
    --border:#252d38;--border-h:#3a4555;
    --text:#c5cdd8;--text-dim:#6b7a8d;--text-bright:#e8edf3;
    --accent:#4fc3f7;--accent-dim:#1a3a4a;
    --green:#66bb6a;--green-dim:#1a3a24;
    --red:#ef5350;--red-dim:#3a1a1a;
    --amber:#ffb74d;
    --radius:10px;
  }
  body{
    font-family:'DM Sans',sans-serif;background:var(--bg);color:var(--text);
    min-height:100vh;padding:0;
  }
  .header{
    background:linear-gradient(135deg,rgba(13,21,32,0.72) 0%,rgba(17,29,43,0.72) 100%);
    backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);
    border-bottom:1px solid var(--border);padding:20px 24px;
    display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px;
  }
  .header h1{
    font-family:'JetBrains Mono',monospace;font-size:1.2em;font-weight:700;
    color:var(--text-bright);letter-spacing:-0.5px;
  }
  .header h1 span{color:var(--accent)}
  .status-pills{display:flex;gap:8px;flex-wrap:wrap}
  .pill{
    font-family:'JetBrains Mono',monospace;font-size:0.72em;
    padding:4px 10px;border-radius:20px;border:1px solid var(--border);
    background:rgba(17,22,29,0.6);color:var(--text-dim);white-space:nowrap;
  }
  .pill.live{border-color:var(--green);color:var(--green);background:rgba(26,58,36,0.6)}
  .pill.warn{border-color:var(--amber);color:var(--amber)}
  .pill.neg{border-color:var(--red);color:var(--red);background:rgba(58,26,26,0.6)}
  .pill.neutral{border-color:var(--accent);color:var(--accent);background:rgba(26,58,74,0.6)}

  .container{max-width:640px;margin:0 auto;padding:16px 16px 100px}

  .card{
    background:rgba(17,22,29,0.7);backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);
    border:1px solid var(--border);border-radius:var(--radius);
    margin-bottom:12px;overflow:hidden;
  }
  .card-head{
    padding:14px 18px 12px;border-bottom:1px solid var(--border);
    display:flex;align-items:center;gap:8px;
  }
  .card-head .icon{font-size:1.1em}
  .card-head h2{font-size:0.82em;font-weight:600;color:var(--text-bright);text-transform:uppercase;letter-spacing:0.5px}
  .card-body{padding:16px 18px 18px}

  .field{margin-bottom:14px}
  .field:last-child{margin-bottom:0}
  .field label{
    display:block;font-size:0.75em;font-weight:500;color:var(--text-dim);
    margin-bottom:5px;text-transform:uppercase;letter-spacing:0.3px;
  }
  .field .hint{font-size:0.7em;color:var(--text-dim);margin-top:3px;font-style:italic}

  input[type=text],input[type=number],input[type=password],select{
    width:100%;padding:9px 12px;background:rgba(10,14,20,0.6);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);border:1px solid var(--border);
    border-radius:6px;color:var(--text-bright);font-family:'JetBrains Mono',monospace;
    font-size:0.85em;outline:none;transition:border-color .2s;
  }
  input:focus,select:focus{border-color:var(--accent)}
  select{cursor:pointer;appearance:none;
    background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' fill='%236b7a8d'%3E%3Cpath d='M2 4l4 4 4-4'/%3E%3C/svg%3E");
    background-repeat:no-repeat;background-position:right 10px center;padding-right:30px;
  }

  .row{display:flex;gap:12px}
  .row .field{flex:1}

  .toggle-row{
    display:flex;align-items:center;justify-content:space-between;
    padding:10px 0;
  }
  .toggle-row .label{font-size:0.82em;color:var(--text)}
  .toggle{
    position:relative;width:44px;height:24px;cursor:pointer;
  }
  .toggle input{opacity:0;width:0;height:0}
  .toggle .slider{
    position:absolute;inset:0;background:var(--border);border-radius:12px;transition:.3s;
  }
  .toggle .slider:before{
    content:'';position:absolute;width:18px;height:18px;left:3px;bottom:3px;
    background:var(--text-dim);border-radius:50%;transition:.3s;
  }
  .toggle input:checked+.slider{background:var(--accent-dim);border:1px solid var(--accent)}
  .toggle input:checked+.slider:before{transform:translateX(20px);background:var(--accent)}

  .auto-info{
    font-family:'JetBrains Mono',monospace;font-size:0.78em;
    color:var(--accent);padding:8px 12px;background:rgba(26,58,74,0.55);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);
    border-radius:6px;margin-top:8px;border:1px solid #1e4a5e;
  }

  .actions{
    position:fixed;bottom:0;left:0;right:0;
    background:linear-gradient(transparent,rgba(10,14,20,0.75) 20%);backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);padding:12px 16px 20px;
    display:flex;gap:8px;justify-content:center;z-index:10;
  }
  .btn{
    font-family:'DM Sans',sans-serif;font-weight:600;font-size:0.82em;
    padding:10px 20px;border-radius:8px;border:1px solid var(--border);
    cursor:pointer;transition:all .2s;color:var(--text);background:rgba(26,32,41,0.65);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);
    display:flex;align-items:center;gap:6px;
  }
  .btn:hover{border-color:var(--border-h);background:rgba(37,45,56,0.7)}
  .btn.primary{background:rgba(26,58,74,0.65);border-color:var(--accent);color:var(--accent)}
  .btn.primary:hover{background:rgba(30,90,110,0.7)}
  .btn.danger{border-color:var(--red);color:var(--red);background:rgba(58,26,26,0.65)}
  .btn.danger:hover{background:rgba(74,26,26,0.7)}

  .toast{
    position:fixed;top:20px;left:50%;transform:translateX(-50%) translateY(-80px);
    font-family:'JetBrains Mono',monospace;font-size:0.8em;
    padding:10px 20px;border-radius:8px;z-index:100;transition:transform .4s ease;
    border:1px solid var(--green);background:rgba(26,58,36,0.75);backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);color:var(--green);
    pointer-events:none;
  }
  .toast.error{border-color:var(--red);background:rgba(58,26,26,0.75);color:var(--red)}
  .toast.show{transform:translateX(-50%) translateY(0)}

  .coin-wrap{position:relative}
  .coin-wrap input[type=text]{padding-right:30px}
  .coin-dd{
    display:none;position:absolute;top:100%;left:0;right:0;z-index:20;
    max-height:220px;overflow-y:auto;
    background:rgba(10,14,20,0.8);backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);border:1px solid var(--accent);border-top:0;border-radius:0 0 6px 6px;
  }
  .coin-dd.open{display:block}
  .coin-dd .coin-item{
    padding:7px 12px;font-family:'JetBrains Mono',monospace;font-size:0.82em;
    color:var(--text);cursor:pointer;
  }
  .coin-dd .coin-item:hover,.coin-dd .coin-item.hl{background:rgba(26,58,74,0.6);color:var(--accent)}
  .coin-dd .coin-empty{padding:10px 12px;font-size:0.78em;color:var(--text-dim);font-style:italic}
  .coin-loading{padding:10px 12px;font-size:0.78em;color:var(--accent);font-family:'JetBrains Mono',monospace}

  .layout-picker{display:flex;gap:8px}
  .layout-opt{flex:1;cursor:pointer}
  .layout-opt input[type=radio]{display:none}
  .layout-thumb{
    display:flex;flex-direction:column;align-items:center;gap:6px;
    padding:10px 8px;border:1px solid var(--border);border-radius:8px;
    background:rgba(10,14,20,0.6);transition:all .2s;
  }
  .layout-opt input:checked + .layout-thumb{
    border-color:var(--accent);background:rgba(26,58,74,0.55);
  }
  .layout-thumb span{
    font-family:'JetBrains Mono',monospace;font-size:0.75em;color:var(--text-dim);
  }
  .layout-opt input:checked + .layout-thumb span{color:var(--accent)}
  .lt-grid{display:grid;gap:2px;width:48px;height:30px}
  .lt-1{grid-template:1fr/1fr}
  .lt-2{grid-template:1fr 1fr/1fr}
  .lt-4{grid-template:1fr 1fr/1fr 1fr}
  .lt-cell{background:var(--border);border-radius:2px}
  .lt-empty{background:transparent;border:1px dashed var(--border)}
  .layout-opt input:checked + .layout-thumb .lt-cell:not(.lt-empty){background:var(--accent)}

  .slot-card{transition:opacity .3s}
  .slot-card.dimmed{opacity:0.45}
  .slot-card.dimmed .card{border-style:dashed}

  @media(max-width:480px){
    .row{flex-direction:column;gap:0}
    .header{padding:14px 16px}
    .header h1{font-size:1em}
    .layout-picker{flex-wrap:wrap}
  }
  #mesh{position:fixed;top:0;left:0;width:100%;height:100%;z-index:0;pointer-events:none}
  body>*:not(#mesh){position:relative;z-index:1}
</style>
</head>
<body>
<canvas id="mesh"></canvas>

<div class="header">
  <h1><span>&#9632;</span> EPD Chart</h1>
  <div class="status-pills">
    <div class="pill live" id="pillWifi">WiFi</div>
    <div class="pill" id="pillIp">--</div>
    <div class="pill" id="pillCrypto">Crypto --</div>
    <div class="pill" id="pillUptime">--</div>
    <div class="pill" id="pillMem">Mem --</div>
    <div class="pill" id="pillNext" style="display:none">--</div>
    <div class="pill warn" id="pillQuiet" style="display:none">Quiet</div>
  </div>
</div>

<div class="container">

  <!-- DISPLAY PREVIEW -->
  <div class="card">
    <div class="card-head"><span class="icon">&#9635;</span><h2>Display Preview</h2></div>
    <div class="card-body" style="text-align:center">
      <img id="displayPreview" src="/api/display" alt="Display preview"
           style="width:100%;max-width:800px;border:1px solid var(--border);border-radius:6px;image-rendering:pixelated;background:#fff">
      <div style="margin-top:8px;font-size:0.72em;color:var(--text-dim);font-family:'JetBrains Mono',monospace">
        Auto-refreshes every 30s &bull; <a href="/api/display" target="_blank" style="color:var(--accent);text-decoration:none">Open full size</a>
      </div>
      <div style="margin-top:4px;font-size:0.7em;color:var(--text-dim);font-family:'JetBrains Mono',monospace">
        Trend blocks: left=down, middle=level, right=up &bull; pips show confidence (1-3)
      </div>
    </div>
  </div>

  <!-- LAYOUT SELECTOR -->
  <div class="card">
    <div class="card-head"><span class="icon">&#9638;</span><h2>Layout</h2></div>
    <div class="card-body">
      <div class="layout-picker">
        <label class="layout-opt"><input type="radio" name="layout" value="1" checked onchange="onLayoutChange()">
          <div class="layout-thumb"><div class="lt-grid lt-1"><div class="lt-cell"></div></div><span>1</span></div></label>
        <label class="layout-opt"><input type="radio" name="layout" value="2" onchange="onLayoutChange()">
          <div class="layout-thumb"><div class="lt-grid lt-2"><div class="lt-cell"></div><div class="lt-cell"></div></div><span>2</span></div></label>
        <label class="layout-opt"><input type="radio" name="layout" value="3" onchange="onLayoutChange()">
          <div class="layout-thumb"><div class="lt-grid lt-4"><div class="lt-cell"></div><div class="lt-cell"></div><div class="lt-cell"></div><div class="lt-cell lt-empty"></div></div><span>3</span></div></label>
        <label class="layout-opt"><input type="radio" name="layout" value="4" onchange="onLayoutChange()">
          <div class="layout-thumb"><div class="lt-grid lt-4"><div class="lt-cell"></div><div class="lt-cell"></div><div class="lt-cell"></div><div class="lt-cell"></div></div><span>4</span></div></label>
      </div>
      <div class="toggle-row" style="margin-top:14px">
        <span class="label">Auto refresh interval</span>
        <label class="toggle"><input type="checkbox" id="autoRefresh" checked onchange="onAutoRefreshChange()"><span class="slider"></span></label>
      </div>
      <div id="autoRefreshInfo" class="auto-info" style="margin-top:8px">Auto: 5m (from shortest active interval)</div>
      <div class="field" id="manualRefreshField" style="margin-top:8px;display:none">
        <label>Refresh every (minutes)</label>
        <input type="number" id="refreshMin" min="1" max="1440" value="5">
      </div>
    </div>
  </div>

  <!-- CHART SLOTS (all 4 always visible) -->
  <div id="slotContainer"></div>

  <!-- DISPLAY -->
  <div class="card">
    <div class="card-head"><span class="icon">&#9635;</span><h2>Display</h2></div>
    <div class="card-body">
      <div class="row">
        <div class="field">
          <label>Full refresh every N cycles</label>
          <input type="number" id="fullRefEvery" min="1" max="50" value="10">
        </div>
        <div class="field">
          <label>Partial threshold %</label>
          <input type="number" id="partialPct" min="10" max="100" value="40">
        </div>
      </div>
      <div class="row" style="margin-top:10px">
        <div class="field">
          <label>Personality HUD</label>
          <label class="toggle"><input type="checkbox" id="personalityEnabled" checked><span class="slider"></span></label>
        </div>
        <div class="field">
          <label>Caption verbosity</label>
          <select id="captionVerbosity"><option value="0">Minimal</option><option value="1" selected>Short</option><option value="2">Detailed</option></select>
        </div>
      </div>
      <div class="field">
        <label>Timezone offset (hours from UTC)</label>
        <select id="tzOffset">
          <option value="-43200">UTC-12</option><option value="-39600">UTC-11</option>
          <option value="-36000">UTC-10</option><option value="-32400">UTC-9</option>
          <option value="-28800">UTC-8 (PST)</option><option value="-25200">UTC-7 (MST)</option>
          <option value="-21600">UTC-6 (CST)</option><option value="-18000">UTC-5 (EST)</option>
          <option value="-14400">UTC-4</option><option value="-10800">UTC-3</option>
          <option value="-7200">UTC-2</option><option value="-3600">UTC-1</option>
          <option value="0" selected>UTC+0 (GMT)</option>
          <option value="3600">UTC+1 (BST/CET)</option><option value="7200">UTC+2</option>
          <option value="10800">UTC+3</option><option value="14400">UTC+4</option>
          <option value="18000">UTC+5</option><option value="19800">UTC+5:30 (IST)</option>
          <option value="21600">UTC+6</option><option value="25200">UTC+7</option>
          <option value="28800">UTC+8</option><option value="32400">UTC+9 (JST)</option>
          <option value="36000">UTC+10</option><option value="39600">UTC+11</option>
          <option value="43200">UTC+12</option>
        </select>
      </div>
      <div class="toggle-row" style="margin-top:10px">
        <span class="label">Quiet hours (skip refresh)</span>
        <label class="toggle"><input type="checkbox" id="quietEnabled"><span class="slider"></span></label>
      </div>
      <div class="row" id="quietHoursRow" style="margin-top:8px;display:none">
        <div class="field">
          <label>Sleep from hour (0-23)</label>
          <input type="number" id="quietStart" min="0" max="23" value="23">
        </div>
        <div class="field">
          <label>Wake at hour (0-23)</label>
          <input type="number" id="quietEnd" min="0" max="23" value="7">
        </div>
      </div>
      <div class="hint" id="quietHint" style="display:none">Display refresh is skipped during this window. Uses device local time. Web UI stays available.</div>
    </div>
  </div>

  <!-- WIFI -->
  <div class="card">
    <div class="card-head"><span class="icon">&#8226;</span><h2>WiFi</h2></div>
    <div class="card-body">
      <div class="field">
        <label>SSID</label>
        <input type="text" id="ssid" placeholder="Network name">
      </div>
      <div class="field">
        <label>Password</label>
        <input type="password" id="wifipass" placeholder="Network password">
        <div class="hint">WiFi changes take effect after reboot</div>
      </div>
      <div class="row">
        <div class="field">
          <label>UI Username (optional)</label>
          <input type="text" id="uiUser" placeholder="admin">
        </div>
        <div class="field">
          <label>UI Password (optional)</label>
          <input type="password" id="uiPass" placeholder="Set to enable API auth">
        </div>
      </div>
    </div>
  </div>

  <!-- CONFIG BACKUP -->
  <div class="card">
    <div class="card-head"><span class="icon">&#128190;</span><h2>Config Backup</h2></div>
    <div class="card-body" style="display:flex;gap:10px;flex-wrap:wrap;align-items:center">
      <button class="btn" onclick="exportConfig()">&#8681; Export Config</button>
      <label class="btn" style="cursor:pointer">
        &#8679; Import Config
        <input type="file" id="importFile" accept=".json" style="display:none" onchange="importConfig(this)">
      </label>
      <div class="hint" style="width:100%;margin-top:4px">Export saves all settings as a JSON file. Import restores from a previously exported file.</div>
    </div>
  </div>

  <!-- FIRMWARE UPDATE -->
  <div class="card">
    <div class="card-head"><span class="icon">&#8679;</span><h2>Firmware Update</h2></div>
    <div class="card-body">
      <div class="field">
        <label>Safety unlock</label>
        <input type="range" id="fwUnlock" min="0" max="100" value="0"
               oninput="onFirmwareUnlockSlide(this.value)"
               onmouseup="onFirmwareUnlockRelease()" ontouchend="onFirmwareUnlockRelease()"
               style="width:100%">
        <div class="hint" id="fwUnlockHint">Slide all the way right to unlock firmware update and arm on-device update screen</div>
      </div>
      <div class="field">
        <label>Upload .bin file</label>
        <input type="file" id="fwFile" accept=".bin" disabled
               style="width:100%;padding:9px 12px;background:var(--bg);border:1px solid var(--border);border-radius:6px;color:var(--text-bright);font-family:'JetBrains Mono',monospace;font-size:0.85em;opacity:.65;cursor:not-allowed">
        <div class="hint">Arduino IDE: Sketch &rarr; Export Compiled Binary, then upload the .bin file here</div>
      </div>
      <div id="fwProgress" style="display:none;margin-top:10px">
        <div style="background:var(--bg);border:1px solid var(--border);border-radius:6px;height:28px;overflow:hidden">
          <div id="fwBar" style="height:100%;width:0%;background:var(--accent-dim);border-right:2px solid var(--accent);transition:width .2s;display:flex;align-items:center;justify-content:center">
            <span id="fwPct" style="font-family:'JetBrains Mono',monospace;font-size:0.75em;color:var(--accent)">0%</span>
          </div>
        </div>
      </div>
      <button id="fwUploadBtn" class="btn" onclick="uploadFirmware()" disabled style="margin-top:10px;width:100%;justify-content:center;opacity:.65;cursor:not-allowed">
        &#8679; Upload &amp; Install
      </button>
      <hr style="border:0;border-top:1px solid var(--border);margin:16px 0">
      <div class="field">
        <label>Remote OTA Polling</label>
        <label class="toggle"><input type="checkbox" id="remoteOtaEnabled"><span class="slider"></span></label>
      </div>
      <div class="field">
        <label>Manifest URL</label>
        <input type="text" id="remoteOtaManifestUrl" placeholder="https://updates.example.com/manifest.json">
      </div>
      <div class="row">
        <div class="field">
          <label>Check interval (min)</label>
          <input type="number" id="remoteOtaCheckMin" min="5" max="1440" value="60">
        </div>
        <div class="field">
          <label>Channel</label>
          <select id="remoteOtaChannel"><option value="stable">stable</option><option value="beta">beta</option></select>
        </div>
      </div>
      <div class="row">
        <div class="field">
          <label>Auto apply</label>
          <label class="toggle"><input type="checkbox" id="remoteOtaAutoApply"><span class="slider"></span></label>
        </div>
        <div class="field">
          <label>Allow downgrade</label>
          <label class="toggle"><input type="checkbox" id="remoteOtaAllowDowngrade"><span class="slider"></span></label>
        </div>
      </div>
      <div class="hint">HTTPS strongly recommended. Leave auto-apply off for notify-only mode.</div>
    </div>
  </div>

</div>

<div class="actions">
  <button class="btn primary" onclick="saveConfig()">&#10003; Save</button>
  <button class="btn" onclick="refreshNow()">&#8635; Refresh Display</button>
  <button class="btn danger" onclick="rebootDevice()">&#9211; Reboot</button>
</div>

<div class="toast" id="toast"></div>

<script>
const AUTO_MAP = {
  '1m':120,'3m':80,'5m':60,'15m':48,'30m':48,
  '1h':48,'2h':42,'4h':42,'8h':30,'12h':30,
  '1d':30,'3d':20,'1w':26,'1M':12
};
const IV_MINS = {
  '1m':1,'3m':3,'5m':5,'15m':15,'30m':30,'1h':60,'2h':120,'4h':240,
  '8h':480,'12h':720,'1d':1440,'3d':4320,'1w':10080,'1M':43200
};
const EX_INTERVALS = {
  hyperliquid:['1m','3m','5m','15m','30m','1h','2h','4h','8h','12h','1d','3d','1w','1M'],
  binance:['1m','3m','5m','15m','30m','1h','2h','4h','6h','8h','12h','1d','3d','1w','1M'],
  asterdex:['1m','3m','5m','15m','30m','1h','2h','4h','6h','8h','12h','1d','3d','1w','1M'],
  kraken:['1m','5m','15m','30m','1h','4h','1d','1w'],
  poloniex:['1m','5m','15m','30m','1h','2h','4h','6h','12h','1d','3d','1w','1M'],
  okx:['1m','3m','5m','15m','30m','1h','2h','4h','6h','12h','1d','1w','1M']
};
const QUOTE_DEFAULTS = {
  hyperliquid:['USDC'],binance:['USDT','BUSD','USDC'],asterdex:['USDT'],
  kraken:['USD','USDT','EUR'],poloniex:['USDT','USDC'],okx:['USDT','USDC']
};
const MAX_SLOTS = 4;

let coinCache = {};
let slotCoinList = [[],[],[],[]];
let slotHlIdx = [-1,-1,-1,-1];
let authHeader = '';
let currentLayout = 1;

function setAuthHeader() {
  const u = (document.getElementById('uiUser').value || '').trim();
  const p = document.getElementById('uiPass').value || '';
  authHeader = (u && p) ? ('Basic ' + btoa(u + ':' + p)) : '';
}

function authFetch(url, opts) {
  opts = opts || {};
  opts.headers = opts.headers || {};
  if (authHeader) opts.headers['Authorization'] = authHeader;
  return fetch(url, opts);
}

function el(id) { return document.getElementById(id); }

// ── Generate slot card HTML ──
function buildSlotCards() {
  let html = '';
  for (let i = 0; i < MAX_SLOTS; i++) {
    html += '<div class="slot-card" id="slotCard_'+i+'">' +
      '<div class="card"><div class="card-head"><span class="icon">&#9670;</span><h2>Chart '+(i+1)+'</h2>' +
      '<span id="slotStatus_'+i+'" style="margin-left:auto;font-size:0.7em;font-family:\'JetBrains Mono\',monospace;padding:2px 8px;border-radius:10px;background:var(--surface2);color:var(--text-dim)">--</span></div>' +
      '<div class="card-body">' +
      '<div class="field"><label>Exchange</label>' +
      '<select id="exchange_'+i+'">' +
      '<option value="hyperliquid">Hyperliquid</option>' +
      '<option value="binance">Binance</option>' +
      '<option value="asterdex">AsterDEX</option>' +
      '<option value="kraken">Kraken</option>' +
      '<option value="poloniex">Poloniex</option>' +
      '<option value="okx">OKX</option></select></div>' +
      '<div class="row">' +
      '<div class="field"><label>Coin</label><div class="coin-wrap">' +
      '<input type="text" id="coinSearch_'+i+'" placeholder="Search coins..." autocomplete="off">' +
      '<input type="hidden" id="coin_'+i+'">' +
      '<div class="coin-dd" id="coinDd_'+i+'"></div></div></div>' +
      '<div class="field"><label>Quote</label><select id="quote_'+i+'"></select></div>' +
      '<div class="field"><label>Interval</label><select id="interval_'+i+'"></select></div></div>' +
      '<div class="toggle-row"><span class="label">Auto candle count</span>' +
      '<label class="toggle"><input type="checkbox" id="autoCandles_'+i+'" checked><span class="slider"></span></label></div>' +
      '<div id="autoInfo_'+i+'" class="auto-info"></div>' +
      '<div class="toggle-row" style="margin-top:8px"><span class="label">Heikin Ashi</span>' +
      '<label class="toggle"><input type="checkbox" id="heikinAshi_'+i+'"><span class="slider"></span></label></div>' +
      '<div class=\"toggle-row\" style=\"margin-top:8px\"><span class=\"label\">Event callouts</span>' +
      '<label class=\"toggle\"><input type=\"checkbox\" id=\"eventCallouts_'+i+'\" checked><span class=\"slider\"></span></label></div>' +
      '<div class="field" id="manualCandleField_'+i+'" style="margin-top:10px;display:none">' +
      '<label>Candles</label><input type="number" id="numCandles_'+i+'" min="5" max="200" value="60"></div>' +
      '<div class="row" style="margin-top:10px">' +
      '<div class="field"><label>EMA fast</label><input type="number" id="emaFast_'+i+'" min="2" max="100" value="9"></div>' +
      '<div class="field"><label>EMA slow</label><input type="number" id="emaSlow_'+i+'" min="2" max="200" value="21"></div>' +
      '<div class="field"><label>RSI</label><input type="number" id="rsiPeriod_'+i+'" min="2" max="100" value="14"></div></div>' +
      '</div></div></div>';
  }
  el('slotContainer').innerHTML = html;

  // Wire up events for each slot
  for (let i = 0; i < MAX_SLOTS; i++) {
    (function(idx) {
      el('exchange_'+idx).addEventListener('change', async function() {
        const ex = this.value;
        rebuildIntervals(idx, ex, el('interval_'+idx).value);
        rebuildQuoteOptions(idx, ex, el('quote_'+idx).value);
        await fetchPairsForSlot(idx, ex);
        el('coin_'+idx).value = '';
        el('coinSearch_'+idx).value = '';
        renderSlotCoinDd(idx, '');
      });
      el('quote_'+idx).addEventListener('change', async function() {
        await fetchPairsForSlot(idx, el('exchange_'+idx).value);
        el('coin_'+idx).value = '';
        el('coinSearch_'+idx).value = '';
        renderSlotCoinDd(idx, '');
      });
      el('interval_'+idx).addEventListener('change', function(){ updateSlotAutoInfo(idx); updateAutoRefreshInfo(); });
      el('autoCandles_'+idx).addEventListener('change', function(){ updateSlotAutoInfo(idx); });
      el('numCandles_'+idx).addEventListener('input', function(){ updateSlotAutoInfo(idx); });

      var csEl = el('coinSearch_'+idx);
      var ddEl = el('coinDd_'+idx);
      csEl.addEventListener('focus', function(){ renderSlotCoinDd(idx, csEl.value); });
      csEl.addEventListener('input', function(){ renderSlotCoinDd(idx, csEl.value); });
      csEl.addEventListener('keydown', function(e) {
        var items = ddEl.querySelectorAll('.coin-item');
        if (e.key==='ArrowDown') { e.preventDefault(); slotHlIdx[idx]=Math.min(slotHlIdx[idx]+1,items.length-1); items.forEach(function(el,j){el.classList.toggle('hl',j===slotHlIdx[idx]);}); }
        else if (e.key==='ArrowUp') { e.preventDefault(); slotHlIdx[idx]=Math.max(slotHlIdx[idx]-1,0); items.forEach(function(el,j){el.classList.toggle('hl',j===slotHlIdx[idx]);}); }
        else if (e.key==='Enter') { e.preventDefault(); if (slotHlIdx[idx]>=0&&slotHlIdx[idx]<items.length) selectSlotCoin(idx,items[slotHlIdx[idx]].dataset.coin); else if(csEl.value.trim()) selectSlotCoin(idx,csEl.value.trim().toUpperCase()); }
        else if (e.key==='Escape') ddEl.classList.remove('open');
      });
      ddEl.addEventListener('click', function(e) {
        var item = e.target.closest('.coin-item');
        if (item) selectSlotCoin(idx, item.dataset.coin);
      });
    })(i);
  }

  document.addEventListener('click', function(e) {
    if (!e.target.closest('.coin-wrap')) {
      for (var j=0;j<MAX_SLOTS;j++) el('coinDd_'+j).classList.remove('open');
    }
  });
}

function rebuildQuoteOptions(idx, ex, currentQuote) {
  var sel = el('quote_'+idx);
  var qs = QUOTE_DEFAULTS[ex] || ['USDT'];
  sel.innerHTML = '';
  qs.forEach(function(v){ var o=document.createElement('option'); o.value=v; o.textContent=v; sel.appendChild(o); });
  sel.value = qs.indexOf(currentQuote) >= 0 ? currentQuote : qs[0];
}

function rebuildIntervals(idx, ex, currentIv) {
  var sel = el('interval_'+idx);
  var ivs = EX_INTERVALS[ex] || EX_INTERVALS.hyperliquid;
  sel.innerHTML = '';
  ivs.forEach(function(v){ var o=document.createElement('option'); o.value=v; o.textContent=v; sel.appendChild(o); });
  if (ivs.indexOf(currentIv)>=0) sel.value=currentIv; else sel.value='1h';
  updateSlotAutoInfo(idx);
}

function fmtSpan(iv, n) {
  var totalMin = (IV_MINS[iv]||5) * n;
  if (totalMin < 60) return totalMin + 'm';
  if (totalMin < 1440) return (totalMin/60).toFixed(1).replace(/\.0$/,'') + 'h';
  return (totalMin/1440).toFixed(1).replace(/\.0$/,'') + 'd';
}

function updateSlotAutoInfo(idx) {
  var ivEl = el('interval_'+idx);
  if (!ivEl) return;
  var iv = ivEl.value;
  if (!iv) return;
  var isAuto = el('autoCandles_'+idx).checked;
  var n = isAuto ? (AUTO_MAP[iv]||60) : parseInt(el('numCandles_'+idx).value)||60;
  var span = fmtSpan(iv, n);
  el('autoInfo_'+idx).innerHTML = '&#x25B6; '+n+' candles &times; '+iv+' = <strong>'+span+'</strong>';
  el('manualCandleField_'+idx).style.display = isAuto ? 'none' : 'block';
  if (isAuto) el('numCandles_'+idx).value = n;
}

async function fetchPairsForSlot(idx, ex) {
  var quote = (el('quote_'+idx).value || 'USDT').toUpperCase();
  var cacheKey = ex + ':' + quote;
  if (coinCache[cacheKey]) { slotCoinList[idx] = coinCache[cacheKey]; return; }
  var dd = el('coinDd_'+idx);
  dd.innerHTML = '<div class="coin-loading">Loading pairs...</div>';
  dd.classList.add('open');
  try {
    var list = [];
    if (ex === 'hyperliquid') {
      var r = await fetch('https://api.hyperliquid.xyz/info', {method:'POST', headers:{'Content-Type':'application/json'}, body:'{"type":"meta"}'});
      var d = await r.json();
      list = (d.universe||[]).filter(function(x){return (x.quoteToken||'USDC')===quote;}).map(function(x){return x.name;});
    } else if (ex === 'binance') {
      var r = await fetch('https://api.binance.com/api/v3/exchangeInfo');
      var d = await r.json();
      list = (d.symbols||[]).filter(function(s){return s.quoteAsset===quote&&s.status==='TRADING';}).map(function(s){return s.baseAsset;});
    } else if (ex === 'asterdex') {
      var r = await fetch('https://fapi.asterdex.com/fapi/v3/exchangeInfo');
      var d = await r.json();
      list = (d.symbols||[]).filter(function(s){return s.quoteAsset===quote&&s.status==='TRADING';}).map(function(s){return s.baseAsset;});
    } else if (ex === 'kraken') {
      var r = await fetch('https://api.kraken.com/0/public/AssetPairs');
      var d = await r.json();
      var pairs=d.result||{},seen={};
      Object.keys(pairs).forEach(function(k){ var p=pairs[k]; var q=(p.quote||'').replace(/^Z/,''); if(q===quote){var b=(p.base||'').replace(/^X/,'');if(!seen[b]){seen[b]=1;list.push(b);}}});
    } else if (ex === 'poloniex') {
      var r = await authFetch('/api/poloniex/markets');
      var d = await r.json();
      list = (d||[]).filter(function(m){return m.symbol&&m.symbol.endsWith('_'+quote);}).map(function(m){return m.symbol.split('_')[0];});
    } else if (ex === 'okx') {
      var r = await fetch('https://www.okx.com/api/v5/public/instruments?instType=SPOT');
      var d = await r.json();
      list = (d.data||[]).filter(function(s){return s.quoteCcy===quote&&s.state==='live';}).map(function(s){return s.baseCcy;});
    }
    list.sort();
    list = list.filter(function(v,i,a){return i===0||v!==a[i-1];});
    coinCache[cacheKey] = list;
    slotCoinList[idx] = list;
  } catch(e) { console.error('fetchPairs slot '+idx+':', e); slotCoinList[idx] = []; }
  dd.classList.remove('open');
}

function renderSlotCoinDd(idx, filter) {
  var dd = el('coinDd_'+idx);
  var q = (filter||'').toUpperCase();
  var cl = slotCoinList[idx] || [];
  var matches = q ? cl.filter(function(c){return c.toUpperCase().indexOf(q)>=0;}) : cl;
  var show = matches.slice(0, 50);
  slotHlIdx[idx] = -1;
  if (!show.length) dd.innerHTML = '<div class="coin-empty">'+(cl.length?'No matches':'No pairs loaded')+'</div>';
  else dd.innerHTML = show.map(function(c){return '<div class="coin-item" data-coin="'+c+'">'+c+'</div>';}).join('');
  dd.classList.add('open');
}

function selectSlotCoin(idx, name) {
  el('coin_'+idx).value = name;
  el('coinSearch_'+idx).value = name;
  el('coinDd_'+idx).classList.remove('open');
}

function onLayoutChange() {
  var radios = document.querySelectorAll('input[name="layout"]');
  for (var i=0;i<radios.length;i++) { if (radios[i].checked) currentLayout = parseInt(radios[i].value); }
  updateSlotDimming();
  updateAutoRefreshInfo();
}

function computeAutoRefreshMin() {
  var min = Infinity;
  for (var i = 0; i < currentLayout; i++) {
    var ivEl = el('interval_'+i);
    if (ivEl) {
      var m = IV_MINS[ivEl.value] || 5;
      if (m < min) min = m;
    }
  }
  return (min === Infinity) ? 5 : Math.max(1, min);
}

function updateAutoRefreshInfo() {
  var autoOn = el('autoRefresh') && el('autoRefresh').checked;
  if (!el('autoRefreshInfo')) return;
  if (autoOn) {
    var m = computeAutoRefreshMin();
    var label = m < 60 ? m+'m' : (m/60).toFixed(1).replace(/\.0$/,'')+'h';
    el('autoRefreshInfo').textContent = 'Auto: ' + label + ' (from shortest active interval)';
    el('autoRefreshInfo').style.display = 'block';
    el('manualRefreshField').style.display = 'none';
  } else {
    el('autoRefreshInfo').style.display = 'none';
    el('manualRefreshField').style.display = 'block';
  }
}

function onAutoRefreshChange() { updateAutoRefreshInfo(); }

function updateSlotDimming() {
  for (var i=0;i<MAX_SLOTS;i++) {
    var card = el('slotCard_'+i);
    if (card) { if (i < currentLayout) card.classList.remove('dimmed'); else card.classList.add('dimmed'); }
  }
}

function toast(msg, err) {
  var t = el('toast');
  t.textContent = msg;
  t.className = 'toast' + (err ? ' error' : '') + ' show';
  setTimeout(function(){t.className='toast';}, 2500);
}

function getActiveSlotSummary(slots, layout) {
  var active = Math.max(1, Math.min(MAX_SLOTS, parseInt(layout || 1)));
  var up = 0, down = 0, flat = 0, tracked = 0;
  for (var i = 0; i < active; i++) {
    var slot = slots[i] || {};
    var pct = Number(slot.pctChange);
    if (!isFinite(pct)) continue;
    tracked++;
    if (pct > 0.05) up++;
    else if (pct < -0.05) down++;
    else flat++;
  }
  return {active:active, tracked:tracked, up:up, down:down, flat:flat};
}

function setCryptoPill(slots, layout, mood) {
  var summary = getActiveSlotSummary(slots || [], layout);
  var text = 'Crypto ' + summary.tracked + '/' + summary.active + ' • ▲' + summary.up + ' ▼' + summary.down;
  if (summary.flat > 0) text += ' ▬' + summary.flat;
  if (mood && mood.caption) text += ' • ' + mood.caption;
  var cls = 'pill';
  if (summary.tracked === 0) cls += ' warn';
  else if (summary.down > summary.up) cls += ' neg';
  else if (summary.up > summary.down) cls += ' live';
  else cls += ' neutral';
  el('pillCrypto').textContent = text;
  el('pillCrypto').className = cls;
}

function setMemoryPill(heapBytes) {
  var kb = Math.round((Number(heapBytes) || 0) / 1024);
  var cls = 'pill';
  if (kb > 0 && kb < 120) cls += ' warn';
  else if (kb >= 120) cls += ' live';
  el('pillMem').textContent = kb > 0 ? ('Mem ' + kb + 'KB') : 'Mem --';
  el('pillMem').className = cls;
}

let configLoaded = false;
async function loadConfig() {
  try {
    var r = await authFetch('/api/status');
    var d = await r.json();

    // Layout
    currentLayout = d.layout || 1;
    var radios = document.querySelectorAll('input[name="layout"]');
    for (var i=0;i<radios.length;i++) radios[i].checked = (parseInt(radios[i].value) === currentLayout);
    updateSlotDimming();

    // Global settings
    el('refreshMin').value = d.refreshMin || 5;
    el('autoRefresh').checked = d.autoRefresh !== false;
    updateAutoRefreshInfo();
    el('tzOffset').value = String(d.tzOffset || 0);
    el('fullRefEvery').value = d.fullRefEvery || 10;
    el('partialPct').value = d.partialPct || 40;
    el('personalityEnabled').checked = d.personalityEnabled !== false;
    el('captionVerbosity').value = String((d.captionVerbosity===0||d.captionVerbosity===2)?d.captionVerbosity:1);
    el('ssid').value = d.ssid || '';
    el('uiUser').value = d.uiUser || '';
    el('uiPass').value = '';
    el('remoteOtaEnabled').checked = !!d.remoteOtaEnabled;
    el('remoteOtaManifestUrl').value = d.remoteOtaManifestUrl || '';
    el('remoteOtaCheckMin').value = d.remoteOtaCheckMin || 60;
    el('remoteOtaChannel').value = (d.remoteOtaChannel === 'beta') ? 'beta' : 'stable';
    el('remoteOtaAutoApply').checked = !!d.remoteOtaAutoApply;
    el('remoteOtaAllowDowngrade').checked = !!d.remoteOtaAllowDowngrade;
    var qe = !!d.quietEnabled;
    el('quietEnabled').checked = qe;
    el('quietStart').value = d.quietStart != null ? d.quietStart : 23;
    el('quietEnd').value = d.quietEnd != null ? d.quietEnd : 7;
    el('quietHoursRow').style.display = qe ? 'flex' : 'none';
    el('quietHint').style.display = qe ? 'block' : 'none';
    var pq = el('pillQuiet');
    if (d.inQuietHours) { pq.style.display=''; pq.textContent='Quiet'; }
    else { pq.style.display='none'; }
    if (typeof d.nextRefreshMs === 'number') startCountdown(d.nextRefreshMs);
    setAuthHeader();

    // Per-slot config
    var slots = d.slots || [];
    for (var i=0;i<MAX_SLOTS;i++) {
      var s = slots[i] || {};
      var ex = s.exchange || 'hyperliquid';
      el('exchange_'+i).value = ex;
      rebuildIntervals(i, ex, s.interval || '5m');
      rebuildQuoteOptions(i, ex, s.quote || 'USDT');
      el('coin_'+i).value = s.coin || '';
      el('coinSearch_'+i).value = s.coin || '';
      el('quote_'+i).value = s.quote || 'USDT';
      el('autoCandles_'+i).checked = s.autoCandles !== false;
      el('numCandles_'+i).value = s.numCandles || 60;
      el('emaFast_'+i).value = s.emaFast || 9;
      el('emaSlow_'+i).value = s.emaSlow || 21;
      el('rsiPeriod_'+i).value = s.rsiPeriod || 14;
      el('heikinAshi_'+i).checked = s.heikinAshi || false;
      el('eventCallouts_'+i).checked = s.eventCallouts !== false;
      updateSlotAutoInfo(i);
      // Per-slot fetch status badge
      var badge = el('slotStatus_'+i);
      if (badge) {
        var age = (s.fetchAge != null) ? s.fetchAge : -1;
        if (age < 0) {
          badge.textContent='--'; badge.style.color='var(--text-dim)'; badge.style.background='var(--surface2)';
        } else if (s.fetchOk) {
          badge.textContent='OK '+(age<60?age+'s':Math.floor(age/60)+'m')+' ago';
          badge.style.color='var(--green)'; badge.style.background='var(--green-dim)';
        } else {
          badge.textContent='FAIL';
          badge.style.color='var(--red)'; badge.style.background='var(--red-dim)';
        }
      }
    }

    // Refresh the auto-refresh label now that slot intervals are populated
    updateAutoRefreshInfo();

    // Status pills
    el('pillIp').textContent = d.apMode ? ('AP: ' + d.apIP) : d.ip;
    if (d.apMode) { el('pillWifi').textContent='AP Mode'; el('pillWifi').className='pill warn'; }
    else if (d.rssi) { el('pillWifi').textContent='WiFi '+d.rssi+'dBm'; el('pillWifi').className='pill live'; }

    // Active panel crypto + runtime + memory pills
    setCryptoPill(slots, d.layout, d.mood || null);

    var uptimeTxt = d.uptime || '--';
    if (d.fails > 0) {
      uptimeTxt += ' | ' + d.fails + ' fails';
      el('pillUptime').className='pill warn';
    } else {
      el('pillUptime').className='pill';
    }
    el('pillUptime').textContent = uptimeTxt;

    setMemoryPill(d.heap);

    if (!configLoaded) {
      configLoaded = true;
      for (var i=0;i<MAX_SLOTS;i++) await fetchPairsForSlot(i, el('exchange_'+i).value);
    }
  } catch(e) { console.error(e); }
}

async function saveConfig() {
  var remoteUrl = el('remoteOtaManifestUrl').value.trim();
  var remoteEnabled = el('remoteOtaEnabled').checked;
  var remoteMin = parseInt(el('remoteOtaCheckMin').value, 10);
  if (remoteEnabled) {
    if (!/^https?:\/\//i.test(remoteUrl) || remoteUrl.length < 10 || remoteUrl.length > 256) {
      toast('Remote OTA manifest URL must be http/https and 10-256 chars', true);
      return;
    }
  }
  if (!(remoteMin >= 5 && remoteMin <= 1440)) {
    toast('Remote OTA check interval must be 5-1440 min', true);
    return;
  }

  var slotsArr = [];
  for (var i=0;i<MAX_SLOTS;i++) {
    slotsArr.push({
      exchange: el('exchange_'+i).value,
      coin: el('coin_'+i).value.toUpperCase().trim(),
      quote: el('quote_'+i).value.toUpperCase().trim(),
      interval: el('interval_'+i).value,
      autoCandles: el('autoCandles_'+i).checked,
      numCandles: parseInt(el('numCandles_'+i).value) || 60,
      emaFast: parseInt(el('emaFast_'+i).value) || 9,
      emaSlow: parseInt(el('emaSlow_'+i).value) || 21,
      rsiPeriod: parseInt(el('rsiPeriod_'+i).value) || 14,
      heikinAshi: el('heikinAshi_'+i).checked,
      eventCallouts: el('eventCallouts_'+i).checked
    });
  }
  var autoRefreshOn = el('autoRefresh').checked;
  var body = {
    layout: currentLayout,
    slots: slotsArr,
    autoRefresh: autoRefreshOn,
    refreshMin: autoRefreshOn ? computeAutoRefreshMin() : (parseInt(el('refreshMin').value) || 5),
    tzOffset: parseInt(el('tzOffset').value) || 0,
    fullRefEvery: parseInt(el('fullRefEvery').value) || 10,
    partialPct: parseInt(el('partialPct').value) || 40,
    personalityEnabled: el('personalityEnabled').checked,
    captionVerbosity: parseInt(el('captionVerbosity').value) || 0,
    ssid: el('ssid').value,
    pass: el('wifipass').value,
    uiUser: el('uiUser').value.trim(),
    uiPass: el('uiPass').value,
    remoteOtaEnabled: remoteEnabled,
    remoteOtaManifestUrl: remoteUrl,
    remoteOtaCheckMin: remoteMin,
    remoteOtaChannel: el('remoteOtaChannel').value,
    remoteOtaAutoApply: el('remoteOtaAutoApply').checked,
    remoteOtaAllowDowngrade: el('remoteOtaAllowDowngrade').checked,
    quietEnabled: el('quietEnabled').checked,
    quietStart: parseInt(el('quietStart').value) || 0,
    quietEnd: parseInt(el('quietEnd').value) || 0
  };
  setAuthHeader();
  try {
    var r = await authFetch('/api/config', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
    if (r.ok) toast('Config saved'); else toast('Save failed', true);
  } catch(e) { toast('Connection error', true); }
}

// ── Quiet hours toggle ──
document.addEventListener('DOMContentLoaded', function() {
  el('quietEnabled').addEventListener('change', function() {
    var on = this.checked;
    el('quietHoursRow').style.display = on ? 'flex' : 'none';
    el('quietHint').style.display = on ? 'block' : 'none';
  });
});

// ── Countdown to next display refresh ──
var countdownTarget = 0;
var countdownTimer = null;
function startCountdown(msFromNow) {
  countdownTarget = Date.now() + msFromNow;
  if (countdownTimer) clearInterval(countdownTimer);
  var pill = el('pillNext');
  pill.style.display = '';
  countdownTimer = setInterval(function() {
    var rem = Math.max(0, countdownTarget - Date.now());
    var m = Math.floor(rem / 60000);
    var s = Math.floor((rem % 60000) / 1000);
    if (rem === 0) { pill.textContent = 'Refreshing...'; pill.className = 'pill neutral'; }
    else { pill.textContent = 'Next: ' + m + ':' + (s < 10 ? '0' : '') + s; pill.className = 'pill'; }
  }, 1000);
}

// ── Config export / import ──
async function exportConfig() {
  try {
    var r = await authFetch('/api/status');
    var d = await r.json();
    var cfg = {
      layout: d.layout, refreshMin: d.refreshMin, autoRefresh: d.autoRefresh, tzOffset: d.tzOffset,
      fullRefEvery: d.fullRefEvery, partialPct: d.partialPct,
      personalityEnabled: d.personalityEnabled, captionVerbosity: d.captionVerbosity,
      quietEnabled: d.quietEnabled, quietStart: d.quietStart, quietEnd: d.quietEnd,
      ssid: d.ssid, uiUser: d.uiUser,
      remoteOtaEnabled: d.remoteOtaEnabled, remoteOtaManifestUrl: d.remoteOtaManifestUrl,
      remoteOtaCheckMin: d.remoteOtaCheckMin, remoteOtaChannel: d.remoteOtaChannel,
      remoteOtaAutoApply: d.remoteOtaAutoApply, remoteOtaAllowDowngrade: d.remoteOtaAllowDowngrade,
      slots: (d.slots || []).map(function(s) { return {
        exchange: s.exchange, coin: s.coin, quote: s.quote, interval: s.interval,
        autoCandles: s.autoCandles, numCandles: s.numCandles,
        emaFast: s.emaFast, emaSlow: s.emaSlow, rsiPeriod: s.rsiPeriod,
        heikinAshi: s.heikinAshi, eventCallouts: s.eventCallouts
      }; })
    };
    var blob = new Blob([JSON.stringify(cfg, null, 2)], {type: 'application/json'});
    var a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'epdchart-config.json';
    a.click();
    toast('Config exported');
  } catch(e) { toast('Export failed', true); }
}
async function importConfig(input) {
  var file = input.files[0];
  if (!file) return;
  var reader = new FileReader();
  reader.onload = async function(ev) {
    try {
      var cfg = JSON.parse(ev.target.result);
      if (!confirm('Apply this config? Current settings will be overwritten.')) { input.value = ''; return; }
      setAuthHeader();
      var r = await authFetch('/api/config', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(cfg)});
      if (r.ok) { toast('Config imported \u2014 reloading...'); setTimeout(loadConfig, 1000); }
      else toast('Import failed: server rejected config', true);
    } catch(ex) { toast('Invalid JSON file', true); }
    input.value = '';
  };
  reader.readAsText(file);
}

async function refreshNow() {
  try { await authFetch('/api/refresh', {method:'POST'}); toast('Display refresh triggered'); } catch(e) { toast('Failed', true); }
}

async function rebootDevice() {
  if (!confirm('Reboot the device?')) return;
  try { await authFetch('/api/restart', {method:'POST'}); toast('Rebooting...'); } catch(e) { toast('Rebooting...'); }
}

function refreshPreview() { el('displayPreview').src = '/api/display?t=' + Date.now(); }
setInterval(refreshPreview, 30000);

let firmwareUnlocked = false;
let firmwareUnlockPending = false;

function setFirmwareUnlocked(unlocked) {
  firmwareUnlocked = !!unlocked;
  var fi = el('fwFile'), btn = el('fwUploadBtn'), hint = el('fwUnlockHint');
  fi.disabled = !firmwareUnlocked; btn.disabled = !firmwareUnlocked;
  fi.style.opacity = btn.style.opacity = firmwareUnlocked ? '1' : '.65';
  fi.style.cursor = btn.style.cursor = firmwareUnlocked ? 'pointer' : 'not-allowed';
  hint.textContent = firmwareUnlocked ? 'Firmware update unlocked.' : 'Slide all the way right to unlock firmware update';
}

async function armFirmwareUpdateScreen() {
  if (firmwareUnlockPending || firmwareUnlocked) return;
  firmwareUnlockPending = true;
  try {
    var r = await authFetch('/api/update/arm', {method:'POST'});
    if (!r.ok) throw new Error('arm failed');
    setFirmwareUnlocked(true); toast('Firmware update unlocked');
  } catch(e) { el('fwUnlock').value=0; setFirmwareUnlocked(false); toast('Unable to unlock update mode', true); }
  finally { firmwareUnlockPending = false; }
}

function onFirmwareUnlockSlide(v) { if (!firmwareUnlocked && Number(v) >= 96) armFirmwareUpdateScreen(); }
function onFirmwareUnlockRelease() { if (!firmwareUnlocked) el('fwUnlock').value = 0; }

function uploadFirmware() {
  var fi = el('fwFile');
  if (!firmwareUnlocked) { toast('Slide to unlock first', true); return; }
  if (!fi.files.length) { toast('Select a .bin file first', true); return; }
  if (!confirm('Upload firmware and reboot?')) return;
  var formData = new FormData(); formData.append('update', fi.files[0]);
  el('fwUnlock').disabled = true;
  var xhr = new XMLHttpRequest();
  var progress = el('fwProgress'), bar = el('fwBar'), pctEl = el('fwPct');
  progress.style.display = 'block';
  var pollTimer = null;
  var setPct = function(p){ var v=Math.max(0,Math.min(100,Number(p)||0)); bar.style.width=v+'%'; pctEl.textContent=v+'%'; };
  var startPoll = function(){ pollTimer=setInterval(async function(){ try{var r=await authFetch('/api/status');if(!r.ok)return;var d=await r.json();if(typeof d.otaProgress==='number')setPct(d.otaProgress);}catch(e){}},500); };
  xhr.onload = function(){ if(pollTimer)clearInterval(pollTimer); if(xhr.status===200){setPct(100);toast('Firmware updated');setTimeout(function(){location.reload();},10000);}else{el('fwUnlock').disabled=false;toast('Update failed',true);} };
  xhr.onerror = function(){ if(pollTimer)clearInterval(pollTimer); toast('Upload error',true); };
  xhr.open('POST','/api/update');
  if (authHeader) xhr.setRequestHeader('Authorization', authHeader);
  startPoll(); xhr.send(formData);
}

// Initialize
buildSlotCards();
setFirmwareUnlocked(false);
loadConfig();
setInterval(loadConfig, 30000);

// ── Crypto Mesh Background ──
(function(){
  var cv=document.getElementById('mesh'),ctx=cv.getContext('2d');
  var W,H,nodes=[],edges=[],mouse={x:-9999,y:-9999};
  var COINS=[
    {c:'#f7931a',d:0},{c:'#627eea',d:1},{c:'#00aae4',d:2},
    {c:'#f0b90b',d:3},{c:'#9945ff',d:4},{c:'#c2a633',d:5},
    {c:'#0033ad',d:6},{c:'#ff0013',d:7},{c:'#2a5ada',d:8},
    {c:'#e84142',d:9}
  ];
  var CONN=200,REST=120,REP_R=100,MOUSE_R=150,DAMP=0.98,MAX_LINKS=3;
  var PI=Math.PI,TAU=PI*2;
  function icon(c,x,y,d){
    var s=7;
    c.save();c.translate(x,y);
    switch(d){
    case 0:// BTC - ₿
      c.lineWidth=1.6;c.strokeStyle=c.fillStyle;
      c.beginPath();c.moveTo(-s*0.4,-s);c.lineTo(-s*0.4,s);c.moveTo(s*0.1,-s);c.lineTo(s*0.1,s);c.stroke();
      c.beginPath();c.moveTo(-s*0.6,-s*0.7);c.lineTo(s*0.1,-s*0.7);
      c.quadraticCurveTo(s*0.8,-s*0.7,s*0.8,-s*0.15);c.quadraticCurveTo(s*0.8,s*0.2,s*0.1,s*0.1);
      c.lineTo(s*0.1,s*0.1);c.quadraticCurveTo(s*0.9,s*0.1,s*0.9,s*0.5);
      c.quadraticCurveTo(s*0.9,s*0.8,s*0.1,s*0.7);c.lineTo(-s*0.6,s*0.7);c.closePath();c.stroke();
      break;
    case 1:// ETH - diamond
      c.beginPath();c.moveTo(0,-s*1.2);c.lineTo(s*0.7,0);c.lineTo(0,s*1.2);c.lineTo(-s*0.7,0);c.closePath();c.fill();
      c.globalAlpha=0.3;c.fillStyle='#000';
      c.beginPath();c.moveTo(0,-s*1.2);c.lineTo(s*0.7,0);c.lineTo(0,s*0.2);c.lineTo(-s*0.7,0);c.closePath();c.fill();
      break;
    case 2:// XRP - X shape with circle
      c.lineWidth=2;c.strokeStyle=c.fillStyle;
      c.beginPath();c.moveTo(-s*0.7,-s*0.7);c.lineTo(s*0.7,s*0.7);c.moveTo(s*0.7,-s*0.7);c.lineTo(-s*0.7,s*0.7);c.stroke();
      c.beginPath();c.arc(0,0,s*0.3,0,TAU);c.fill();
      break;
    case 3:// BNB - diamond
      c.beginPath();c.moveTo(0,-s);c.lineTo(s,0);c.lineTo(0,s);c.lineTo(-s,0);c.closePath();c.fill();
      c.globalAlpha=0.4;c.fillStyle='#000';
      c.beginPath();c.moveTo(0,-s*0.4);c.lineTo(s*0.4,0);c.lineTo(0,s*0.4);c.lineTo(-s*0.4,0);c.closePath();c.fill();
      break;
    case 4:// SOL - three slanted parallel lines
      c.lineWidth=2.2;c.strokeStyle=c.fillStyle;c.lineCap='round';
      c.beginPath();c.moveTo(-s*0.8,-s*0.6);c.lineTo(s*0.8,-s*0.6);c.lineTo(-s*0.8,-s*0.1);c.stroke();
      c.beginPath();c.moveTo(-s*0.8,s*0.15);c.lineTo(s*0.8,s*0.15);c.stroke();
      c.beginPath();c.moveTo(s*0.8,s*0.6);c.lineTo(-s*0.8,s*0.6);c.lineTo(s*0.8,s*0.1);c.stroke();
      break;
    case 5:// DOGE - Ð
      c.lineWidth=1.6;c.strokeStyle=c.fillStyle;
      c.beginPath();c.moveTo(-s*0.3,-s*0.8);c.lineTo(-s*0.3,s*0.8);c.lineTo(0,s*0.8);
      c.quadraticCurveTo(s*0.9,s*0.8,s*0.9,0);c.quadraticCurveTo(s*0.9,-s*0.8,0,-s*0.8);c.closePath();c.stroke();
      c.beginPath();c.moveTo(-s*0.6,0);c.lineTo(s*0.5,0);c.stroke();
      break;
    case 6:// ADA - six-pointed star
      var i,a;c.beginPath();
      for(i=0;i<6;i++){a=i*PI/3-PI/2;c.lineTo(Math.cos(a)*s,Math.sin(a)*s);c.lineTo(Math.cos(a+PI/6)*s*0.5,Math.sin(a+PI/6)*s*0.5)}
      c.closePath();c.fill();
      break;
    case 7:// TRX - triangle
      c.beginPath();c.moveTo(0,-s);c.lineTo(s*0.9,s*0.7);c.lineTo(-s*0.9,s*0.7);c.closePath();c.fill();
      c.globalAlpha=0.3;c.fillStyle='#000';
      c.beginPath();c.moveTo(0,-s*0.3);c.lineTo(s*0.4,s*0.4);c.lineTo(-s*0.4,s*0.4);c.closePath();c.fill();
      break;
    case 8:// LINK - hexagon
      var i,a;c.beginPath();
      for(i=0;i<6;i++){a=i*PI/3;c.lineTo(Math.cos(a)*s,Math.sin(a)*s)}
      c.closePath();c.stroke();c.lineWidth=1.5;c.strokeStyle=c.fillStyle;
      c.beginPath();for(i=0;i<6;i++){a=i*PI/3;c.lineTo(Math.cos(a)*s*0.55,Math.sin(a)*s*0.55)}
      c.closePath();c.fill();
      break;
    case 9:// AVAX - mountain A
      c.beginPath();c.moveTo(0,-s);c.lineTo(s*0.9,s*0.7);c.lineTo(s*0.3,s*0.7);c.lineTo(0,s*0.15);c.lineTo(-s*0.3,s*0.7);c.lineTo(-s*0.9,s*0.7);c.closePath();c.fill();
      break;
    }
    c.restore();
  }

  function hexRgb(h){var v=parseInt(h.slice(1),16);return[(v>>16)&255,(v>>8)&255,v&255]}
  function avgCol(a,b){var A=hexRgb(a),B=hexRgb(b);return 'rgb('+(A[0]+B[0]>>1)+','+(A[1]+B[1]>>1)+','+(A[2]+B[2]>>1)+')'}

  function nodeCount(){return Math.max(25,Math.min(60,Math.floor(W*H/25000)))}

  function makeNode(){
    var c=COINS[Math.floor(Math.random()*COINS.length)];
    return{x:Math.random()*W,y:Math.random()*H,vx:0,vy:0,coin:c}
  }

  function resize(){
    W=cv.width=window.innerWidth;H=cv.height=window.innerHeight;
    var target=nodeCount(),diff=target-nodes.length;
    if(diff>0)for(var i=0;i<diff;i++)nodes.push(makeNode());
    else if(diff<0)nodes.splice(target);
  }

  function tick(){
    var i,j,n,m,dx,dy,d,f,ax,ay,lc=[],cands=[];
    edges=[];
    for(i=0;i<nodes.length;i++){
      n=nodes[i];lc[i]=0;
      // Brownian drift
      n.vx+=(Math.random()-0.5)*0.15;
      n.vy+=(Math.random()-0.5)*0.15;
      // Mouse repulsion
      dx=n.x-mouse.x;dy=n.y-mouse.y;
      d=Math.sqrt(dx*dx+dy*dy);
      if(d<MOUSE_R&&d>0.1){f=0.8*(1-d/MOUSE_R);n.vx+=dx/d*f;n.vy+=dy/d*f}
    }
    // Pairwise: repulsion + collect candidates
    for(i=0;i<nodes.length;i++){
      n=nodes[i];
      for(j=i+1;j<nodes.length;j++){
        m=nodes[j];dx=n.x-m.x;dy=n.y-m.y;
        if(Math.abs(dx)>CONN)continue;
        if(Math.abs(dy)>CONN)continue;
        d=Math.sqrt(dx*dx+dy*dy);
        if(d<0.1)continue;
        // Repulsion (always applies)
        if(d<REP_R){f=3.0/(d*d);ax=dx/d*f;ay=dy/d*f;n.vx+=ax;n.vy+=ay;m.vx-=ax;m.vy-=ay}
        // Candidate edge
        if(d<CONN)cands.push({i:i,j:j,d:d});
      }
    }
    // Sort candidates by distance (shortest first)
    cands.sort(function(a,b){return a.d-b.d});
    // Greedily assign edges to nearest neighbors
    for(i=0;i<cands.length;i++){
      var e=cands[i];
      if(lc[e.i]<MAX_LINKS&&lc[e.j]<MAX_LINKS){
        // Apply spring force
        n=nodes[e.i];m=nodes[e.j];dx=n.x-m.x;dy=n.y-m.y;d=e.d;
        f=0.0003*(d-REST);ax=dx/d*f;ay=dy/d*f;
        n.vx-=ax;n.vy-=ay;m.vx+=ax;m.vy+=ay;
        edges.push(e.i,e.j,d);lc[e.i]++;lc[e.j]++;
      }
    }
    // Integrate
    for(i=0;i<nodes.length;i++){
      n=nodes[i];n.vx*=DAMP;n.vy*=DAMP;n.x+=n.vx;n.y+=n.vy;
      // Wrap
      if(n.x<-30)n.x=W+30;else if(n.x>W+30)n.x=-30;
      if(n.y<-30)n.y=H+30;else if(n.y>H+30)n.y=-30;
    }
  }

  function draw(){
    ctx.clearRect(0,0,W,H);
    var i,n,m,d,a,k;
    // Draw connections from precomputed edges
    ctx.lineWidth=1.2;
    for(k=0;k<edges.length;k+=3){
      n=nodes[edges[k]];m=nodes[edges[k+1]];d=edges[k+2];
      a=(1-d/CONN)*0.25;
      ctx.save();
      ctx.strokeStyle=avgCol(n.coin.c,m.coin.c);
      ctx.globalAlpha=a;
      ctx.shadowColor=ctx.strokeStyle;ctx.shadowBlur=4;
      ctx.beginPath();ctx.moveTo(n.x,n.y);ctx.lineTo(m.x,m.y);ctx.stroke();
      ctx.restore();
    }
    // Draw nodes
    for(i=0;i<nodes.length;i++){
      n=nodes[i];
      // Glow
      ctx.save();
      ctx.globalAlpha=0.12;
      ctx.shadowColor=n.coin.c;ctx.shadowBlur=20;
      ctx.fillStyle=n.coin.c;
      ctx.beginPath();ctx.arc(n.x,n.y,14,0,6.2832);ctx.fill();
      ctx.restore();
      // Icon
      ctx.save();
      ctx.globalAlpha=0.75;
      ctx.fillStyle=n.coin.c;
      ctx.strokeStyle=n.coin.c;
      ctx.shadowColor=n.coin.c;ctx.shadowBlur=8;
      icon(ctx,n.x,n.y,n.coin.d);
      ctx.restore();
    }
  }

  function loop(){tick();draw();requestAnimationFrame(loop)}

  document.addEventListener('mousemove',function(e){mouse.x=e.clientX;mouse.y=e.clientY});
  document.addEventListener('mouseleave',function(){mouse.x=-9999;mouse.y=-9999});
  var resizeTimer;
  window.addEventListener('resize',function(){clearTimeout(resizeTimer);resizeTimer=setTimeout(resize,200)});

  resize();
  requestAnimationFrame(loop);
})();
</script>
</body>
</html>
)rawliteral";

// ═══════════════════════════════════════════════════════
// WEB SERVER HANDLERS
// ═══════════════════════════════════════════════════════

void handleRoot() {
    server.send_P(200, "text/html", HTML_PAGE);
}

void handleStatus() {
    unsigned long uptimeSec = (millis() - bootTime) / 1000;
    int uh = uptimeSec / 3600, um = (uptimeSec % 3600) / 60;
    char uptimeStr[16];
    snprintf(uptimeStr, sizeof(uptimeStr), "%dh %dm", uh, um);

    DynamicJsonDocument doc(4096);

    // Backward-compatible top-level fields (from slot 0)
    doc["exchange"] = slots[0].cfg.exchange;
    doc["coin"] = slots[0].cfg.coin;
    doc["quote"] = slots[0].cfg.quote;
    doc["interval"] = slots[0].cfg.interval;
    doc["autoCandles"] = slots[0].cfg.autoCandles;
    doc["numCandles"] = slots[0].cfg.numCandles;
    doc["emaFast"] = slots[0].cfg.emaFast;
    doc["emaSlow"] = slots[0].cfg.emaSlow;
    doc["rsiPeriod"] = slots[0].cfg.rsiPeriod;
    doc["heikinAshi"] = slots[0].cfg.heikinAshi;
    doc["eventCallouts"] = slots[0].cfg.eventCallouts;
    doc["eventTs"] = slots[0].lastEvent.ts;
    doc["eventType"] = slotEventTypeToStr(slots[0].lastEvent.type);
    doc["eventMessage"] = slots[0].lastEvent.message;

    // Global settings
    doc["layout"] = cfgLayout;
    doc["refreshMin"] = cfgRefreshMin;
    doc["autoRefresh"] = cfgAutoRefresh;
    doc["autoRefreshMin"] = (int)(effectiveRefreshMs() / 60000UL);
    doc["tzOffset"] = cfgTzOffset;
    doc["fullRefEvery"] = cfgFullRefEvery;
    doc["partialPct"] = cfgPartialPct;
    doc["personalityEnabled"] = cfgPersonalityEnabled;
    doc["captionVerbosity"] = cfgCaptionVerbosity;

    bool canViewSensitive = !authEnabled() || server.authenticate(cfgUiUser, cfgUiPass);
    doc["ssid"] = canViewSensitive ? cfgSSID : "";
    doc["uiUser"] = canViewSensitive ? cfgUiUser : "";
    doc["authEnabled"] = authEnabled();
    doc["restricted"] = !canViewSensitive;
    doc["ip"] = WiFi.localIP().toString();
    doc["apMode"] = apModeActive;
    doc["apIP"] = WiFi.softAPIP().toString();
    doc["rssi"] = WiFi.RSSI();
    doc["price"] = slots[0].lastPrice;
    doc["pctChange"] = slots[0].lastPctChange;
    doc["uptime"] = uptimeStr;
    doc["fails"] = consecutiveFails;
    doc["otaActive"] = otaActive;
    doc["otaProgress"] = otaProgressPct;
    doc["otaFailed"] = otaFailed;
    doc["remoteOtaEnabled"] = remoteOtaEnabled;
    doc["remoteOtaManifestUrl"] = remoteOtaManifestUrl;
    doc["remoteOtaCheckMin"] = remoteOtaCheckMin;
    doc["remoteOtaChannel"] = remoteOtaChannel;
    doc["remoteOtaAutoApply"] = remoteOtaAutoApply;
    doc["remoteOtaAllowDowngrade"] = remoteOtaAllowDowngrade;
    doc["quietEnabled"] = cfgQuietEnabled;
    doc["quietStart"]   = cfgQuietStart;
    doc["quietEnd"]     = cfgQuietEnd;
    doc["inQuietHours"] = isInQuietHours();
    long msUntilRefresh = (long)effectiveRefreshMs() - (long)(millis() - lastRefreshMs);
    doc["nextRefreshMs"] = max(0L, msUntilRefresh);
    doc["heap"] = ESP.getFreeHeap();
    doc["fwVersion"] = FW_VERSION;
    doc["gitSha"] = FW_GIT_SHA;
    doc["buildTimestamp"] = FW_BUILD_TIMESTAMP;

    currentMood = getAggregateMood();
    JsonObject mood = doc.createNestedObject("mood");
    mood["id"] = (int)currentMood.id;
    mood["caption"] = currentMood.caption;
    mood["style"] = currentMood.style;
    mood["aggregatePct"] = currentMood.aggregatePct;

    // Per-slot config array
    JsonArray slotsArr = doc.createNestedArray("slots");
    for (int i = 0; i < MAX_SLOTS; i++) {
        JsonObject s = slotsArr.createNestedObject();
        s["exchange"] = slots[i].cfg.exchange;
        s["coin"] = slots[i].cfg.coin;
        s["quote"] = slots[i].cfg.quote;
        s["interval"] = slots[i].cfg.interval;
        s["autoCandles"] = slots[i].cfg.autoCandles;
        s["numCandles"] = slots[i].cfg.numCandles;
        s["emaFast"] = slots[i].cfg.emaFast;
        s["emaSlow"] = slots[i].cfg.emaSlow;
        s["rsiPeriod"] = slots[i].cfg.rsiPeriod;
        s["heikinAshi"] = slots[i].cfg.heikinAshi;
        s["eventCallouts"] = slots[i].cfg.eventCallouts;
        s["price"] = slots[i].lastPrice;
        s["pctChange"] = slots[i].lastPctChange;
        s["eventTs"] = slots[i].lastEvent.ts;
        s["eventType"] = slotEventTypeToStr(slots[i].lastEvent.type);
        s["eventMessage"] = slots[i].lastEvent.message;
        s["fetchOk"] = slots[i].lastFetchOk;
        s["fetchAge"] = (slots[i].lastFetchMs > 0)
            ? (int)((millis() - slots[i].lastFetchMs) / 1000)
            : -1;
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void handleConfigPost() {
    if (!authenticateRequest()) return;
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"no body\"}");
        return;
    }

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }

    // Global settings
    if (doc.containsKey("layout")) {
        int layout = doc["layout"].as<int>();
        if (layout >= 1 && layout <= 4) cfgLayout = layout;
    }
    if (doc.containsKey("autoRefresh")) cfgAutoRefresh = doc["autoRefresh"].as<bool>();
    if (!cfgAutoRefresh && doc.containsKey("refreshMin"))
        cfgRefreshMin = constrain(doc["refreshMin"].as<int>(), 1, 1440);
    if (doc.containsKey("tzOffset"))    cfgTzOffset     = doc["tzOffset"].as<int>();
    if (doc.containsKey("fullRefEvery"))cfgFullRefEvery = constrain(doc["fullRefEvery"].as<int>(), 1, 50);
    if (doc.containsKey("partialPct"))  cfgPartialPct   = constrain(doc["partialPct"].as<int>(), 10, 100);
    if (doc.containsKey("personalityEnabled")) cfgPersonalityEnabled = doc["personalityEnabled"].as<bool>();
    if (doc.containsKey("captionVerbosity")) cfgCaptionVerbosity = constrain(doc["captionVerbosity"].as<int>(), 0, 2);
    if (doc.containsKey("quietEnabled")) cfgQuietEnabled = doc["quietEnabled"].as<bool>();
    if (doc.containsKey("quietStart")) cfgQuietStart = constrain(doc["quietStart"].as<int>(), 0, 23);
    if (doc.containsKey("quietEnd"))   cfgQuietEnd   = constrain(doc["quietEnd"].as<int>(), 0, 23);

    if (doc.containsKey("remoteOtaEnabled")) remoteOtaEnabled = doc["remoteOtaEnabled"].as<bool>();
    if (doc.containsKey("remoteOtaManifestUrl")) {
        const char* url = doc["remoteOtaManifestUrl"].as<const char*>();
        if (url && strlen(url) == 0) {
            remoteOtaManifestUrl[0] = '\0';
        } else if (!isHttpUrl(url)) {
            server.send(400, "application/json", "{\"error\":\"remoteOtaManifestUrl must be http/https and 10-256 chars\"}");
            return;
        } else {
            copyBounded(remoteOtaManifestUrl, sizeof(remoteOtaManifestUrl), url);
        }
    }
    if (doc.containsKey("remoteOtaCheckMin")) {
        int checkMin = doc["remoteOtaCheckMin"].as<int>();
        if (checkMin < 5 || checkMin > 1440) {
            server.send(400, "application/json", "{\"error\":\"remoteOtaCheckMin out of range (5-1440)\"}");
            return;
        }
        remoteOtaCheckMin = checkMin;
    }
    if (doc.containsKey("remoteOtaChannel")) {
        const char* channel = doc["remoteOtaChannel"].as<const char*>();
        if (channel && strlen(channel) > 0) {
            if (!isValidRemoteChannel(channel)) {
                server.send(400, "application/json", "{\"error\":\"remoteOtaChannel must be stable or beta\"}");
                return;
            }
            copyBounded(remoteOtaChannel, sizeof(remoteOtaChannel), channel);
        } else {
            copyBounded(remoteOtaChannel, sizeof(remoteOtaChannel), "stable");
        }
    }
    if (doc.containsKey("remoteOtaAutoApply")) remoteOtaAutoApply = doc["remoteOtaAutoApply"].as<bool>();
    if (doc.containsKey("remoteOtaAllowDowngrade")) remoteOtaAllowDowngrade = doc["remoteOtaAllowDowngrade"].as<bool>();
    if (remoteOtaAutoApply && strncmp(remoteOtaManifestUrl, "https://", 8) != 0) {
        server.send(400, "application/json", "{\"error\":\"remoteOtaAutoApply requires HTTPS manifest URL\"}");
        return;
    }

    if (doc.containsKey("ssid") && strlen(doc["ssid"].as<const char*>()) > 0)
        copyBounded(cfgSSID, sizeof(cfgSSID), doc["ssid"].as<const char*>());
    if (doc.containsKey("pass") && strlen(doc["pass"].as<const char*>()) > 0)
        copyBounded(cfgPass, sizeof(cfgPass), doc["pass"].as<const char*>());
    if (doc.containsKey("uiUser"))
        copyBounded(cfgUiUser, sizeof(cfgUiUser), doc["uiUser"].as<const char*>());
    if (doc.containsKey("uiPass"))
        copyBounded(cfgUiPass, sizeof(cfgUiPass), doc["uiPass"].as<const char*>());

    // Per-slot config from "slots" array
    if (doc.containsKey("slots")) {
        JsonArray slotsArr = doc["slots"].as<JsonArray>();
        for (int i = 0; i < MAX_SLOTS && i < (int)slotsArr.size(); i++) {
            JsonObject s = slotsArr[i].as<JsonObject>();
            SlotConfig& sc = slots[i].cfg;

            char tmpExchange[16], tmpCoin[16], tmpQuote[8], tmpInterval[8];
            copyBounded(tmpExchange, sizeof(tmpExchange), sc.exchange);
            copyBounded(tmpCoin, sizeof(tmpCoin), sc.coin);
            copyBounded(tmpQuote, sizeof(tmpQuote), sc.quote);
            copyBounded(tmpInterval, sizeof(tmpInterval), sc.interval);

            if (s.containsKey("exchange")) copyBounded(tmpExchange, sizeof(tmpExchange), s["exchange"].as<const char*>());
            if (s.containsKey("coin"))     copyBounded(tmpCoin, sizeof(tmpCoin), s["coin"].as<const char*>());
            if (s.containsKey("quote"))    copyBounded(tmpQuote, sizeof(tmpQuote), s["quote"].as<const char*>());
            if (s.containsKey("interval")) copyBounded(tmpInterval, sizeof(tmpInterval), s["interval"].as<const char*>());

            toUpperInPlace(tmpCoin);
            toUpperInPlace(tmpQuote);

            if (isValidExchange(tmpExchange))
                copyBounded(sc.exchange, sizeof(sc.exchange), tmpExchange);
            if (isValidInterval(tmpInterval))
                copyBounded(sc.interval, sizeof(sc.interval), tmpInterval);
            if (isValidSymbolToken(tmpCoin, sizeof(tmpCoin)))
                copyBounded(sc.coin, sizeof(sc.coin), tmpCoin);
            if (isValidSymbolToken(tmpQuote, sizeof(tmpQuote)))
                copyBounded(sc.quote, sizeof(sc.quote), tmpQuote);

            if (s.containsKey("autoCandles")) sc.autoCandles = s["autoCandles"].as<bool>();
            if (s.containsKey("numCandles"))  sc.numCandles  = constrain(s["numCandles"].as<int>(), 5, MAX_CANDLES);
            if (s.containsKey("emaFast"))     sc.emaFast     = constrain(s["emaFast"].as<int>(), 2, 100);
            if (s.containsKey("emaSlow"))     sc.emaSlow     = constrain(s["emaSlow"].as<int>(), 2, 200);
            if (s.containsKey("rsiPeriod"))   sc.rsiPeriod   = constrain(s["rsiPeriod"].as<int>(), 2, 100);
            if (s.containsKey("heikinAshi"))  sc.heikinAshi  = s["heikinAshi"].as<bool>();
            if (s.containsKey("eventCallouts")) sc.eventCallouts = s["eventCallouts"].as<bool>();
        }
    } else {
        // Backward-compat: old format without "slots" — apply to slot 0
        SlotConfig& sc = slots[0].cfg;
        char tmpExchange[16], tmpCoin[16], tmpQuote[8], tmpInterval[8];
        copyBounded(tmpExchange, sizeof(tmpExchange), sc.exchange);
        copyBounded(tmpCoin, sizeof(tmpCoin), sc.coin);
        copyBounded(tmpQuote, sizeof(tmpQuote), sc.quote);
        copyBounded(tmpInterval, sizeof(tmpInterval), sc.interval);

        if (doc.containsKey("exchange")) copyBounded(tmpExchange, sizeof(tmpExchange), doc["exchange"].as<const char*>());
        if (doc.containsKey("coin"))     copyBounded(tmpCoin, sizeof(tmpCoin), doc["coin"].as<const char*>());
        if (doc.containsKey("quote"))    copyBounded(tmpQuote, sizeof(tmpQuote), doc["quote"].as<const char*>());
        if (doc.containsKey("interval")) copyBounded(tmpInterval, sizeof(tmpInterval), doc["interval"].as<const char*>());

        toUpperInPlace(tmpCoin);
        toUpperInPlace(tmpQuote);

        if (isValidExchange(tmpExchange)) copyBounded(sc.exchange, sizeof(sc.exchange), tmpExchange);
        if (isValidInterval(tmpInterval)) copyBounded(sc.interval, sizeof(sc.interval), tmpInterval);
        if (isValidSymbolToken(tmpCoin, sizeof(tmpCoin))) copyBounded(sc.coin, sizeof(sc.coin), tmpCoin);
        if (isValidSymbolToken(tmpQuote, sizeof(tmpQuote))) copyBounded(sc.quote, sizeof(sc.quote), tmpQuote);

        if (doc.containsKey("autoCandles")) sc.autoCandles = doc["autoCandles"].as<bool>();
        if (doc.containsKey("numCandles"))  sc.numCandles  = constrain(doc["numCandles"].as<int>(), 5, MAX_CANDLES);
        if (doc.containsKey("emaFast"))     sc.emaFast     = constrain(doc["emaFast"].as<int>(), 2, 100);
        if (doc.containsKey("emaSlow"))     sc.emaSlow     = constrain(doc["emaSlow"].as<int>(), 2, 200);
        if (doc.containsKey("rsiPeriod"))   sc.rsiPeriod   = constrain(doc["rsiPeriod"].as<int>(), 2, 100);
        if (doc.containsKey("heikinAshi"))  sc.heikinAshi  = doc["heikinAshi"].as<bool>();
        if (doc.containsKey("eventCallouts")) sc.eventCallouts = doc["eventCallouts"].as<bool>();
    }

    // Keep scratch globals in sync with slot 0
    copyBounded(cfgExchange, sizeof(cfgExchange), slots[0].cfg.exchange);
    copyBounded(cfgCoin, sizeof(cfgCoin), slots[0].cfg.coin);
    copyBounded(cfgQuote, sizeof(cfgQuote), slots[0].cfg.quote);
    copyBounded(cfgInterval, sizeof(cfgInterval), slots[0].cfg.interval);
    cfgNumCandles  = slots[0].cfg.numCandles;
    cfgAutoCandles = slots[0].cfg.autoCandles;
    cfgEmaFast     = slots[0].cfg.emaFast;
    cfgEmaSlow     = slots[0].cfg.emaSlow;
    cfgRsiPeriod   = slots[0].cfg.rsiPeriod;
    cfgHeikinAshi  = slots[0].cfg.heikinAshi;

    if (remoteOtaEnabled && !isHttpUrl(remoteOtaManifestUrl)) {
        server.send(400, "application/json", "{\"error\":\"remote OTA enabled but manifest URL is invalid\"}");
        return;
    }

    saveConfig();
    configTime(cfgTzOffset, 0, NTP_SERVER);

    server.send(200, "application/json", "{\"ok\":true}");
    Serial.println("Config updated via web UI");
}

void handleRefresh() {
    if (!authenticateRequest()) return;
    forceRefresh = true;
    server.send(200, "application/json", "{\"ok\":true}");
    Serial.println("Refresh triggered via web UI");
}

void handleRestart() {
    if (!authenticateRequest()) return;
    server.send(200, "application/json", "{\"ok\":true}");
    Serial.println("Reboot triggered via web UI");
    delay(500);
    ESP.restart();
}

// ── Display preview — serve framebuffer as 1-bit BMP ──

void handleDisplayBMP() {
    const int W = SCR_W;            // 800
    const int H = SCR_H;            // 480
    const int rowBytes = W / 8;     // 100  (already 4-byte aligned)
    const int imgSize  = rowBytes * H;
    const int fileSize = 62 + imgSize;

    // BMP file header (14) + DIB header (40) + 2-colour palette (8)
    uint8_t hdr[62];
    memset(hdr, 0, sizeof(hdr));

    // -- File header (14 bytes) --
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = fileSize; hdr[3] = fileSize >> 8; hdr[4] = fileSize >> 16; hdr[5] = fileSize >> 24;
    hdr[10] = 62;  // pixel data offset

    // -- BITMAPINFOHEADER (40 bytes) --
    hdr[14] = 40;                    // header size
    hdr[18] = W; hdr[19] = W >> 8;  // width
    hdr[22] = H; hdr[23] = H >> 8;  // height (positive = bottom-up)
    hdr[26] = 1;                     // planes
    hdr[28] = 1;                     // bits per pixel
    // compression = 0 (BI_RGB), image size can be 0 for BI_RGB

    // -- Colour table: index 0 = black, index 1 = white --
    // Entry 0: B G R A = 0,0,0,0
    hdr[54] = 0; hdr[55] = 0; hdr[56] = 0; hdr[57] = 0;
    // Entry 1: B G R A = 255,255,255,0
    hdr[58] = 0xFF; hdr[59] = 0xFF; hdr[60] = 0xFF; hdr[61] = 0;

    server.setContentLength(fileSize);
    server.send(200, "image/bmp", "");
    server.sendContent((const char*)hdr, sizeof(hdr));

    // Send rows bottom-to-top (BMP convention)
    for (int y = H - 1; y >= 0; y--) {
        server.sendContent((const char*)&framebuf[y * rowBytes], rowBytes);
    }
}

void handlePoloniexMarkets() {
    if (!authenticateRequest()) return;

    HTTPClient http;
    http.begin("https://api.poloniex.com/markets");
    http.setTimeout(12000);

    int httpCode = http.GET();
    if (httpCode != 200) {
        http.end();
        server.send(502, "application/json", "{\"error\":\"poloniex unavailable\"}");
        return;
    }

    String payload = http.getString();
    http.end();
    server.send(200, "application/json", payload);
}

// ── OTA firmware update via browser upload ──


void handleUpdateArm() {
    if (!authenticateRequest()) return;
    otaFailed = false;
    otaActive = false;
    otaProgressPct = 0;
    otaNeedsRender = true;
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleUpdateResult() {
    if (!authenticateRequest()) return;
    server.sendHeader("Connection", "close");
    if (Update.hasError()) {
        otaFailed = true;
        otaActive = false;
        otaProgressPct = 0;
        otaNeedsRender = true;
        updateOtaDisplay(true);
        if (otaDisplayReady) {
            epd.Sleep();
            otaDisplayReady = false;
        }
        server.send(500, "application/json", "{\"error\":\"Update failed\"}");
    } else {
        otaProgressPct = 100;
        otaNeedsRender = true;
        updateOtaDisplay(true);
        if (otaDisplayReady) {
            epd.Sleep();
            otaDisplayReady = false;
        }
        otaActive = false;
        server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Rebooting...\"}");
        delay(1200);
        ESP.restart();
    }
}


void handleUpdateUpload() {
    if (!authenticateRequest()) return;
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA update: %s\n", upload.filename.c_str());
        otaActive = true;
        otaFailed = false;
        otaProgressPct = 0;
        otaNeedsRender = true;
        otaLastRenderMs = 0;

        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
            otaFailed = true;
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
            otaFailed = true;
        }

        size_t total = Update.size();
        if (total > 0) {
            otaProgressPct = (int)((Update.progress() * 100UL) / total);
            otaProgressPct = constrain(otaProgressPct, 0, 100);
        }

        if ((millis() - otaLastRenderMs >= OTA_RENDER_INTERVAL_MS) || otaProgressPct >= 100) {
            otaNeedsRender = true;
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!Update.end(true)) {
            Update.printError(Serial);
            otaFailed = true;
        } else {
            otaProgressPct = 100;
            otaNeedsRender = true;
            Serial.println("OTA update complete");
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        otaFailed = true;
        otaNeedsRender = true;
        otaActive = false;
        Serial.println("OTA update aborted");
    }
}



struct RemoteOtaManifest {
    String version;
    String url;
    String sha256;
    int size;
    String board;
    String channel;
};

void markRemoteOtaFailure(const char* reason) {
    remoteOtaFailureCount++;
    unsigned long baseDelay = (unsigned long)remoteOtaCheckMin * 60UL * 1000UL;
    unsigned long backoff = baseDelay;
    for (int i = 1; i < remoteOtaFailureCount && i < 6; i++) backoff *= 2UL;
    unsigned long maxBackoff = 6UL * 60UL * 60UL * 1000UL;
    if (backoff > maxBackoff) backoff = maxBackoff;
    remoteOtaNextAllowedMs = millis() + backoff;
    Serial.printf("Remote OTA failed: %s (attempt %d, backoff %lu ms)\n", reason, remoteOtaFailureCount, backoff);
}

void toLowerHex(char* s) {
    if (!s) return;
    for (size_t i = 0; s[i]; i++) {
        s[i] = (char)tolower((unsigned char)s[i]);
    }
}

bool parseRemoteManifest(const String& payload, RemoteOtaManifest& mf) {
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, payload)) return false;
    if (!doc["version"].is<const char*>() || !doc["url"].is<const char*>() ||
        !doc["sha256"].is<const char*>() || !doc["board"].is<const char*>()) return false;

    mf.version = doc["version"].as<const char*>();
    mf.url = doc["url"].as<const char*>();
    mf.sha256 = doc["sha256"].as<const char*>();
    mf.board = doc["board"].as<const char*>();
    mf.size = doc["size"].is<int>() ? doc["size"].as<int>() : -1;
    mf.channel = doc["channel"].is<const char*>() ? doc["channel"].as<const char*>() : "";

    return isHttpUrl(mf.url.c_str()) && mf.sha256.length() == 64;
}

bool beginHttpForUrl(HTTPClient& http, const String& url) {
    static WiFiClientSecure secure;
    static WiFiClient plain;
    if (url.startsWith("https://")) {
        secure.setTimeout(15000);
        if (REMOTE_OTA_TLS_CA_CERT[0] != '\0') {
            secure.setCACert(REMOTE_OTA_TLS_CA_CERT);
        } else {
            secure.setInsecure();
        }
        return http.begin(secure, url);
    }
    plain.setTimeout(15000);
    return http.begin(plain, url);
}

bool runRemoteOtaUpdate(const RemoteOtaManifest& mf) {
    HTTPClient http;
    if (!beginHttpForUrl(http, mf.url)) {
        markRemoteOtaFailure("download begin failed");
        return false;
    }
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int code = http.GET();
    if (code != 200) {
        http.end();
        markRemoteOtaFailure("download http status != 200");
        return false;
    }

    int contentLen = http.getSize();
    if (mf.size > 0 && contentLen > 0 && contentLen != mf.size) {
        http.end();
        markRemoteOtaFailure("size mismatch with manifest");
        return false;
    }

    if (!Update.begin(mf.size > 0 ? (size_t)mf.size : UPDATE_SIZE_UNKNOWN)) {
        http.end();
        markRemoteOtaFailure("Update.begin failed");
        return false;
    }

    otaActive = true;
    otaFailed = false;
    otaProgressPct = 0;
    otaNeedsRender = true;

    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    size_t writtenTotal = 0;
    unsigned long started = millis();

    while (http.connected() && (contentLen < 0 || (int)writtenTotal < contentLen)) {
        size_t avail = stream->available();
        if (!avail) {
            delay(5);
            if (millis() - started > 180000) break;
            continue;
        }
        int n = stream->readBytes(buf, (avail > sizeof(buf)) ? sizeof(buf) : avail);
        if (n <= 0) continue;
        if (Update.write(buf, n) != (size_t)n) {
            mbedtls_sha256_free(&shaCtx);
            Update.abort();
            http.end();
            otaFailed = true;
            otaActive = false;
            otaNeedsRender = true;
            markRemoteOtaFailure("Update.write failed");
            return false;
        }
        mbedtls_sha256_update(&shaCtx, buf, n);
        writtenTotal += (size_t)n;
        if (mf.size > 0) {
            otaProgressPct = constrain((int)((writtenTotal * 100UL) / (unsigned long)mf.size), 0, 100);
            otaNeedsRender = true;
        }
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&shaCtx, digest);
    mbedtls_sha256_free(&shaCtx);

    char digestHex[65];
    for (int i = 0; i < 32; i++) sprintf(&digestHex[i * 2], "%02x", digest[i]);
    digestHex[64] = '\0';

    char expected[65];
    copyBounded(expected, sizeof(expected), mf.sha256.c_str());
    toLowerHex(expected);

    if (strcmp(digestHex, expected) != 0) {
        Update.abort();
        http.end();
        otaFailed = true;
        otaActive = false;
        otaNeedsRender = true;
        markRemoteOtaFailure("sha256 mismatch");
        return false;
    }

    if (mf.size > 0 && (int)writtenTotal != mf.size) {
        Update.abort();
        http.end();
        otaFailed = true;
        otaActive = false;
        otaNeedsRender = true;
        markRemoteOtaFailure("written size mismatch");
        return false;
    }

    if (!Update.end(true)) {
        http.end();
        otaFailed = true;
        otaActive = false;
        otaNeedsRender = true;
        markRemoteOtaFailure("Update.end failed");
        return false;
    }

    http.end();
    otaProgressPct = 100;
    otaNeedsRender = true;
    otaActive = false;
    remoteOtaFailureCount = 0;
    remoteOtaNextAllowedMs = 0;
    Serial.println("Remote OTA update complete; rebooting");
    delay(1000);
    ESP.restart();
    return true;
}

void remoteOtaTick() {
    if (!remoteOtaEnabled || otaActive || WiFi.status() != WL_CONNECTED) return;
    if (!isHttpUrl(remoteOtaManifestUrl)) return;

    unsigned long now = millis();
    unsigned long intervalMs = (unsigned long)remoteOtaCheckMin * 60UL * 1000UL;
    if (lastRemoteOtaCheckMs != 0 && now - lastRemoteOtaCheckMs < intervalMs) return;
    if (remoteOtaNextAllowedMs != 0 && now < remoteOtaNextAllowedMs) return;
    lastRemoteOtaCheckMs = now;

    HTTPClient http;
    String manifestUrl(remoteOtaManifestUrl);
    if (!beginHttpForUrl(http, manifestUrl)) {
        markRemoteOtaFailure("manifest begin failed");
        return;
    }
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int code = http.GET();
    if (code != 200) {
        http.end();
        markRemoteOtaFailure("manifest http status != 200");
        return;
    }

    String payload = http.getString();
    http.end();

    RemoteOtaManifest mf;
    if (!parseRemoteManifest(payload, mf)) {
        markRemoteOtaFailure("manifest parse/validation failed");
        return;
    }

    if (mf.url.startsWith("http://") && remoteOtaAutoApply) {
        markRemoteOtaFailure("auto-apply requires https artifact URL");
        return;
    }

    if (mf.board != String(FW_BOARD_ID)) {
        Serial.printf("Remote OTA skipped: board mismatch (%s != %s)\n", mf.board.c_str(), FW_BOARD_ID);
        remoteOtaFailureCount = 0;
        return;
    }

    if (mf.channel.length() > 0 && mf.channel != String(remoteOtaChannel)) {
        Serial.printf("Remote OTA skipped: channel mismatch (%s != %s)\n", mf.channel.c_str(), remoteOtaChannel);
        remoteOtaFailureCount = 0;
        return;
    }

    int verCmp = compareSemver(mf.version.c_str(), FW_VERSION);
    if (verCmp < 0 && !remoteOtaAllowDowngrade) {
        Serial.printf("Remote OTA downgrade skipped (%s < %s)\n", mf.version.c_str(), FW_VERSION);
        remoteOtaFailureCount = 0;
        return;
    }
    if (verCmp == 0) {
        remoteOtaFailureCount = 0;
        return;
    }

    if (!remoteOtaAutoApply) {
        Serial.printf("Remote OTA available: %s (auto-apply disabled)\n", mf.version.c_str());
        remoteOtaFailureCount = 0;
        return;
    }

    runRemoteOtaUpdate(mf);
}

void setupWebServer() {
    server.on("/",          HTTP_GET,  handleRoot);
    server.on("/api/status",HTTP_GET,  handleStatus);
    server.on("/api/config",HTTP_POST, handleConfigPost);
    server.on("/api/refresh",HTTP_POST,handleRefresh);
    server.on("/api/restart",HTTP_POST,handleRestart);
    server.on("/api/display",HTTP_GET, handleDisplayBMP);
    server.on("/api/poloniex/markets",HTTP_GET, handlePoloniexMarkets);
    server.on("/api/update/arm", HTTP_POST, handleUpdateArm);
    server.on("/api/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);
    server.begin();
    Serial.println("Web server started on port 80");

    MDNS.end();  // clean slate — safe even if never started
    if (MDNS.begin(MDNS_HOST)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS: http://%s.local\n", MDNS_HOST);
    }
}

// ═══════════════════════════════════════════════════════
// FRAMEBUFFER DIFF + DISPLAY UPDATE
// ═══════════════════════════════════════════════════════

float calcChangePct() {
    unsigned long changed = 0;
    for (int i = 0; i < BUF_SIZE; i++) {
        unsigned char diff = framebuf[i] ^ oldbuf[i];
        while (diff) { changed++; diff &= diff - 1; }
    }
    return (float)changed / (SCR_W * SCR_H);
}

void updateDisplay(bool forceFullRefresh) {
    float changePct = calcChangePct();
    float threshold = cfgPartialPct / 100.0f;
    bool doFull = forceFullRefresh
               || partialCount >= cfgFullRefEvery
               || changePct > threshold;

    if (doFull) {
        Serial.printf("Full refresh (%.1f%% changed, cycle %d)\n", changePct * 100, partialCount);
        epd.DisplayFrame(framebuf);
        partialCount = 0;
    } else {
        Serial.printf("Partial refresh (%.1f%% changed, cycle %d)\n", changePct * 100, partialCount);
        epd.DisplayFramePartial(oldbuf, framebuf);
        partialCount++;
    }
    memcpy(oldbuf, framebuf, BUF_SIZE);
}

// ═══════════════════════════════════════════════════════
// WIFI & NTP
// ═══════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════
// WIFI — STA with AP fallback
// ═══════════════════════════════════════════════════════

bool connectWiFiSTA() {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.printf("Connecting to %s", cfgSSID);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);   // auto-reconnect on drop
    WiFi.persistent(false);         // don't write creds to flash every boot
    WiFi.disconnect(true);
    delay(200);
    WiFi.begin(cfgSSID, cfgPass);

    // Disable modem sleep — keeps WiFi radio active for web server
    esp_wifi_set_ps(WIFI_PS_NONE);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500); Serial.print("."); attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

        // If we were in AP mode, tear it down gently
        if (apModeActive) {
            Serial.println("STA connected — disabling AP after stabilisation");
            delay(1000);  // let STA stabilise before mode switch
            WiFi.softAPdisconnect(true);
            delay(500);
            WiFi.mode(WIFI_STA);
            WiFi.setAutoReconnect(true);
            esp_wifi_set_ps(WIFI_PS_NONE);
            delay(500);
            apModeActive = false;

            // Re-setup mDNS on the STA interface
            MDNS.end();
            if (MDNS.begin(MDNS_HOST)) MDNS.addService("http", "tcp", 80);
        }
        return true;
    }

    Serial.println("\nWiFi STA FAILED");
    return false;
}

// Hard WiFi reset — nuclear option for ghost connections
void forceWiFiReset() {
    Serial.println(">>> Force WiFi reset — tearing down radio");
    WiFi.disconnect(true, true);  // disconnect + erase credentials from RAM
    WiFi.mode(WIFI_OFF);
    delay(1000);
    Serial.printf("  Heap free: %d, min: %d\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    delay(500);
    WiFi.begin(cfgSSID, cfgPass);
    esp_wifi_set_ps(WIFI_PS_NONE);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500); Serial.print("."); attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n  Reconnected! IP: %s, RSSI: %d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        // Re-init mDNS
        MDNS.end();
        if (MDNS.begin(MDNS_HOST)) MDNS.addService("http", "tcp", 80);
        consecutiveFails = 0;
        forceRefresh = true;  // trigger immediate chart refresh
    } else {
        Serial.println("\n  Force reset FAILED — starting AP");
        startAPMode();
    }
}

void startAPMode() {
    if (apModeActive) return;

    Serial.println("Starting AP fallback: EPDChart-Setup");
    WiFi.mode(WIFI_AP_STA);  // keep STA alive for retries
    WiFi.softAP("EPDChart-Setup");
    delay(200);

    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("AP IP: %s\n", apIP.toString().c_str());

    apModeActive = true;
    lastWifiRetryMs = millis();
}

void connectWiFi() {
    if (!connectWiFiSTA()) {
        startAPMode();
    }
}

void syncTime() {
    configTime(cfgTzOffset, 0, NTP_SERVER);
    Serial.print("Syncing NTP");
    time_t now = time(nullptr);
    int attempts = 0;
    while (now < 1700000000 && attempts < 20) {
        delay(500); Serial.print("."); now = time(nullptr); attempts++;
    }
    Serial.printf("\nTime: %lu\n", (unsigned long)now);
}

// ═══════════════════════════════════════════════════════
// FETCH CANDLES
// ═══════════════════════════════════════════════════════

bool ensureTimeSync() {
    time_t now = time(nullptr);
    if (now > 1700000000) return true;  // already synced

    Serial.println("Time not synced — retrying NTP...");
    configTime(cfgTzOffset, 0, NTP_SERVER);
    int attempts = 0;
    while (time(nullptr) < 1700000000 && attempts < 30) {
        delay(500); attempts++;
    }
    now = time(nullptr);
    Serial.printf("NTP retry result: %lu\n", (unsigned long)now);
    return (now > 1700000000);
}

// ── Common HTTP error logger ──
void logHttpError(int httpCode) {
    Serial.printf("HTTP %d | WiFi:%s RSSI:%d Heap:%d\n",
                  httpCode,
                  (WiFi.status() == WL_CONNECTED) ? "OK" : "DOWN",
                  WiFi.RSSI(),
                  ESP.getFreeHeap());
    if (httpCode < 0) {
        HTTPClient tmp;
        Serial.printf("  Error: %s\n", tmp.errorToString(httpCode).c_str());
    }
}

// ── Post-parse: calculate indicators & cache price ──
// Convert standard OHLCV candles to Heikin Ashi in-place
void applyHeikinAshi() {
    if (candleCount < 1) return;
    // First candle: HA open = (o+c)/2, HA close = (o+h+l+c)/4
    float prevO = (candles[0].o + candles[0].c) / 2.0f;
    float prevC = (candles[0].o + candles[0].h + candles[0].l + candles[0].c) / 4.0f;
    candles[0].o = prevO;
    candles[0].c = prevC;
    candles[0].h = max(candles[0].h, max(prevO, prevC));
    candles[0].l = min(candles[0].l, min(prevO, prevC));

    for (int i = 1; i < candleCount; i++) {
        float haC = (candles[i].o + candles[i].h + candles[i].l + candles[i].c) / 4.0f;
        float haO = (prevO + prevC) / 2.0f;
        float haH = max(candles[i].h, max(haO, haC));
        float haL = min(candles[i].l, min(haO, haC));
        candles[i].o = haO;
        candles[i].c = haC;
        candles[i].h = haH;
        candles[i].l = haL;
        // volume stays unchanged
        prevO = haO;
        prevC = haC;
    }
    Serial.println("Heikin Ashi applied");
}

void finalizeCandleData() {
    Serial.printf("Parsed %d candles\n", candleCount);
    if (cfgHeikinAshi) applyHeikinAshi();
    calcEMA(emaFast, cfgEmaFast);
    calcEMA(emaSlow, cfgEmaSlow);
    calcRSI();
    if (candleCount > 0) {
        lastPrice = candles[candleCount - 1].c;
        float firstOpen = candles[0].o;
        lastPctChange = ((lastPrice - firstOpen) / firstOpen) * 100.0f;
    }
}

// ── Hyperliquid ──────────────────────────────────────
bool fetchCandlesHyperliquid(int numCandles, uint64_t startMs, uint64_t nowMs) {
    char body[256];
    snprintf(body, sizeof(body),
        "{\"type\":\"candleSnapshot\",\"req\":{\"coin\":\"%s\",\"interval\":\"%s\",\"startTime\":%llu,\"endTime\":%llu}}",
        cfgCoin, cfgInterval, startMs, nowMs);
    Serial.printf("Hyperliquid POST: %s\n", body);

    HTTPClient http;
    http.begin("https://api.hyperliquid.xyz/info");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);

    int httpCode = http.POST(body);
    if (httpCode != 200) { logHttpError(httpCode); http.end(); return false; }

    String payload = http.getString();
    http.end();
    Serial.printf("Payload: %d bytes\n", payload.length());

    DynamicJsonDocument doc(98304);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) { Serial.printf("JSON error: %s\n", err.c_str()); return false; }

    JsonArray arr = doc.as<JsonArray>();
    candleCount = 0;
    int total = arr.size();
    int skip = (total > numCandles) ? total - numCandles : 0;


    for (int i = skip; i < total && candleCount < numCandles; i++) {
        JsonObject c = arr[i];
        candles[candleCount].o = atof(c["o"].as<const char*>());
        candles[candleCount].h = atof(c["h"].as<const char*>());
        candles[candleCount].l = atof(c["l"].as<const char*>());
        candles[candleCount].c = atof(c["c"].as<const char*>());
        candles[candleCount].v = atof(c["v"].as<const char*>());
        candles[candleCount].t = c["t"].as<uint64_t>();
        candleCount++;
    }
    return candleCount > 0;
}

// ── Binance-format (shared by Binance & AsterDEX) ───
bool fetchCandlesBinanceFormat(const char* baseUrl, int numCandles, uint64_t startMs, uint64_t nowMs) {
    char symbol[32];
    snprintf(symbol, sizeof(symbol), "%s%s", cfgCoin, cfgQuote);

    int requestCount = numCandles + 50;
    if (requestCount > 1000) requestCount = 1000;

    char url[256];
    snprintf(url, sizeof(url), "%s?symbol=%s&interval=%s&startTime=%llu&endTime=%llu&limit=%d",
             baseUrl, symbol, cfgInterval, startMs, nowMs, requestCount);
    Serial.printf("GET: %s\n", url);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(15000);

    int httpCode = http.GET();
    if (httpCode != 200) { logHttpError(httpCode); http.end(); return false; }

    String payload = http.getString();
    http.end();
    Serial.printf("Payload: %d bytes\n", payload.length());

    DynamicJsonDocument doc(98304);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) { Serial.printf("JSON error: %s\n", err.c_str()); return false; }

    JsonArray arr = doc.as<JsonArray>();
    candleCount = 0;
    int total = arr.size();
    int skip = (total > numCandles) ? total - numCandles : 0;

    // Binance klines: [openTime, open, high, low, close, volume, closeTime, ...]
    for (int i = skip; i < total && candleCount < numCandles; i++) {
        JsonArray k = arr[i];
        candles[candleCount].t = k[0].as<uint64_t>();
        candles[candleCount].o = atof(k[1].as<const char*>());
        candles[candleCount].h = atof(k[2].as<const char*>());
        candles[candleCount].l = atof(k[3].as<const char*>());
        candles[candleCount].c = atof(k[4].as<const char*>());
        candles[candleCount].v = atof(k[5].as<const char*>());
        candleCount++;
    }
    return candleCount > 0;
}

// ── Kraken ───────────────────────────────────────────
bool fetchCandlesKraken(int numCandles, uint64_t startMs) {
    char pair[32];
    snprintf(pair, sizeof(pair), "%s%s", cfgCoin, cfgQuote);

    char ivBuf[8];
    krakenInterval(cfgInterval, ivBuf, sizeof(ivBuf));

    uint64_t sinceSec = startMs / 1000ULL;

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.kraken.com/0/public/OHLC?pair=%s&interval=%s&since=%llu",
        pair, ivBuf, sinceSec);
    Serial.printf("GET: %s\n", url);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(15000);

    int httpCode = http.GET();
    if (httpCode != 200) { logHttpError(httpCode); http.end(); return false; }

    String payload = http.getString();
    http.end();
    Serial.printf("Payload: %d bytes\n", payload.length());

    DynamicJsonDocument doc(98304);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) { Serial.printf("JSON error: %s\n", err.c_str()); return false; }

    // Kraken wraps data in {"error":[], "result":{"PAIR":[[...]], "last":...}}
    JsonObject result = doc["result"];
    if (result.isNull()) { Serial.println("Kraken: no result object"); return false; }

    // Find the first key that isn't "last"
    JsonArray arr;
    for (JsonPair kv : result) {
        if (strcmp(kv.key().c_str(), "last") != 0) {
            arr = kv.value().as<JsonArray>();
            break;
        }
    }
    if (arr.isNull()) { Serial.println("Kraken: no candle array"); return false; }

    candleCount = 0;
    int total = arr.size();
    int skip = (total > numCandles) ? total - numCandles : 0;

    // Kraken: [timestamp, open, high, low, close, vwap, volume, count]
    for (int i = skip; i < total && candleCount < numCandles; i++) {
        JsonArray k = arr[i];
        candles[candleCount].t = k[0].as<uint64_t>() * 1000ULL;  // seconds → ms
        candles[candleCount].o = atof(k[1].as<const char*>());
        candles[candleCount].h = atof(k[2].as<const char*>());
        candles[candleCount].l = atof(k[3].as<const char*>());
        candles[candleCount].c = atof(k[4].as<const char*>());
        candles[candleCount].v = atof(k[6].as<const char*>());   // index 6 = volume
        candleCount++;
    }
    return candleCount > 0;
}

static bool readJsonFloat(JsonVariantConst value, float& out) {
    if (value.isNull()) return false;
    if (value.is<const char*>()) {
        const char* s = value.as<const char*>();
        if (!s || !*s) return false;
        out = atof(s);
        return true;
    }
    if (value.is<float>() || value.is<double>() || value.is<int>() || value.is<long>() || value.is<unsigned long>()) {
        out = value.as<float>();
        return true;
    }
    return false;
}

// ── Poloniex ─────────────────────────────────────────
bool fetchCandlesPoloniex(int numCandles, uint64_t startMs, uint64_t nowMs) {
    char symbol[32];
    snprintf(symbol, sizeof(symbol), "%s_%s", cfgCoin, cfgQuote);

    char ivBuf[16];
    poloniexInterval(cfgInterval, ivBuf, sizeof(ivBuf));

    int requestCount = numCandles + 50;
    if (requestCount > 500) requestCount = 500;

    char url[300];
    snprintf(url, sizeof(url),
        "https://api.poloniex.com/markets/%s/candles?interval=%s&startTime=%llu&endTime=%llu&limit=%d",
        symbol, ivBuf, startMs, nowMs, requestCount);
    Serial.printf("GET: %s\n", url);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(15000);

    int httpCode = http.GET();
    if (httpCode != 200) { logHttpError(httpCode); http.end(); return false; }

    String payload = http.getString();
    http.end();
    Serial.printf("Payload: %d bytes\n", payload.length());

    DynamicJsonDocument doc(98304);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) { Serial.printf("JSON error: %s\n", err.c_str()); return false; }

    JsonArray arr = doc.as<JsonArray>();
    candleCount = 0;
    int total = arr.size();
    int skip = (total > numCandles) ? total - numCandles : 0;

    if (total > 0) {
        JsonVariant first = arr[0];
        if (first.is<JsonObject>()) Serial.println("Poloniex candle payload shape: object");
        else if (first.is<JsonArray>()) Serial.println("Poloniex candle payload shape: array");
        else Serial.println("Poloniex candle payload shape: unknown");
    }

    for (int i = skip; i < total && candleCount < numCandles; i++) {
        float o, h, l, cl, v;
        uint64_t t = 0;

        if (arr[i].is<JsonArray>()) {
            // Array format: [low, high, open, close, amount, quantity,
            //   buyAmount, buyQuantity, tradeCount, ts, weightedAvg,
            //   interval, startTime, closeTime]
            JsonArray c = arr[i].as<JsonArray>();
            if (c.size() < 13 ||
                !readJsonFloat(c[2], o) || !readJsonFloat(c[1], h) ||
                !readJsonFloat(c[0], l) || !readJsonFloat(c[3], cl) ||
                !readJsonFloat(c[5], v)) {
                Serial.printf("Poloniex: skipping malformed candle at index %d\n", i);
                continue;
            }
            t = c[12].as<uint64_t>();
            if (t == 0 && c[12].is<const char*>()) {
                const char* ts = c[12].as<const char*>();
                if (ts && *ts) t = strtoull(ts, nullptr, 10);
            }
        } else {
            // Object format (legacy): {open, high, low, close, quantity, startTime, ...}
            JsonObject c = arr[i].as<JsonObject>();
            if (!readJsonFloat(c["open"], o) || !readJsonFloat(c["high"], h) || !readJsonFloat(c["low"], l) ||
                !readJsonFloat(c["close"], cl) || !readJsonFloat(c["quantity"], v)) {
                Serial.printf("Poloniex: skipping malformed candle at index %d\n", i);
                continue;
            }
            t = c["startTime"].as<uint64_t>();
            if (t == 0 && c["startTime"].is<const char*>()) {
                const char* ts = c["startTime"].as<const char*>();
                if (ts && *ts) t = strtoull(ts, nullptr, 10);
            }
        }

        if (t == 0) {
            Serial.printf("Poloniex: skipping candle with invalid startTime at index %d\n", i);
            continue;
        }

        candles[candleCount].o = o;
        candles[candleCount].h = h;
        candles[candleCount].l = l;
        candles[candleCount].c = cl;
        candles[candleCount].v = v;
        candles[candleCount].t = t;
        candleCount++;
    }
    return candleCount > 0;
}

// ── OKX ──────────────────────────────────────────────
bool fetchCandlesOKX(int numCandles) {
    char barStr[8];
    intervalToOkxBar(cfgInterval, barStr, sizeof(barStr));

    int limit = min(numCandles + 10, 100);

    char url[256];
    snprintf(url, sizeof(url),
        "https://www.okx.com/api/v5/market/candles?instId=%s-%s&bar=%s&limit=%d",
        cfgCoin, cfgQuote, barStr, limit);
    Serial.printf("GET: %s\n", url);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(15000);

    int httpCode = http.GET();
    if (httpCode != 200) { logHttpError(httpCode); http.end(); return false; }

    String payload = http.getString();
    http.end();
    Serial.printf("OKX payload: %d bytes\n", payload.length());

    DynamicJsonDocument doc(65536);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) { Serial.printf("OKX JSON error: %s\n", err.c_str()); return false; }

    JsonArray data = doc["data"].as<JsonArray>();
    if (data.isNull() || data.size() == 0) {
        Serial.println("OKX: empty data array");
        return false;
    }

    // OKX returns newest-first; collect into a temp buffer then reverse
    int rawCount = (int)data.size();
    if (rawCount > MAX_CANDLES) rawCount = MAX_CANDLES;

    // Parse into candles[] in reverse order (oldest first)
    candleCount = 0;
    for (int i = rawCount - 1; i >= 0; i--) {
        JsonArray row = data[i].as<JsonArray>();
        if (row.isNull() || row.size() < 5) continue;
        float o = atof(row[1].as<const char*>());
        float h = atof(row[2].as<const char*>());
        float l = atof(row[3].as<const char*>());
        float cl = atof(row[4].as<const char*>());
        float v = (row.size() > 5) ? atof(row[5].as<const char*>()) : 0.0f;
        uint64_t ts = strtoull(row[0].as<const char*>(), nullptr, 10);
        if (o <= 0 || h <= 0 || l <= 0 || cl <= 0) continue;
        if (candleCount >= MAX_CANDLES) break;
        candles[candleCount].o = o;
        candles[candleCount].h = h;
        candles[candleCount].l = l;
        candles[candleCount].c = cl;
        candles[candleCount].v = v;
        candles[candleCount].t = ts;
        candleCount++;
    }

    // Trim to requested count (keep most recent)
    if (candleCount > numCandles) {
        int drop = candleCount - numCandles;
        memmove(candles, candles + drop, (candleCount - drop) * sizeof(Candle));
        candleCount -= drop;
    }

    return candleCount > 0;
}

// ── Main dispatcher ──────────────────────────────────
bool fetchCandles() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected");
        return false;
    }
    if (!ensureTimeSync()) {
        Serial.println("Clock not synced — skipping fetch");
        return false;
    }

    int numCandles = getNumCandles();
    uint64_t ivMs = intervalToMs(cfgInterval);
    uint64_t nowMs = (uint64_t)time(nullptr) * 1000ULL;
    int requestCount = numCandles + 50;
    uint64_t startMs = nowMs - (uint64_t)requestCount * ivMs;

    Serial.printf("Exchange: %s  Pair: %s/%s  Interval: %s\n", cfgExchange, cfgCoin, cfgQuote, cfgInterval);

    bool ok = false;
    if (strcmp(cfgExchange, "binance") == 0)
        ok = fetchCandlesBinanceFormat("https://api.binance.com/api/v3/klines", numCandles, startMs, nowMs);
    else if (strcmp(cfgExchange, "asterdex") == 0)
        ok = fetchCandlesBinanceFormat("https://fapi.asterdex.com/fapi/v3/klines", numCandles, startMs, nowMs);
    else if (strcmp(cfgExchange, "kraken") == 0)
        ok = fetchCandlesKraken(numCandles, startMs);
    else if (strcmp(cfgExchange, "poloniex") == 0)
        ok = fetchCandlesPoloniex(numCandles, startMs, nowMs);
    else if (strcmp(cfgExchange, "okx") == 0)
        ok = fetchCandlesOKX(numCandles);
    else
        ok = fetchCandlesHyperliquid(numCandles, startMs, nowMs);

    if (ok) finalizeCandleData();
    return ok;
}

// ── Per-slot fetch: swaps globals, fetches, copies results into slot ──
bool fetchSlotCandles(int slotIdx) {
    if (slotIdx < 0 || slotIdx >= MAX_SLOTS) return false;
    SlotConfig& sc = slots[slotIdx].cfg;

    // Save current globals
    char savedExchange[16], savedCoin[16], savedQuote[8], savedInterval[8];
    copyBounded(savedExchange, sizeof(savedExchange), cfgExchange);
    copyBounded(savedCoin,     sizeof(savedCoin),     cfgCoin);
    copyBounded(savedQuote,    sizeof(savedQuote),    cfgQuote);
    copyBounded(savedInterval, sizeof(savedInterval), cfgInterval);
    int savedNumCandles = cfgNumCandles;
    bool savedAutoCandles = cfgAutoCandles;
    int savedEmaFast = cfgEmaFast;
    int savedEmaSlow = cfgEmaSlow;
    int savedRsiPeriod = cfgRsiPeriod;
    bool savedHeikinAshi = cfgHeikinAshi;

    // Set globals to this slot's config
    copyBounded(cfgExchange, sizeof(cfgExchange), sc.exchange);
    copyBounded(cfgCoin,     sizeof(cfgCoin),     sc.coin);
    copyBounded(cfgQuote,    sizeof(cfgQuote),    sc.quote);
    copyBounded(cfgInterval, sizeof(cfgInterval), sc.interval);
    cfgNumCandles  = sc.numCandles;
    cfgAutoCandles = sc.autoCandles;
    cfgEmaFast     = sc.emaFast;
    cfgEmaSlow     = sc.emaSlow;
    cfgRsiPeriod   = sc.rsiPeriod;
    cfgHeikinAshi  = sc.heikinAshi;

    Serial.printf("Fetching slot %d: %s %s/%s %s\n", slotIdx, sc.exchange, sc.coin, sc.quote, sc.interval);
    bool ok = fetchCandles();
    slots[slotIdx].lastFetchOk = ok;
    slots[slotIdx].lastFetchMs = millis();

    if (ok) {
        // Copy results from scratch globals into slot
        memcpy(slots[slotIdx].candles, candles, candleCount * sizeof(Candle));
        memcpy(slots[slotIdx].emaFastArr, emaFast, candleCount * sizeof(float));
        memcpy(slots[slotIdx].emaSlowArr, emaSlow, candleCount * sizeof(float));
        memcpy(slots[slotIdx].rsiVal, rsiVal, candleCount * sizeof(float));
        slots[slotIdx].candleCount = candleCount;
        slots[slotIdx].lastPrice = lastPrice;
        slots[slotIdx].lastPctChange = lastPctChange;
        detectSlotEvents(slots[slotIdx]);
    }

    // Restore globals
    copyBounded(cfgExchange, sizeof(cfgExchange), savedExchange);
    copyBounded(cfgCoin,     sizeof(cfgCoin),     savedCoin);
    copyBounded(cfgQuote,    sizeof(cfgQuote),    savedQuote);
    copyBounded(cfgInterval, sizeof(cfgInterval), savedInterval);
    cfgNumCandles  = savedNumCandles;
    cfgAutoCandles = savedAutoCandles;
    cfgEmaFast     = savedEmaFast;
    cfgEmaSlow     = savedEmaSlow;
    cfgRsiPeriod   = savedRsiPeriod;
    cfgHeikinAshi  = savedHeikinAshi;

    return ok;
}

// ═══════════════════════════════════════════════════════
// RENDER CHART
// ═══════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════
// STATUS SCREEN — shown when WiFi is down or on errors
// ═══════════════════════════════════════════════════════

void renderStatusScreen(const char* title, const char* line1, const char* line2,
                         const char* line3, const char* line4) {
    bufClear();

    // Box border
    int bx = 120, by = 130, bw = 560, bh = 220;
    drawRect(bx, by, bw, bh);
    drawRect(bx + 1, by + 1, bw - 2, bh - 2);  // double border

    // Title
    drawString(bx + (bw - strlen(title) * 12) / 2, by + 20, title, 2);

    // Separator
    hLine(bx + 30, bx + bw - 30, by + 46);

    // Info lines
    int ly = by + 60;
    if (line1 && strlen(line1) > 0) { drawString(bx + 30, ly, line1, 1); ly += 20; }
    if (line2 && strlen(line2) > 0) { drawString(bx + 30, ly, line2, 1); ly += 20; }
    if (line3 && strlen(line3) > 0) { drawString(bx + 30, ly, line3, 1); ly += 20; }
    if (line4 && strlen(line4) > 0) { drawString(bx + 30, ly, line4, 1); ly += 20; }

    // Footer
    char footer[64];
    snprintf(footer, sizeof(footer), "EPD Chart v2 | %s.local", MDNS_HOST);
    drawString(bx + (bw - strlen(footer) * 6) / 2, by + bh - 24, footer, 1);
}

void showWifiFailScreen() {
    char l1[64], l2[64], l3[64], l4[64];
    snprintf(l1, sizeof(l1), "Could not connect to: %s", cfgSSID);

    if (apModeActive) {
        IPAddress apIP = WiFi.softAPIP();
        snprintf(l2, sizeof(l2), "AP active: EPDChart-Setup");
        snprintf(l3, sizeof(l3), "Connect and visit: http://%s", apIP.toString().c_str());
        snprintf(l4, sizeof(l4), "Retrying STA every 60 seconds...");
    } else {
        snprintf(l2, sizeof(l2), "Retrying in 60 seconds...");
        l3[0] = '\0';
        l4[0] = '\0';
    }

    renderStatusScreen("WIFI DISCONNECTED", l1, l2, l3, l4);
}


void renderOtaProgressScreen(int pct, bool failed) {
    pct = constrain(pct, 0, 100);
    bufClear();

    // Framed splash panel
    int bx = 90, by = 110, bw = 620, bh = 260;
    drawRect(bx, by, bw, bh);
    drawRect(bx + 1, by + 1, bw - 2, bh - 2);

    const char* title = failed ? "OTA UPDATE FAILED" : "FIRMWARE UPDATE";
    drawString(bx + (bw - (int)strlen(title) * 12) / 2, by + 24, title, 2);
    hLine(bx + 24, bx + bw - 24, by + 56);

    const char* subtitle = failed ? "Please retry from Web GUI" : "Writing firmware to flash...";
    drawString(bx + (bw - (int)strlen(subtitle) * 6) / 2, by + 78, subtitle, 1);

    int barX = bx + 60;
    int barY = by + 130;
    int barW = bw - 120;
    int barH = 44;
    int innerPad = 4;

    // Modern-ish progress bar with inline percent
    drawRect(barX, barY, barW, barH);
    drawRect(barX + 1, barY + 1, barW - 2, barH - 2);

    int fillW = (barW - 2 * innerPad - 2) * pct / 100;
    if (fillW > 0) fillRect(barX + innerPad + 1, barY + innerPad + 1, fillW, barH - 2 * innerPad - 2, true);

    for (int t = 10; t < 100; t += 10) {
        int tx = barX + 1 + (barW - 2) * t / 100;
        vLine(tx, barY + barH - 8, barY + barH - 4);
    }

    char pctStr[12];
    snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
    drawString(barX + (barW - (int)strlen(pctStr) * 12) / 2, barY + barH + 16, pctStr, 2);

    const char* footer = failed ? "Device remains online for another upload" : "Do not power off the device";
    drawString(bx + (bw - (int)strlen(footer) * 6) / 2, by + bh - 28, footer, 1);
}

void updateOtaDisplay(bool forceFullRefresh) {
    if (!otaNeedsRender) return;

    if (!otaDisplayReady) {
        Serial.println("OTA display init...");
        if (epd.Init() != 0) {
            Serial.println("OTA display init failed");
            return;
        }
        otaDisplayReady = true;
        memset(oldbuf, 0xFF, BUF_SIZE);
    }

    renderOtaProgressScreen(otaProgressPct, otaFailed);

    if (forceFullRefresh) {
        epd.DisplayFrame(framebuf);
    } else {
        epd.DisplayFramePartial(oldbuf, framebuf);
    }
    memcpy(oldbuf, framebuf, BUF_SIZE);

    otaNeedsRender = false;
    otaLastRenderMs = millis();
}

int choosePriceLabelDecimals(float priceLo, float priceRange) {
    float step = priceRange / 5.0f;
    for (int decimals = 2; decimals <= 6; decimals++) {
        float scale = powf(10.0f, (float)decimals);
        long prev = 0;
        bool hasPrev = false;
        bool distinct = true;
        for (int g = 0; g <= 5; g++) {
            float price = priceLo + step * g;
            long rounded = lroundf(price * scale);
            if (hasPrev && rounded == prev) {
                distinct = false;
                break;
            }
            prev = rounded;
            hasPrev = true;
        }
        if (distinct) return decimals;
    }
    return 6;
}

enum TrendState {
    TREND_UP,
    TREND_FLAT,
    TREND_DOWN
};

TrendState classifyTrend(const ChartSlot& slot) {
    if (slot.candleCount < 2) return TREND_FLAT;

    const float pct = slot.lastPctChange;
    int tail = min(4, slot.candleCount - 1);
    float emaSlopePct = 0.0f;
    if (tail > 0 && slot.lastPrice > 0.0f) {
        float emaDelta = slot.emaFastArr[slot.candleCount - 1] - slot.emaFastArr[slot.candleCount - 1 - tail];
        emaSlopePct = (emaDelta / slot.lastPrice) * 100.0f;
    }

    if (pct > 0.6f || (pct > 0.2f && emaSlopePct > 0.05f)) return TREND_UP;
    if (pct < -0.6f || (pct < -0.2f && emaSlopePct < -0.05f)) return TREND_DOWN;
    return TREND_FLAT;
}

int trendConfidencePips(const ChartSlot& slot, TrendState state) {
    if (slot.candleCount < 3) return 1;

    int score = 0;
    int tail = min(12, slot.candleCount - 1);
    int upMoves = 0, downMoves = 0;
    for (int i = slot.candleCount - tail; i < slot.candleCount; i++) {
        if (slot.candles[i].c > slot.candles[i - 1].c) upMoves++;
        else if (slot.candles[i].c < slot.candles[i - 1].c) downMoves++;
    }

    float moveBias = (float)(upMoves - downMoves) / (float)tail;
    float pctMag = fabsf(slot.lastPctChange);
    float emaSlopePct = 0.0f;
    if (slot.lastPrice > 0.0f) {
        int emaTail = min(4, slot.candleCount - 1);
        float emaDelta = slot.emaFastArr[slot.candleCount - 1] - slot.emaFastArr[slot.candleCount - 1 - emaTail];
        emaSlopePct = (emaDelta / slot.lastPrice) * 100.0f;
    }
    float rsi = slot.rsiVal[slot.candleCount - 1];

    if (state == TREND_UP) {
        if (pctMag >= 0.8f) score++;
        if (moveBias > 0.2f) score++;
        if (emaSlopePct > 0.05f || rsi > 52.0f) score++;
    } else if (state == TREND_DOWN) {
        if (pctMag >= 0.8f) score++;
        if (moveBias < -0.2f) score++;
        if (emaSlopePct < -0.05f || rsi < 48.0f) score++;
    } else {
        if (pctMag <= 0.6f) score++;
        if (fabsf(moveBias) < 0.2f) score++;
        if (fabsf(emaSlopePct) < 0.05f && fabsf(rsi - 50.0f) < 7.0f) score++;
    }

    return constrain(score, 1, 3);
}

void drawTrendBlocks(int x, int y, int blockW, int blockH, int gap, TrendState state) {
    for (int i = 0; i < 3; i++) {
        int bx = x + i * (blockW + gap);
        bool active = (state == TREND_DOWN && i == 0) || (state == TREND_FLAT && i == 1) || (state == TREND_UP && i == 2);
        drawRect(bx, y, blockW, blockH);
        if (active) fillRect(bx + 1, y + 1, blockW - 2, blockH - 2, true);
    }
}

void drawTrendPips(int x, int y, int count, int filled, int pipSize = 4, int gap = 2) {
    filled = constrain(filled, 0, count);
    for (int i = 0; i < count; i++) {
        int px = x + i * (pipSize + gap);
        if (i < filled) fillRect(px, y, pipSize, pipSize, true);
        else drawRect(px, y, pipSize, pipSize);
    }
}

static const uint8_t moodFaceBearish[] PROGMEM = {
    0x3C, 0x42, 0xA5, 0x81, 0xA5, 0x99, 0x42, 0x3C
};

static const uint8_t moodFaceNeutral[] PROGMEM = {
    0x3C, 0x42, 0xA5, 0x81, 0xBD, 0x81, 0x42, 0x3C
};

static const uint8_t moodFaceBullish[] PROGMEM = {
    0x3C, 0x42, 0xA5, 0x81, 0x99, 0xA5, 0x42, 0x3C
};

MoodInfo moodFromPct(float pct) {
    MoodInfo mood = {MOOD_NEUTRAL, "STABLE", "none", pct};

    if (pct <= -1.25f) mood.id = MOOD_VERY_BEARISH;
    else if (pct <= -0.35f) mood.id = MOOD_BEARISH;
    else if (pct >= 1.25f) mood.id = MOOD_VERY_BULLISH;
    else if (pct >= 0.35f) mood.id = MOOD_BULLISH;
    else mood.id = MOOD_NEUTRAL;

    if (cfgCaptionVerbosity == 0) {
        if (mood.id == MOOD_VERY_BEARISH) mood.caption = "PANIC";
        else if (mood.id == MOOD_BEARISH) mood.caption = "DOWN";
        else if (mood.id == MOOD_BULLISH) mood.caption = "UP";
        else if (mood.id == MOOD_VERY_BULLISH) mood.caption = "RIP";
        else mood.caption = "FLAT";
    } else if (cfgCaptionVerbosity == 2) {
        if (mood.id == MOOD_VERY_BEARISH) mood.caption = "AGGRESSIVE SELL PRESSURE";
        else if (mood.id == MOOD_BEARISH) mood.caption = "BEARS IN CONTROL";
        else if (mood.id == MOOD_BULLISH) mood.caption = "BULLS BUILDING";
        else if (mood.id == MOOD_VERY_BULLISH) mood.caption = "STRONG UPSIDE MOMENTUM";
        else mood.caption = "BALANCED FLOW";
    } else {
        if (mood.id == MOOD_VERY_BEARISH) mood.caption = "VERY BEARISH";
        else if (mood.id == MOOD_BEARISH) mood.caption = "BEARISH";
        else if (mood.id == MOOD_BULLISH) mood.caption = "BULLISH";
        else if (mood.id == MOOD_VERY_BULLISH) mood.caption = "VERY BULLISH";
        else mood.caption = "NEUTRAL";
    }

    if (mood.id == MOOD_VERY_BEARISH || mood.id == MOOD_VERY_BULLISH) mood.style = "bold";
    else if (mood.id == MOOD_BEARISH || mood.id == MOOD_BULLISH) mood.style = "blink";
    else mood.style = "none";

    return mood;
}

MoodInfo getAggregateMood() {
    int active = constrain(cfgLayout, 1, MAX_SLOTS);
    int tracked = 0;
    float sum = 0.0f;

    for (int i = 0; i < active; i++) {
        if (slots[i].candleCount < 2) continue;
        tracked++;
        sum += slots[i].lastPctChange;
    }

    float aggPct = (tracked > 0) ? (sum / tracked) : 0.0f;
    return moodFromPct(aggPct);
}

void drawMoodHud(const Viewport& vp, const MoodInfo& mood, bool fullScreen) {
    if (!cfgPersonalityEnabled) return;

    int hudH = fullScreen ? 24 : 18;
    int hudW = min(vp.w - 6, fullScreen ? 380 : 250);
    int hudX = vp.x + 3;
    int hudY = vp.y + 2;

    drawRect(hudX, hudY, hudW, hudH);
    if (strcmp(mood.style, "bold") == 0) {
        drawRect(hudX + 1, hudY + 1, hudW - 2, hudH - 2);
    } else if (strcmp(mood.style, "blink") == 0) {
        hLineDash(hudX + 1, hudX + hudW - 2, hudY + hudH - 2, 2, 2);
    }

    char hudText[48];
    snprintf(hudText, sizeof(hudText), "%s %.2f%%", mood.caption, mood.aggregatePct);

    if (fullScreen) {
        // Full-screen: draw face icon at scale 2 beside the text
        int faceScale = 2;
        int faceW = 8 * faceScale;
        const uint8_t* faceBmp = moodFaceNeutral;
        if (mood.id == MOOD_VERY_BEARISH || mood.id == MOOD_BEARISH) faceBmp = moodFaceBearish;
        else if (mood.id == MOOD_BULLISH || mood.id == MOOD_VERY_BULLISH) faceBmp = moodFaceBullish;
        drawBitmapScaledFromProgmem(faceBmp, 8, 8, hudX + 3, hudY + (hudH - faceW) / 2, faceScale);
        drawString(hudX + 8 + faceW, hudY + 8, hudText, 1);
    } else {
        // Multi-panel: text only in the top bar; face is drawn larger in the bottom-right corner
        drawString(hudX + 4, hudY + 6, hudText, 1);
    }
}

void renderSlotChart(const Viewport& vp, ChartSlot& slot) {
    const SlotConfig& sc = slot.cfg;
    bool isFullScreen = (vp.w >= 700 && vp.h >= 400);
    bool isHalf = (vp.w >= 700 && !isFullScreen);

    // Adaptive layout values based on viewport size
    int moodReserve = cfgPersonalityEnabled ? (isFullScreen ? 26 : 20) : 0;
    int mT = (isFullScreen ? 31 : 16) + moodReserve;
    // Keep enough breathing room for time labels in multi-panel layouts.
    // Smaller viewports were clipping the bottom of labels/bars near row dividers.
    int mB = isFullScreen ? 18 : 14;
    int mL = (vp.w >= 600) ? 9 : 5;
    int mR = isFullScreen ? 72 : (vp.w >= 500 ? 48 : 40);
    int rH = isFullScreen ? 38 : 22;
    int vH = isFullScreen ? 24 : 14;
    int gp = isFullScreen ? 4 : 2;
    int titleScale = isFullScreen ? 2 : 1;
    int priceScale = isFullScreen ? 2 : 1;

    // Compute derived layout relative to viewport
    int chartW = vp.w - mL - mR;
    int rsiTop = vp.y + vp.h - mB - vH - gp - rH;
    int chartH = rsiTop - gp - (vp.y + mT);
    int volTop = vp.y + vp.h - mB - vH;

    if (slot.candleCount == 0) {
        drawString(vp.x + vp.w / 2 - 21, vp.y + vp.h / 2 - 3, "NO DATA", 1);
        return;
    }

    // Runtime candle geometry
    int candleGap  = (vp.w < 500) ? 1 : 2;
    int candleW    = (chartW - (slot.candleCount - 1) * candleGap) / slot.candleCount;
    if (candleW < 1) candleW = 1;
    int candleStep = candleW + candleGap;

    // Price range
    float priceHi = -1e9, priceLo = 1e9, volMax = 0;
    for (int i = 0; i < slot.candleCount; i++) {
        if (slot.candles[i].h > priceHi) priceHi = slot.candles[i].h;
        if (slot.candles[i].l < priceLo) priceLo = slot.candles[i].l;
        if (slot.candles[i].v > volMax)  volMax  = slot.candles[i].v;
    }
    float priceRange = priceHi - priceLo;
    if (priceRange < 0.01) priceRange = 1.0;
    float pad = priceRange * 0.02;
    priceHi += pad; priceLo -= pad;
    priceRange = priceHi - priceLo;

    if (chartH < 1) chartH = 1;
    float priceToYPx = (float)chartH / priceRange;
    auto priceToY = [&](float p) -> int {
        return (vp.y + mT) + chartH - (int)((p - priceLo) * priceToYPx);
    };

    // ── Title bar ──
    float pctChange = slot.lastPctChange;
    char pctSign = (pctChange >= 0) ? '+' : '-';
    float absPct = (pctChange < 0) ? -pctChange : pctChange;

    char exLabel[12];
    if (strcmp(sc.exchange, "hyperliquid") == 0) strncpy(exLabel, "HL", sizeof(exLabel));
    else if (strcmp(sc.exchange, "binance") == 0) strncpy(exLabel, "BIN", sizeof(exLabel));
    else if (strcmp(sc.exchange, "asterdex") == 0) strncpy(exLabel, "ASTER", sizeof(exLabel));
    else if (strcmp(sc.exchange, "kraken") == 0) strncpy(exLabel, "KRK", sizeof(exLabel));
    else if (strcmp(sc.exchange, "poloniex") == 0) strncpy(exLabel, "POLO", sizeof(exLabel));
    else strncpy(exLabel, sc.exchange, sizeof(exLabel));

    char title[80];
    if (vp.w < 500) {
        // Quarter mode: compact title
        snprintf(title, sizeof(title), "%s:%s %s%s", exLabel, sc.coin, sc.interval,
                 sc.heikinAshi ? " HA" : "");
    } else {
        snprintf(title, sizeof(title), "%s:%s/%s  %s%s", exLabel, sc.coin, sc.quote, sc.interval,
                 sc.heikinAshi ? "  HA" : "");
    }
    drawString(vp.x + mL, vp.y + (isFullScreen ? 5 : 3) + moodReserve, title, titleScale);

    // Legend line (EMA/RSI info) — only if there's room
    if (isFullScreen) {
        char legend[48];
        snprintf(legend, sizeof(legend), "EMA%d/EMA%d  RSI%d:%.0f",
                 sc.emaFast, sc.emaSlow, sc.rsiPeriod, slot.rsiVal[slot.candleCount - 1]);
        drawString(vp.x + mL + strlen(title) * 12 + 20, vp.y + 12 + moodReserve, legend, 1);
    }

    // Price + pct on the right
    char priceStr[32];
    snprintf(priceStr, sizeof(priceStr), "%.2f %c%.1f%%", slot.lastPrice, pctSign, absPct);

    TrendState trend = classifyTrend(slot);
    int trendPips = trendConfidencePips(slot, trend);

    int blockW = (vp.w < 500) ? 8 : (isFullScreen ? 12 : 10);
    int blockH = (vp.w < 500) ? 7 : (isFullScreen ? 10 : 9);
    int blockGap = 2;
    int blocksW = blockW * 3 + blockGap * 2;
    int blocksX = vp.x + vp.w - 4 - blocksW;
    int blocksY = vp.y + (isFullScreen ? 3 : 2);
    drawTrendBlocks(blocksX, blocksY, blockW, blockH, blockGap, trend);

    if (vp.w >= 500) {
        drawTrendPips(blocksX, blocksY + blockH + 2, 3, trendPips, 3, 2);
    }

    int priceRight = blocksX - 4;
    drawStringR(priceRight, vp.y + 3 + moodReserve, priceStr, priceScale);

    // IP address — only in full-screen single chart mode
    if (isFullScreen) {
        char ipStr[32];
        snprintf(ipStr, sizeof(ipStr), "%s", WiFi.localIP().toString().c_str());
        drawStringR(priceRight - 6, vp.y + 22 + moodReserve, ipStr, 1);
    }

    MoodInfo slotMood = isFullScreen ? currentMood : moodFromPct(slot.lastPctChange);
    drawMoodHud(vp, slotMood, isFullScreen);
    hLine(vp.x, vp.x + vp.w - 1, vp.y + mT - 2);

    // Multi-panel: draw mood face at 2× scale in the bottom-right whitespace
    // (the mR zone below the volume separator line — no other content there).
    if (!isFullScreen && cfgPersonalityEnabled) {
        const uint8_t* faceBmp = moodFaceNeutral;
        if (slotMood.id == MOOD_VERY_BEARISH || slotMood.id == MOOD_BEARISH) faceBmp = moodFaceBearish;
        else if (slotMood.id == MOOD_BULLISH || slotMood.id == MOOD_VERY_BULLISH) faceBmp = moodFaceBullish;
        int faceScale = 2;
        int faceW = 8 * faceScale;  // 16 px
        int faceX = vp.x + mL + chartW + (mR - faceW) / 2;
        // Centre vertically in the zone from the volume separator to the panel bottom
        int bottomZoneH = mB + vH;
        int faceY = vp.y + vp.h - bottomZoneH + (bottomZoneH - faceW) / 2;
        drawBitmapScaledFromProgmem(faceBmp, 8, 8, faceX, faceY, faceScale);
    }

    // Event strip
    if (sc.eventCallouts && slot.lastEvent.type != SLOT_EVENT_NONE && slot.lastEvent.message[0] != '\0') {
        char eventText[96];
        char timeBuf[8] = "";
        if (slot.lastEvent.ts > 0) {
            time_t ets = (time_t)(slot.lastEvent.ts / 1000ULL);
            struct tm* etm = gmtime(&ets);
            if (etm) snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", etm->tm_hour, etm->tm_min);
        }
        if (timeBuf[0] != '\0') snprintf(eventText, sizeof(eventText), "%s %s", timeBuf, slot.lastEvent.message);
        else snprintf(eventText, sizeof(eventText), "%s", slot.lastEvent.message);

        int stripX = vp.x + mL;
        int stripY = vp.y + mT + 1;
        int stripW = chartW - 2;
        int stripH = isFullScreen ? 12 : 10;
        if (stripW > 60) {
            drawRect(stripX, stripY, stripW, stripH);
            int maxChars = (stripW - 6) / 6;
            if (maxChars > 4) {
                char clipped[96];
                strncpy(clipped, eventText, sizeof(clipped) - 1);
                clipped[sizeof(clipped) - 1] = '\0';
                int n = strlen(clipped);
                if (n > maxChars) {
                    if (maxChars >= 3) {
                        clipped[maxChars - 3] = '.';
                        clipped[maxChars - 2] = '.';
                        clipped[maxChars - 1] = '.';
                        clipped[maxChars] = '\0';
                    } else {
                        clipped[maxChars] = '\0';
                    }
                }
                drawString(stripX + 3, stripY + (isFullScreen ? 3 : 2), clipped, 1);
            }
        }
    }

    // ── Price grid ──
    int priceLabelDecimals = choosePriceLabelDecimals(priceLo, priceRange);
    char priceFmt[8];
    snprintf(priceFmt, sizeof(priceFmt), "%%.%df", priceLabelDecimals);

    int gridLines = isFullScreen ? 5 : 3;
    for (int g = 0; g <= gridLines; g++) {
        float price = priceLo + priceRange * g / gridLines;
        int y = priceToY(price);
        if (y >= vp.y + mT && y <= vp.y + mT + chartH) {
            hLineDash(vp.x + mL, vp.x + mL + chartW, y);
            char lbl[16];
            snprintf(lbl, sizeof(lbl), priceFmt, price);
            int lblY = constrain(y - 3, vp.y + mT + 1, vp.y + mT + chartH - 7);
            drawString(vp.x + mL + chartW + 3, lblY, lbl, 1);
        }
    }

    // ── Draw candles ──
    for (int i = 0; i < slot.candleCount; i++) {
        int cx = vp.x + mL + i * candleStep;
        int candleMid = cx + candleW / 2;

        int yOpen  = constrain(priceToY(slot.candles[i].o), vp.y + mT, vp.y + mT + chartH);
        int yClose = constrain(priceToY(slot.candles[i].c), vp.y + mT, vp.y + mT + chartH);
        int yHigh  = constrain(priceToY(slot.candles[i].h), vp.y + mT, vp.y + mT + chartH);
        int yLow   = constrain(priceToY(slot.candles[i].l), vp.y + mT, vp.y + mT + chartH);

        bool bullish = (slot.candles[i].c >= slot.candles[i].o);
        int bodyTop = bullish ? yClose : yOpen;
        int bodyBot = bullish ? yOpen  : yClose;
        if (bodyTop == bodyBot) bodyBot = bodyTop + 1;

        vLine(candleMid, yHigh, yLow);
        if (bullish) drawRect(cx, bodyTop, candleW, bodyBot - bodyTop + 1);
        else         fillRect(cx, bodyTop, candleW, bodyBot - bodyTop + 1, true);

        // Volume bar
        if (volMax > 0) {
            int vHeight = max(1, (int)((slot.candles[i].v / volMax) * vH));
            int vy = volTop + vH - vHeight;
            if (bullish) drawRect(cx, vy, candleW, vHeight);
            else         fillRect(cx, vy, candleW, vHeight, true);
        }
    }

    // ── EMA lines ──
    for (int i = 1; i < slot.candleCount; i++) {
        int x0 = vp.x + mL + (i - 1) * candleStep + candleW / 2;
        int x1 = vp.x + mL + i * candleStep + candleW / 2;
        if (i >= sc.emaFast) {
            int y0 = constrain(priceToY(slot.emaFastArr[i-1]), vp.y + mT, vp.y + mT + chartH);
            int y1 = constrain(priceToY(slot.emaFastArr[i]),   vp.y + mT, vp.y + mT + chartH);
            drawLine(x0, y0, x1, y1, false);
        }
        if (i >= sc.emaSlow) {
            int y0 = constrain(priceToY(slot.emaSlowArr[i-1]), vp.y + mT, vp.y + mT + chartH);
            int y1 = constrain(priceToY(slot.emaSlowArr[i]),   vp.y + mT, vp.y + mT + chartH);
            drawLine(x0, y0, x1, y1, true);
        }
    }

    // ── Chart border ──
    hLine(vp.x + mL, vp.x + mL + chartW, vp.y + mT);
    hLine(vp.x + mL, vp.x + mL + chartW, vp.y + mT + chartH);
    vLine(vp.x + mL, vp.y + mT, vp.y + mT + chartH);
    vLine(vp.x + mL + chartW, vp.y + mT, vp.y + mT + chartH);

    // ── RSI strip ──
    hLine(vp.x + mL, vp.x + mL + chartW, rsiTop);
    hLine(vp.x + mL, vp.x + mL + chartW, rsiTop + rH);
    vLine(vp.x + mL, rsiTop, rsiTop + rH);
    vLine(vp.x + mL + chartW, rsiTop, rsiTop + rH);

    int rsi70y = rsiTop + rH - (int)(70.0f / 100.0f * rH);
    int rsi30y = rsiTop + rH - (int)(30.0f / 100.0f * rH);
    hLineDot(vp.x + mL + 1, vp.x + mL + chartW - 1, rsi70y);
    hLineDot(vp.x + mL + 1, vp.x + mL + chartW - 1, rsi30y);

    if (isFullScreen) {
        int rsi50y = rsiTop + rH - (int)(50.0f / 100.0f * rH);
        hLineDot(vp.x + mL + 1, vp.x + mL + chartW - 1, rsi50y);
    }

    drawString(vp.x + mL + chartW + 3, rsi70y - 3, "70", 1);
    drawString(vp.x + mL + chartW + 3, rsi30y - 3, "30", 1);
    if (isFullScreen || isHalf) {
        drawString(vp.x + mL + chartW + 3, rsiTop + 2, "RSI", 1);
    }

    for (int i = sc.rsiPeriod + 1; i < slot.candleCount; i++) {
        int x0 = vp.x + mL + (i - 1) * candleStep + candleW / 2;
        int x1 = vp.x + mL + i * candleStep + candleW / 2;
        int y0 = rsiTop + rH - (int)(constrain(slot.rsiVal[i-1], 0.0f, 100.0f) / 100.0f * rH);
        int y1 = rsiTop + rH - (int)(constrain(slot.rsiVal[i],   0.0f, 100.0f) / 100.0f * rH);
        y0 = constrain(y0, rsiTop, rsiTop + rH);
        y1 = constrain(y1, rsiTop, rsiTop + rH);
        drawLine(x0, y0, x1, y1, false);
    }

    // ── Volume separator + label ──
    hLine(vp.x + mL, vp.x + mL + chartW, volTop - 1);
    if (isFullScreen || isHalf) {
        drawString(vp.x + mL + chartW + 3, volTop + 2, "Vol", 1);
    }

    // ── Time labels — adaptive spacing ──
    int labelTarget = isFullScreen ? 7 : (isHalf ? 6 : 4);
    int labelEvery = max(1, slot.candleCount / labelTarget);
    for (int i = 0; i < slot.candleCount; i += labelEvery) {
        int cx = vp.x + mL + i * candleStep + candleW / 2;
        vLine(cx, volTop + vH + 1, volTop + vH + 3);

        time_t ts = (time_t)(slot.candles[i].t / 1000ULL);
        struct tm* tm_info = gmtime(&ts);
        if (tm_info) {
            char timeLbl[12];
            uint64_t ivMs = intervalToMs(sc.interval);
            if (ivMs >= 86400000ULL)
                snprintf(timeLbl, sizeof(timeLbl), "%02d/%02d", tm_info->tm_mday, tm_info->tm_mon + 1);
            else
                snprintf(timeLbl, sizeof(timeLbl), "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);
            drawString(cx - 12, volTop + vH + 5, timeLbl, 1);
        }
    }

    // ── Footer — only in full-screen mode ──
    if (isFullScreen) {
        time_t now = time(nullptr);
        struct tm* t = gmtime(&now);
        char tsStr[32];
        snprintf(tsStr, sizeof(tsStr), "%04d-%02d-%02d %02d:%02d UTC",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min);
        drawStringR(vp.x + vp.w - 10, vp.y + vp.h - 12, tsStr, 1);

        char urlStr[64];
        snprintf(urlStr, sizeof(urlStr), "%s  |  %s.local",
                 WiFi.localIP().toString().c_str(), MDNS_HOST);
        drawString(vp.x + mL, vp.y + vp.h - 12, urlStr, 1);
    }
}

void drawGlobalFooter(int y, int h) {
    if (h < 8) return;

    hLine(0, SCR_W - 1, y);

    time_t now = time(nullptr);
    struct tm* t = gmtime(&now);
    char tsStr[32] = "UTC --:--";
    if (t) {
        snprintf(tsStr, sizeof(tsStr), "UTC %02d:%02d", t->tm_hour, t->tm_min);
    }

    char netStr[80];
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(netStr, sizeof(netStr), "%s  %s.local", WiFi.localIP().toString().c_str(), MDNS_HOST);
    } else {
        snprintf(netStr, sizeof(netStr), "%s  AP mode", MDNS_HOST);
    }

    int textY = y + max(1, h - 9);
    drawString(6, textY, netStr, 1);
    drawStringR(SCR_W - 6, textY, tsStr, 1);
}

// ── Layout orchestrator ──
void renderAllCharts() {
    bufClear();

    int activeSlots = cfgLayout;
    Viewport vps[MAX_SLOTS];
    const int footerH = (cfgLayout == 1) ? 0 : 12;
    const int chartAreaH = SCR_H - footerH;

    if (cfgLayout == 1) {
        vps[0] = {0, 0, SCR_W, SCR_H};
    } else if (cfgLayout == 2) {
        int rowGap = 4;
        int rowH = (chartAreaH - rowGap) / 2;
        int y2 = rowH + rowGap;
        vps[0] = {0, 0, SCR_W, rowH};
        vps[1] = {0, y2, SCR_W, rowH};
    } else {
        // 3 or 4: 2x2 grid
        int colGap = 4;
        int rowGap = 4;
        int colW = (SCR_W - colGap) / 2;
        int rowH = (chartAreaH - rowGap) / 2;
        int x2 = colW + colGap;
        int y2 = rowH + rowGap;
        vps[0] = {0, 0, colW, rowH};
        vps[1] = {x2, 0, colW, rowH};
        vps[2] = {0, y2, colW, rowH};
        if (cfgLayout == 4) {
            vps[3] = {x2, y2, colW, rowH};
        }
    }

    currentMood = getAggregateMood();
    for (int i = 0; i < activeSlots; i++) {
        renderSlotChart(vps[i], slots[i]);
    }

    // Draw divider lines
    if (cfgLayout >= 2) {
        int centerY = vps[0].h + 1;
        hLine(0, SCR_W - 1, centerY);
        hLine(0, SCR_W - 1, centerY + 1);
    }
    if (cfgLayout >= 3) {
        int centerX = vps[0].w + 1;
        vLine(centerX, 0, chartAreaH - 1);
        vLine(centerX + 1, 0, chartAreaH - 1);
    }

    if (footerH > 0) {
        drawGlobalFooter(chartAreaH, footerH);
    }
}

// Legacy wrapper for backward compatibility
void renderChart() {
    // For single-slot mode, populate scratch globals from slot 0
    if (cfgLayout == 1 && slots[0].candleCount > 0) {
        renderAllCharts();
    } else {
        renderAllCharts();
    }
}

// ═══════════════════════════════════════════════════════
// DISPLAY REFRESH CYCLE
// ═══════════════════════════════════════════════════════

void showSplash() {
    bufClear();

    const int logoScale = 4;
    const int logoW = ARMOURED_BIRD_64_WIDTH * logoScale;
    const int logoH = ARMOURED_BIRD_64_HEIGHT * logoScale;
    const int logoX = (SCR_W - logoW) / 2;
    const int logoY = 56;

    drawBitmapScaledFromProgmem(armoured_bird_64x64,
                                ARMOURED_BIRD_64_WIDTH,
                                ARMOURED_BIRD_64_HEIGHT,
                                logoX, logoY, logoScale);

    const char* title = "Armoured-Candles";
    const int titleScale = 3;
    int titleW = (int)strlen(title) * 6 * titleScale;
    drawString((SCR_W - titleW) / 2, logoY + logoH + 34, title, titleScale);

    char versionLine[32];
    snprintf(versionLine, sizeof(versionLine), "Version %s", FW_VERSION);
    int versionW = (int)strlen(versionLine) * 6 * 2;
    drawString((SCR_W - versionW) / 2, logoY + logoH + 78, versionLine, 2);

    memset(oldbuf, 0xFF, BUF_SIZE);
    epd.DisplayFrame(framebuf);
    memcpy(oldbuf, framebuf, BUF_SIZE);
    Serial.printf("Splash screen displayed (%s)\n", FW_VERSION);
}

void doRefreshCycle(bool fullRefresh) {
    // Diagnostics
    Serial.printf("  Heap: %d free, %d min | WiFi: %s RSSI:%d | Fails: %d | Layout: %d\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  (WiFi.status() == WL_CONNECTED) ? "OK" : "DOWN",
                  WiFi.RSSI(), consecutiveFails, cfgLayout);

    connectWiFi();

    Serial.println("Re-init e-Paper...");
    if (epd.Init() != 0) {
        Serial.println("e-Paper re-init FAILED");
        return;
    }

    bool anySuccess = false;
    int activeSlots = cfgLayout;
    for (int i = 0; i < activeSlots; i++) {
        if (fetchSlotCandles(i)) {
            anySuccess = true;
        }
        server.handleClient();  // keep web UI responsive between fetches
    }

    if (anySuccess) {
        consecutiveFails = 0;
        renderAllCharts();
        Serial.println("Pushing to display...");
        updateDisplay(fullRefresh);
        Serial.println("Display updated!");
    } else {
        consecutiveFails++;
        Serial.printf("Fetch failed (%d consecutive), keeping previous frame\n", consecutiveFails);

        // After repeated failures, the WiFi stack is likely dead
        if (consecutiveFails >= MAX_CONSECUTIVE_FAILS) {
            Serial.println("Too many consecutive failures — forcing WiFi reset");
            epd.Sleep();
            forceWiFiReset();
            lastWifiCheckMs = millis();  // prevent keepalive re-triggering immediately
            return;  // skip display update, next cycle will retry
        }
    }

    epd.Sleep();
    Serial.println("e-Paper sleeping");
}

// ═══════════════════════════════════════════════════════
// SETUP & LOOP
// ═══════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Hyperliquid Candle Display ===");

    bootTime = millis();

    // Load persistent config
    loadConfig();

    // Init display first — we may need it for status screens
    Serial.println("Init e-Paper...");
    if (epd.Init() != 0) {
        Serial.println("e-Paper init FAILED");
        return;
    }
    Serial.println("e-Paper OK");

    // Show splash screen while connecting
    showSplash();

    // Attempt WiFi connection (falls back to AP if STA fails)
    connectWiFi();

    // Always start the web server (works on AP or STA)
    setupWebServer();

    if (WiFi.status() == WL_CONNECTED) {
        syncTime();

        bool anySuccess = false;
        int activeSlots = cfgLayout;
        for (int i = 0; i < activeSlots; i++) {
            if (fetchSlotCandles(i)) {
                anySuccess = true;
            }
            server.handleClient();
        }

        if (anySuccess) {
            renderAllCharts();
            Serial.println("First run — full refresh...");
            memset(oldbuf, 0xFF, BUF_SIZE);
            updateDisplay(true);
            Serial.println("Display updated!");
        } else {
            Serial.println("Fetch failed on first boot");
            renderStatusScreen("NO DATA", "API request failed", "Will retry next cycle", "", "");
            memset(oldbuf, 0xFF, BUF_SIZE);
            updateDisplay(true);
        }

        Serial.printf("Config URL: http://%s.local or http://%s\n",
                      MDNS_HOST, WiFi.localIP().toString().c_str());
    } else {
        // WiFi failed — show status screen with AP info
        Serial.println("Showing WiFi fail screen on e-ink");
        showWifiFailScreen();
        memset(oldbuf, 0xFF, BUF_SIZE);
        updateDisplay(true);
    }

    epd.Sleep();
    Serial.println("e-Paper sleeping");

    lastRefreshMs = millis();
    lastWifiRetryMs = millis();
    lastWifiCheckMs = millis();
    firstBoot = false;
}

void loop() {
    // Service web server continuously (works on AP or STA)
    server.handleClient();

    if (otaNeedsRender) {
        bool forceFull = !otaDisplayReady || otaFailed || otaProgressPct >= 100;
        updateOtaDisplay(forceFull);
    }

    if (otaActive) {
        delay(5);
        return;
    }

    // ── WiFi keepalive check every 15s ──
    if (!apModeActive && (millis() - lastWifiCheckMs >= WIFI_CHECK_MS)) {
        lastWifiCheckMs = millis();

        if (WiFi.status() != WL_CONNECTED) {
            // WiFi stack knows it's down — try soft reconnect
            Serial.println("WiFi dropped — attempting reconnect...");
            WiFi.reconnect();
            int attempts = 0;
            while (WiFi.status() != WL_CONNECTED && attempts < 10) {
                delay(500); attempts++;
            }
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
                consecutiveFails = 0;
            } else {
                Serial.println("Quick reconnect failed — will retry in 15s");
            }
        } else if (consecutiveFails >= MAX_CONSECUTIVE_FAILS) {
            // WiFi.status() says connected but fetches keep failing — ghost connection
            Serial.println("Ghost connection detected — WiFi reports OK but API unreachable");
            forceWiFiReset();
        }
    }

    // ── If in AP mode, periodically retry STA connection ──
    if (apModeActive && (millis() - lastWifiRetryMs >= WIFI_RETRY_MS)) {
        lastWifiRetryMs = millis();
        Serial.println("Retrying STA connection...");

        if (connectWiFiSTA()) {
            Serial.println("STA reconnected! Syncing time and refreshing...");
            syncTime();
            forceRefresh = true;  // trigger immediate chart refresh
        } else {
            Serial.println("STA retry failed, AP still active");
        }
    }

    // Remote OTA polling
    remoteOtaTick();

    // Normal refresh cycle — only when we have WiFi
    unsigned long interval = effectiveRefreshMs();
    if (WiFi.status() == WL_CONNECTED) {
        if (forceRefresh || (millis() - lastRefreshMs >= interval)) {
            if (!forceRefresh && isInQuietHours()) {
                // Bump timer and skip — check again next interval
                lastRefreshMs = millis();
                Serial.println("Quiet hours — skipping refresh");
            } else {
                bool doFull = forceRefresh;
                forceRefresh = false;
                lastRefreshMs = millis();
                Serial.println("Refresh cycle starting...");
                doRefreshCycle(doFull);
            }
        }
    } else if (!apModeActive) {
        // Lost WiFi mid-session and not in AP mode yet
        if (millis() - lastRefreshMs >= interval) {
            lastRefreshMs = millis();
            Serial.println("WiFi lost — starting AP and updating display");
            startAPMode();

            // Show status on e-ink
            if (epd.Init() == 0) {
                showWifiFailScreen();
                updateDisplay(true);
                epd.Sleep();
            }
        }
    }
}
