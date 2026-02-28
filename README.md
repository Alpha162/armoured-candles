# Armoured Bird EPD Display

A self-refreshing cryptocurrency candlestick chart on a Waveshare 7.5" e-ink display, driven by a Seeed XIAO ESP32-S3. Pulls live OHLCV data from the Hyperliquid DEX API and renders candles, EMA/RSI indicators, and volume bars to a 800×480 1-bit framebuffer.

Fully configurable via a built-in web UI — no reflashing needed to change pairs, timeframes, or indicators.

![Splash Screen](docs/splash_preview.png)

## Features

- **Live candlestick charts** from Hyperliquid's REST API (any listed pair)
- **Technical indicators**: EMA 9/21 (configurable), RSI 14 (configurable), volume bars
- **Web config UI** at `http://epdchart.local` — change coin, interval, indicators, refresh rate
- **Persistent settings** via ESP32 NVS flash — survives reboots
- **Auto candle count** — scales intelligently per timeframe for optimal display density
- **Smart display refresh** — partial updates when possible, full refresh to clear ghosting
- **WiFi resilience** — auto-reconnect, ghost connection detection, AP fallback mode
- **Boot splash screen** with dithered logo

## Hardware

| Component | Model |
|-----------|-------|
| MCU | Seeed XIAO ESP32-S3 |
| Display | Waveshare 7.5" V2 (800×480 B/W) — GDEY075T7 |
| Driver | Waveshare e-Paper Driver HAT Rev 2.3 |
| Housing | Seeed Studio e-ink frame/case |
| Power | USB 5V (powerbank recommended) |

## Wiring

> **CRITICAL: Both VCC and PWR must be on 3.3V.** The Rev 2.3 HAT level shifters need VCC to match MCU logic level. 5V on VCC causes SPI data corruption ("snow").

| HAT Pin | XIAO S3 Pin | GPIO |
|---------|-------------|------|
| PWR | 3V3 | — |
| VCC | 3V3 | — |
| GND | GND | — |
| CLK | D8 | GPIO7 (SCK) |
| DIN | D10 | GPIO9 (MOSI) |
| CS | D1 | GPIO2 |
| DC | D2 | GPIO3 |
| RST | D3 | GPIO4 |
| BUSY | D4 | GPIO5 |

**HAT config switches:** Display B, Interface 0 (4-wire SPI)

## Arduino IDE Setup

1. **Board package:** ESP32 by Espressif (3.x+)
2. **Board:** XIAO_ESP32S3
3. **PSRAM:** OPI PSRAM *(must be enabled)*
4. **Library:** ArduinoJson by Benoit Blanchon (Library Manager)

## First Boot

1. Flash the sketch
2. The splash screen appears while WiFi connects
3. If your WiFi credentials aren't set (or wrong), the device creates an AP: **EPDChart-Setup**
4. Connect to the AP, navigate to `http://192.168.4.1`
5. Enter your WiFi SSID/password, configure your preferred coin and timeframe
6. Save and reboot — the chart will appear on next cycle

## Web UI

Access at `http://epdchart.local` or the device IP (shown on the e-ink display footer).

**Chart settings:** coin pair, candle interval, auto/manual candle count, refresh rate

**Indicators:** EMA fast/slow periods, RSI period

**Display:** full refresh frequency, partial refresh threshold, timezone

**WiFi:** SSID and password (changes take effect after reboot)

**Actions:** force display refresh, reboot device

## Configuration

All settings persist in NVS flash. Defaults on first boot:

| Setting | Default | Range |
|---------|---------|-------|
| Coin | ETH | Any Hyperliquid pair |
| Interval | 5m | 1m–1M |
| Auto candles | On | — |
| Refresh | 5 min | 1–60 min |
| EMA fast | 9 | 2–100 |
| EMA slow | 21 | 2–200 |
| RSI period | 14 | 2–100 |
| Full refresh every | 10 cycles | 1–50 |
| Partial threshold | 40% | 10–100% |

### Auto Candle Count

When enabled, the candle count scales to the interval for optimal display density:

| Interval | Candles | Window |
|----------|---------|--------|
| 1m | 120 | 2h |
| 5m | 60 | 5h |
| 15m | 48 | 12h |
| 1h | 48 | 2d |
| 4h | 42 | 7d |
| 1d | 30 | 30d |
| 1w | 26 | 6mo |

## WiFi Resilience

The firmware handles network issues at multiple levels:

- **Auto-reconnect:** ESP32 WiFi stack handles transient drops
- **Keepalive monitor:** checks connection health every 15 seconds
- **Ghost connection detection:** after 3 consecutive API failures (WiFi reports OK but TCP is dead), forces full radio teardown and cold reconnect
- **AP fallback:** if all else fails, creates `EPDChart-Setup` AP so you can access the web UI and fix credentials
- **Status screen:** e-ink displays connection status and AP info when WiFi is down

## API

Uses Hyperliquid's public REST API (no authentication required):

- **Endpoint:** `POST https://api.hyperliquid.xyz/info`
- **Intervals:** 1m, 3m, 5m, 15m, 30m, 1h, 2h, 4h, 8h, 12h, 1d, 3d, 1w, 1M

## Project Structure

```
epd_candles/
├── epd_candles.ino      # Main sketch: config, WiFi, API, chart rendering, web UI
├── epd7in5_V2.cpp/.h    # Display driver (full + partial refresh)
├── epdif.cpp/.h         # SPI interface layer
└── splash_image.h       # Boot splash (auto-generated 1-bit image header)
```

## Known Issues

- **Rev 2.3 RST pin** only controls reset, not power. Keep reset pulse short (2ms).
- **`unsigned long` is 32-bit on ESP32** — all timestamps use `uint64_t` to avoid overflow.
- **PSRAM must be enabled** or large JSON payloads cause OOM.
- **Partial refresh ghosting** accumulates — auto full-refresh handles this.
- **mDNS** may not work on all networks/clients. Use IP address as fallback.

## License

MIT

## Credits

Built with unreasonable amounts of caffeine and an unhealthy obsession with e-ink displays.
