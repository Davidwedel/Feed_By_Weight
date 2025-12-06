# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based automated chicken feed control system. Monitors bin weights via BinTrac Modbus TCP and dispenses precise feed amounts using sequential auger control (chain pre-run, then both auger+chain).

## Build & Development Commands

```bash
# Build firmware
pio run

# Upload to ESP32
pio run -t upload

# Upload web interface to filesystem
pio run --target uploadfs

# Monitor serial output
pio device monitor

# Build and upload in one command
pio run -t upload && pio device monitor
```

## Network Configuration

The system supports **both** Ethernet (W5500) and WiFi. Toggle in `src/config.h`:

```cpp
// Comment/uncomment to switch:
#define USE_ETHERNET
// #define USE_WIFI
```

**WiFi mode:** Set WIFI_SSID and WIFI_PASSWORD in config.h
**Ethernet mode:** Uses DHCP, falls back to static IP 192.168.1.177

**Current configuration:** WiFi mode on "CW Wifi" network

## Architecture & State Machine

### Main State Machine (main.cpp)

The system operates as a state machine in `runStateMachine()`:

- **IDLE / WAITING_FOR_SCHEDULE**: Waiting for next scheduled feed time
- **FEEDING**: Active feeding in progress (calls AugerControl::update() every loop)
- **MANUAL_OVERRIDE**: Manual control active via web interface
- **ALARM**: Feeding failed, requires user intervention
- **ERROR**: Fatal system error

State transitions are driven by:
- Scheduler detecting feed time
- AugerControl returning COMPLETED or FAILED stage
- Manual control commands from web API

### Critical Component Interaction

**Feeding Sequence Flow:**
1. `Scheduler::shouldFeed()` checks if current time matches any of 4 daily feed times
2. `main.cpp` transitions to FEEDING state, calls `AugerControl::startFeeding()`
3. `AugerControl::update()` is called every loop with current total weight from BinTrac
4. AugerControl manages 2-stage sequence:
   - Stage 1: Chain runs alone for `chainPreRunTime` seconds
   - Stage 2: Both auger + chain run until target weight dispensed
5. Returns COMPLETED or FAILED stage to main state machine
6. Main creates FeedEvent record and saves via Storage

**Weight Monitoring:**
- BinTrac reads 4 bins (A, B, C, D) as 32-bit signed integers via Modbus TCP
- Address 1000, Function Code 4 (Read Input Registers), 8 registers total
- Value -32767 (0xFFFF8001) indicates bin disabled
- Total weight = sum of all enabled bins
- Weight dispensed = startWeight - currentWeight

**Alarm Detection (in AugerControl::update()):**
- Tracks weight change per minute
- Triggers if: weight/min < threshold, max runtime exceeded, weight increases, or no change for 30s

### Component Responsibilities

**AugerControl** (the most complex component):
- Manages 2-stage sequential feeding (chain pre-run, then both)
- Monitors weight change rate for alarm detection
- Tracks feeding progress (weightDispensed = startWeight - currentWeight)
- **Important:** Does NOT directly read BinTrac; main.cpp passes current weight to update()
- Returns FeedingStage to drive state machine transitions

**Scheduler**:
- NTP time synchronization
- Converts 4 feed times (minutes from midnight) to actual triggers
- Tracks which feeds completed today (resets at midnight)
- **Important:** Uses EthernetUDP for NTP even in WiFi mode (works with both)

**BinTrac**:
- Modbus TCP client to HouseLink HL-10E
- Reads all 4 bins in single transaction (8 registers)
- Auto-reconnect logic with timeout handling
- Returns weights as float array [A, B, C, D]

**Storage**:
- LittleFS persistence for Config and FeedEvent history
- JSON serialization using ArduinoJson
- Files: `/config.json`, `/history.csv`
- **Important:** History is CSV, not JSON despite using ArduinoJson for config

**Web Server**:
- ESPAsyncWebServer (switched from WebServer to fix WiFi hanging)
- Async handlers require different signature: `void handler(AsyncWebServerRequest *request)`
- POST body handlers use: `void handler(AsyncWebServerRequest *req, uint8_t *data, size_t len)`
- Serves `/index.html` from LittleFS (must upload via `pio run --target uploadfs`)

## Web Interface Deployment

The web UI exists at `data/index.html` but must be uploaded to ESP32 LittleFS:

```bash
pio run --target uploadfs
```

Without this, web browser shows "Web interface not installed" placeholder. The API endpoints work regardless.

## BinTrac Modbus Protocol

- **IP:** 192.168.1.100 (default, configurable via web)
- **Port:** 502
- **Function Code:** 4 (Read Input Registers)
- **Address:** 1000 (reads all 8 registers = 4 bins × 2 registers each)
- **Data Format:** Each bin is 32-bit signed integer, big-endian
- **Special Value:** -32767 = bin disabled

**Key Implementation Detail:** The modbus-esp8266 library is used even though this is ESP32. It works fine and handles the Modbus TCP protocol.

## Telegram Bot

Uses UniversalTelegramBot library with SSL:
- **WiFi mode:** Uses WiFiClientSecure with `setInsecure()`
- **Ethernet mode:** Uses SSLClient wrapper around EthernetClient
- Configure token and chatID via web interface
- Sends: alarm notifications, feeding complete, daily summaries
- Receives: /status, /enable, /disable commands

## Important Gotchas

1. **AsyncWebServer POST bodies:** Must use 3-argument lambda with body callback, not hasArg("plain")
2. **NTP uses EthernetUDP:** Works for both network modes, don't change to WiFiUDP
3. **Weight calculation direction:** Dispensed = start - current (bins get lighter as feed dispenses)
4. **Scheduler daily reset:** Feed completion flags reset at midnight, don't manually clear
5. **Manual control vs Auto feed:** Manual mode sets MANUAL_OVERRIDE state, preventing auto feeds
6. **LittleFS corruption:** If "Corrupted dir pair" error appears, filesystem needs format (Storage::formatFilesystem())

## System Constants (src/config.h)

Key timing values that affect behavior:
- `WEIGHT_CHECK_INTERVAL`: 1000ms (how often to check bin weight during feeding)
- `BINTRAC_TIMEOUT`: 5000ms (Modbus TCP timeout)
- `NTP_UPDATE_INTERVAL`: 3600000ms (sync time every hour)
- `STATUS_UPDATE_INTERVAL`: 5000ms (web status refresh rate)
- `TELEGRAM_UPDATE_INTERVAL`: 60000ms (check for Telegram messages)

## Typical Modification Patterns

**Adding a new relay:**
1. Define pin in config.h
2. Add pinMode/digitalWrite in AugerControl::begin()
3. Add control method in AugerControl class
4. Add API endpoint in web_server.cpp
5. Add button in data/index.html

**Changing feeding logic:**
- Modify AugerControl::update() state machine
- Stages are: STOPPED → CHAIN_ONLY → BOTH_RUNNING → COMPLETED/FAILED
- Return value drives main.cpp state transitions

**Adding new API endpoint:**
1. Add handler method in web_server.h
2. Implement in web_server.cpp (use async signature)
3. Register in FeedWebServer::begin() with `_server->on()`
4. Update data/index.html JavaScript to call it
