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
#include <ArduinoJson.h>
#include <SPI.h>
#include <time.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ctype.h>
#include "epd7in5_V2.h"
#include "splash_image.h"

// ─── COMPILE-TIME CONSTANTS ────────────────────────────
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
};

// ─── RUNTIME CONFIG (loaded from NVS) ──────────────────
Preferences prefs;

char     cfgSSID[64]      = "YOUR_SSID";
char     cfgPass[64]      = "YOUR_PASSWORD";
int      cfgLayout        = 1;        // 1, 2, 3, or 4
int      cfgRefreshMin    = 5;
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

// ─── OBJECTS ────────────────────────────────────────────
Epd       epd;
WebServer server(80);

// ─── FORWARD DECLARATIONS ──────────────────────────────
void startAPMode();
void setupWebServer();
void renderOtaProgressScreen(int pct, bool failed);
void updateOtaDisplay(bool forceFullRefresh);
bool authenticateRequest();

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
                 strcmp(s, "poloniex") == 0);
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

    // Initialize all slots with defaults
    for (int i = 0; i < MAX_SLOTS; i++) {
        initSlotDefaults(slots[i].cfg);
        slots[i].candleCount = 0;
        slots[i].lastPrice = 0;
        slots[i].lastPctChange = 0;
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
    prefs.putInt("tzOffset", cfgTzOffset);
    prefs.putInt("fullRefEv", cfgFullRefEvery);
    prefs.putInt("partialPct", cfgPartialPct);
    prefs.putInt("layout", cfgLayout);

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
    background:linear-gradient(135deg,#0d1520 0%,#111d2b 100%);
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
    background:var(--surface);color:var(--text-dim);white-space:nowrap;
  }
  .pill.live{border-color:var(--green);color:var(--green);background:var(--green-dim)}
  .pill.warn{border-color:var(--amber);color:var(--amber)}

  .container{max-width:640px;margin:0 auto;padding:16px 16px 100px}

  .card{
    background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);
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
    width:100%;padding:9px 12px;background:var(--bg);border:1px solid var(--border);
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
    color:var(--accent);padding:8px 12px;background:var(--accent-dim);
    border-radius:6px;margin-top:8px;border:1px solid #1e4a5e;
  }

  .actions{
    position:fixed;bottom:0;left:0;right:0;
    background:linear-gradient(transparent,var(--bg) 20%);padding:12px 16px 20px;
    display:flex;gap:8px;justify-content:center;z-index:10;
  }
  .btn{
    font-family:'DM Sans',sans-serif;font-weight:600;font-size:0.82em;
    padding:10px 20px;border-radius:8px;border:1px solid var(--border);
    cursor:pointer;transition:all .2s;color:var(--text);background:var(--surface2);
    display:flex;align-items:center;gap:6px;
  }
  .btn:hover{border-color:var(--border-h);background:var(--border)}
  .btn.primary{background:var(--accent-dim);border-color:var(--accent);color:var(--accent)}
  .btn.primary:hover{background:#1e5a6e}
  .btn.danger{border-color:var(--red);color:var(--red);background:var(--red-dim)}
  .btn.danger:hover{background:#4a1a1a}

  .toast{
    position:fixed;top:20px;left:50%;transform:translateX(-50%) translateY(-80px);
    font-family:'JetBrains Mono',monospace;font-size:0.8em;
    padding:10px 20px;border-radius:8px;z-index:100;transition:transform .4s ease;
    border:1px solid var(--green);background:var(--green-dim);color:var(--green);
    pointer-events:none;
  }
  .toast.error{border-color:var(--red);background:var(--red-dim);color:var(--red)}
  .toast.show{transform:translateX(-50%) translateY(0)}

  .coin-wrap{position:relative}
  .coin-wrap input[type=text]{padding-right:30px}
  .coin-dd{
    display:none;position:absolute;top:100%;left:0;right:0;z-index:20;
    max-height:220px;overflow-y:auto;
    background:var(--bg);border:1px solid var(--accent);border-top:0;border-radius:0 0 6px 6px;
  }
  .coin-dd.open{display:block}
  .coin-dd .coin-item{
    padding:7px 12px;font-family:'JetBrains Mono',monospace;font-size:0.82em;
    color:var(--text);cursor:pointer;
  }
  .coin-dd .coin-item:hover,.coin-dd .coin-item.hl{background:var(--accent-dim);color:var(--accent)}
  .coin-dd .coin-empty{padding:10px 12px;font-size:0.78em;color:var(--text-dim);font-style:italic}
  .coin-loading{padding:10px 12px;font-size:0.78em;color:var(--accent);font-family:'JetBrains Mono',monospace}

  .layout-picker{display:flex;gap:8px}
  .layout-opt{flex:1;cursor:pointer}
  .layout-opt input[type=radio]{display:none}
  .layout-thumb{
    display:flex;flex-direction:column;align-items:center;gap:6px;
    padding:10px 8px;border:1px solid var(--border);border-radius:8px;
    background:var(--bg);transition:all .2s;
  }
  .layout-opt input:checked + .layout-thumb{
    border-color:var(--accent);background:var(--accent-dim);
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
</style>
</head>
<body>

<div class="header">
  <h1><span>&#9632;</span> EPD Chart</h1>
  <div class="status-pills">
    <div class="pill live" id="pillWifi">WiFi</div>
    <div class="pill" id="pillIp">--</div>
    <div class="pill" id="pillPrice">--</div>
    <div class="pill" id="pillUptime">--</div>
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
      <div class="field" style="margin-top:14px">
        <label>Refresh every (minutes)</label>
        <input type="number" id="refreshMin" min="1" max="60" value="5">
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
  poloniex:['1m','5m','15m','30m','1h','2h','4h','6h','12h','1d','3d','1w','1M']
};
const QUOTE_DEFAULTS = {
  hyperliquid:['USDC'],binance:['USDT','BUSD','USDC'],asterdex:['USDT'],
  kraken:['USD','USDT','EUR'],poloniex:['USDT','USDC']
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
      '<div class="card"><div class="card-head"><span class="icon">&#9670;</span><h2>Chart '+(i+1)+'</h2></div>' +
      '<div class="card-body">' +
      '<div class="field"><label>Exchange</label>' +
      '<select id="exchange_'+i+'">' +
      '<option value="hyperliquid">Hyperliquid</option>' +
      '<option value="binance">Binance</option>' +
      '<option value="asterdex">AsterDEX</option>' +
      '<option value="kraken">Kraken</option>' +
      '<option value="poloniex">Poloniex</option></select></div>' +
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
      el('interval_'+idx).addEventListener('change', function(){ updateSlotAutoInfo(idx); });
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
      var r = await fetch('https://api.poloniex.com/markets');
      var d = await r.json();
      list = d.filter(function(m){return m.symbol&&m.symbol.endsWith('_'+quote);}).map(function(m){return m.symbol.split('_')[0];});
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
}

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
    el('tzOffset').value = String(d.tzOffset || 0);
    el('fullRefEvery').value = d.fullRefEvery || 10;
    el('partialPct').value = d.partialPct || 40;
    el('ssid').value = d.ssid || '';
    el('uiUser').value = d.uiUser || '';
    el('uiPass').value = '';
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
      updateSlotAutoInfo(i);
    }

    // Status pills
    el('pillIp').textContent = d.apMode ? ('AP: ' + d.apIP) : d.ip;
    if (d.apMode) { el('pillWifi').textContent='AP Mode'; el('pillWifi').className='pill warn'; }
    else if (d.rssi) { el('pillWifi').textContent='WiFi '+d.rssi+'dBm'; el('pillWifi').className='pill live'; }

    // Price pill from slot 0
    var s0 = slots[0] || {};
    if (s0.price > 0) {
      var sign = s0.pctChange >= 0 ? '+' : '';
      el('pillPrice').textContent = s0.coin+' $'+s0.price.toFixed(2)+' '+sign+s0.pctChange.toFixed(2)+'%';
      el('pillPrice').className = 'pill ' + (s0.pctChange >= 0 ? 'live' : 'warn');
    }
    if (d.uptime) el('pillUptime').textContent = d.uptime;
    if (d.heap) el('pillUptime').textContent += ' | '+Math.round(d.heap/1024)+'KB';
    if (d.fails > 0) { el('pillUptime').textContent += ' | '+d.fails+' fails'; el('pillUptime').className='pill warn'; }
    else el('pillUptime').className='pill';

    if (!configLoaded) {
      configLoaded = true;
      for (var i=0;i<MAX_SLOTS;i++) await fetchPairsForSlot(i, el('exchange_'+i).value);
    }
  } catch(e) { console.error(e); }
}

async function saveConfig() {
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
      heikinAshi: el('heikinAshi_'+i).checked
    });
  }
  var body = {
    layout: currentLayout,
    slots: slotsArr,
    refreshMin: parseInt(el('refreshMin').value) || 5,
    tzOffset: parseInt(el('tzOffset').value) || 0,
    fullRefEvery: parseInt(el('fullRefEvery').value) || 10,
    partialPct: parseInt(el('partialPct').value) || 40,
    ssid: el('ssid').value,
    pass: el('wifipass').value,
    uiUser: el('uiUser').value.trim(),
    uiPass: el('uiPass').value
  };
  setAuthHeader();
  try {
    var r = await authFetch('/api/config', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
    if (r.ok) toast('Config saved'); else toast('Save failed', true);
  } catch(e) { toast('Connection error', true); }
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

    // Global settings
    doc["layout"] = cfgLayout;
    doc["refreshMin"] = cfgRefreshMin;
    doc["tzOffset"] = cfgTzOffset;
    doc["fullRefEvery"] = cfgFullRefEvery;
    doc["partialPct"] = cfgPartialPct;

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
    doc["heap"] = ESP.getFreeHeap();

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
        s["price"] = slots[i].lastPrice;
        s["pctChange"] = slots[i].lastPctChange;
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
    if (doc.containsKey("refreshMin"))  cfgRefreshMin   = constrain(doc["refreshMin"].as<int>(), 1, 60);
    if (doc.containsKey("tzOffset"))    cfgTzOffset     = doc["tzOffset"].as<int>();
    if (doc.containsKey("fullRefEvery"))cfgFullRefEvery = constrain(doc["fullRefEvery"].as<int>(), 1, 50);
    if (doc.containsKey("partialPct"))  cfgPartialPct   = constrain(doc["partialPct"].as<int>(), 10, 100);

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


void setupWebServer() {
    server.on("/",          HTTP_GET,  handleRoot);
    server.on("/api/status",HTTP_GET,  handleStatus);
    server.on("/api/config",HTTP_POST, handleConfigPost);
    server.on("/api/refresh",HTTP_POST,handleRefresh);
    server.on("/api/restart",HTTP_POST,handleRestart);
    server.on("/api/display",HTTP_GET, handleDisplayBMP);
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

    for (int i = skip; i < total && candleCount < numCandles; i++) {
        JsonObject c = arr[i];
        candles[candleCount].o = atof(c["open"].as<const char*>());
        candles[candleCount].h = atof(c["high"].as<const char*>());
        candles[candleCount].l = atof(c["low"].as<const char*>());
        candles[candleCount].c = atof(c["close"].as<const char*>());
        candles[candleCount].v = atof(c["quantity"].as<const char*>());
        candles[candleCount].t = c["startTime"].as<uint64_t>();
        candleCount++;
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

    if (ok) {
        // Copy results from scratch globals into slot
        memcpy(slots[slotIdx].candles, candles, candleCount * sizeof(Candle));
        memcpy(slots[slotIdx].emaFastArr, emaFast, candleCount * sizeof(float));
        memcpy(slots[slotIdx].emaSlowArr, emaSlow, candleCount * sizeof(float));
        memcpy(slots[slotIdx].rsiVal, rsiVal, candleCount * sizeof(float));
        slots[slotIdx].candleCount = candleCount;
        slots[slotIdx].lastPrice = lastPrice;
        slots[slotIdx].lastPctChange = lastPctChange;
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

void renderSlotChart(const Viewport& vp, ChartSlot& slot) {
    const SlotConfig& sc = slot.cfg;
    bool isFullScreen = (vp.w >= 700 && vp.h >= 400);
    bool isHalf = (vp.w >= 700 && !isFullScreen);

    // Adaptive layout values based on viewport size
    int mT = isFullScreen ? 35 : 18;
    int mB = isFullScreen ? 25 : 12;
    int mL = (vp.w >= 600) ? 10 : 6;
    int mR = isFullScreen ? 85 : (vp.w >= 500 ? 55 : 45);
    int rH = isFullScreen ? 45 : 25;
    int vH = isFullScreen ? 30 : 18;
    int gp = isFullScreen ? 5 : 3;
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
    drawString(vp.x + mL, vp.y + (isFullScreen ? 5 : 3), title, titleScale);

    // Legend line (EMA/RSI info) — only if there's room
    if (isFullScreen) {
        char legend[48];
        snprintf(legend, sizeof(legend), "EMA%d/EMA%d  RSI%d:%.0f",
                 sc.emaFast, sc.emaSlow, sc.rsiPeriod, slot.rsiVal[slot.candleCount - 1]);
        drawString(vp.x + mL + strlen(title) * 12 + 20, vp.y + 12, legend, 1);
    }

    // Price + pct on the right
    char priceStr[32];
    snprintf(priceStr, sizeof(priceStr), "%.2f %c%.1f%%", slot.lastPrice, pctSign, absPct);
    drawStringR(vp.x + vp.w - 4, vp.y + (isFullScreen ? 3 : 3), priceStr, priceScale);

    // IP address — only in full-screen single chart mode
    if (isFullScreen) {
        char ipStr[32];
        snprintf(ipStr, sizeof(ipStr), "%s", WiFi.localIP().toString().c_str());
        drawStringR(vp.x + vp.w - 10, vp.y + 22, ipStr, 1);
    }

    hLine(vp.x, vp.x + vp.w - 1, vp.y + mT - 2);

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

// ── Layout orchestrator ──
void renderAllCharts() {
    bufClear();

    int activeSlots = cfgLayout;
    Viewport vps[MAX_SLOTS];

    if (cfgLayout == 1) {
        vps[0] = {0, 0, 800, 480};
    } else if (cfgLayout == 2) {
        vps[0] = {0, 0, 800, 238};
        vps[1] = {0, 242, 800, 238};
    } else {
        // 3 or 4: 2x2 grid
        vps[0] = {0, 0, 398, 238};
        vps[1] = {402, 0, 398, 238};
        vps[2] = {0, 242, 398, 238};
        if (cfgLayout == 4) {
            vps[3] = {402, 242, 398, 238};
        }
    }

    for (int i = 0; i < activeSlots; i++) {
        renderSlotChart(vps[i], slots[i]);
    }

    // Draw divider lines
    if (cfgLayout >= 2) {
        hLine(0, 799, 239);
        hLine(0, 799, 240);
    }
    if (cfgLayout >= 3) {
        vLine(399, 0, 479);
        vLine(400, 0, 479);
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
    // Copy splash from PROGMEM into framebuffer
    memcpy_P(framebuf, splash_image, BUF_SIZE);
    memset(oldbuf, 0xFF, BUF_SIZE);
    epd.DisplayFrame(framebuf);
    memcpy(oldbuf, framebuf, BUF_SIZE);
    Serial.println("Splash screen displayed");
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

    // Normal refresh cycle — only when we have WiFi
    unsigned long interval = (unsigned long)cfgRefreshMin * 60UL * 1000UL;
    if (WiFi.status() == WL_CONNECTED) {
        if (forceRefresh || (millis() - lastRefreshMs >= interval)) {
            bool doFull = forceRefresh;
            forceRefresh = false;
            lastRefreshMs = millis();

            Serial.println("Refresh cycle starting...");
            doRefreshCycle(doFull);
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
