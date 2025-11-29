# Weight Feeder Control System

Automated feed control system using ESP32 with W5500 Ethernet, BinTrac weight monitoring, and dual auger control.

## Hardware Requirements

- **LilyGo ESP32 8-Channel Relay Board** with W5500 Ethernet Shield
- **BinTrac HouseLink HL-10E** Ethernet interface
- **BinTrac Indicators** (up to 8 supported, monitors 4 bins each)
- **Two Augers:**
  - Auger 1: Main feed auger (Relay 1)
  - Auger 2: Pre-feed auger (Relay 2)
- **12V Power Supply** for system

## Features

### Core Functionality
- ✅ Reads weight data from BinTrac via Modbus TCP (all 4 bins: A, B, C, D)
- ✅ Sequential auger control (Auger 2 pre-runs, then both run until target weight)
- ✅ 4 daily feeding schedules (configurable times)
- ✅ Alarm system for low feed rate detection
- ✅ Web-based configuration interface
- ✅ Telegram bot notifications
- ✅ Feed history logging

### Web Interface
Access at `http://<ESP32-IP>/`

**Features:**
- Real-time status monitoring (system state, bin weights, auger status)
- Manual auger controls (test/override)
- Configuration editor (feed times, weights, thresholds)
- Feed history viewer
- All settings persist to flash memory

### Telegram Bot
Receives notifications for:
- ⚠️ Alarms (low feed rate, timeout, errors)
- ✅ Feeding completion
- 📊 Daily summaries

**Commands:**
- `/start` - Show available commands
- `/status` - Get current system status
- `/disable` - Disable auto-feeding
- `/enable` - Enable auto-feeding

## Getting Started

### 1. Hardware Connections

**BinTrac HouseLink HL-10E:**
```
HL-10E +COM (IN) → BinTrac Indicator +SIG (COMM Port)
HL-10E -COM (IN) → BinTrac Indicator -SIG (COMM Port)
HL-10E +12V (IN) → Power Supply +12V
HL-10E -12V (IN) → Power Supply -12V
HL-10E Ethernet  → Network Switch/Router
```

**ESP32 Relay Board:**
```
Relay 1 → Auger 1 (Main)
Relay 2 → Auger 2 (Pre-feed)
W5500 Ethernet → Network Switch/Router (same network as HouseLink)
```

### 2. Software Setup

**Install PlatformIO:**
```bash
# Using VSCode
# Install PlatformIO IDE extension

# Or command line
pip install platformio
```

**Build and Upload:**
```bash
cd weightfeeder
pio run --target upload
```

**Upload Filesystem (Web UI):**
```bash
pio run --target uploadfs
```

### 3. Initial Configuration

1. Connect to the ESP32's network (check Serial Monitor for IP address)
2. Open web browser to `http://<ESP32-IP>/`
3. Configure:
   - BinTrac IP address
   - BinTrac Device ID (found on BinTrac Indicator, or use 0 for auto-discover)
   - Feed times (4 daily times in HH:MM format)
   - Target weight per feeding
   - Auger 2 pre-run time (how long it runs alone before Auger 1 starts)
   - Alarm threshold (minimum lbs/minute)
   - Maximum runtime (safety limit)
   - Timezone offset (UTC±hours)

4. (Optional) Configure Telegram:
   - Create bot via [@BotFather](https://t.me/botfather)
   - Get bot token
   - Get your chat ID (send message to bot, then check `/getUpdates`)
   - Enter both in config and enable Telegram

## Configuration Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| **bintracIP** | BinTrac HouseLink IP address | 192.168.1.100 |
| **bintracDeviceID** | Device ID (0=auto) | 0 |
| **feedTimes[4]** | Minutes from midnight for each feed | 360, 720, 1080, 1440 (6am, 12pm, 6pm, 12am) |
| **targetWeight** | Target weight to dispense (lbs) | 50.0 |
| **auger2PreRunTime** | Auger 2 solo run time (seconds) | 10 |
| **alarmThreshold** | Min lbs/minute (alarm if below) | 10.0 |
| **maxRuntime** | Maximum feeding time (seconds) | 600 |
| **timezone** | UTC offset in hours | 0 |

## Operating Sequence

### Automatic Feeding Cycle
1. **Wait for scheduled time** (one of 4 daily times)
2. **Read initial bin weight** from BinTrac
3. **Stage 1:** Auger 2 runs alone for configured pre-run time
4. **Stage 2:** Both augers run together
5. **Monitor weight:** Check every second, calculate dispensed amount
6. **Stop when target reached** or alarm condition detected
7. **Log event** to history
8. **Send notifications** via Telegram (if enabled)

### Alarm Conditions
- Weight dispensed per minute falls below threshold
- Maximum runtime exceeded
- No weight change detected after 30 seconds
- Weight increases during feeding (bin filling error)

### Safety Features
- All relays OFF on boot
- Watchdog timer
- Network connection monitoring
- BinTrac communication timeout handling
- Emergency stop button (web interface)

## API Endpoints

### GET /api/status
Returns current system status (JSON)

### GET /api/config
Returns current configuration (JSON)

### POST /api/config
Update configuration (JSON body)

### GET /api/history
Returns feed event history (JSON)

### DELETE /api/history
Clear all feed history

### POST /api/manual
Manual auger control
```json
{"action": "auger1_on|auger1_off|auger2_on|auger2_off|stop_all"}
```

### POST /api/feed/start
Start manual feeding cycle

### POST /api/feed/stop
Emergency stop all augers

## File Structure

```
weightfeeder/
├── src/
│   ├── main.cpp              # Main program with state machine
│   ├── config.h              # Pin definitions and constants
│   ├── types.h               # Data structures
│   ├── bintrac.cpp/h         # Modbus TCP communication
│   ├── auger_control.cpp/h   # Dual auger sequencing logic
│   ├── scheduler.cpp/h       # NTP time sync and scheduling
│   ├── web_server.cpp/h      # HTTP server and API
│   ├── telegram_bot.cpp/h    # Telegram notifications
│   └── storage.cpp/h         # Config and history persistence
├── data/
│   └── index.html            # Web user interface
├── platformio.ini            # Build configuration
└── README.md                 # This file
```

## BinTrac Modbus Details

**Protocol:** Modbus TCP on port 502
**Function Code:** 4 (Read Input Registers)

**Addresses:**
- 1000: All bins (8 registers = 4 bins × 2 registers each)
- 1000: Bin A (2 registers)
- 1002: Bin B (2 registers)
- 1004: Bin C (2 registers)
- 1006: Bin D (2 registers)

**Data Format:**
- Each weight is 32-bit signed integer (big-endian)
- Value of -32767 (0xFFFF8001) indicates bin disabled
- Read example in manual page 12

## Troubleshooting

**BinTrac not connecting:**
- Verify HouseLink IP address
- Check both devices on same network
- Ensure BinTrac Device ID is set correctly (or use 0 for auto-discover)
- Check HouseLink manual for Modbus TCP setup

**Time not syncing:**
- Verify internet connection
- Check NTP server accessibility
- Adjust timezone offset in config

**Feeding not starting:**
- Check "Auto Feed Enabled" setting
- Verify time is synchronized
- Check scheduled times are in future
- Review last error in status page

**Alarms triggering:**
- Verify alarm threshold isn't too high
- Check bins aren't empty
- Inspect augers for jams
- Review feed history for patterns

## Development

**Monitor Serial Output:**
```bash
pio device monitor
```

**Build Flags:**
```ini
ETH_PHY_TYPE=ETH_PHY_W5500
ETH_PHY_ADDR=1
```

## License

Copyright © 2025. All rights reserved.

## Support

For issues or questions, refer to:
- BinTrac Manual: `HouseLink-HL-10E.pdf`
- Serial monitor for debug output
- Web interface status page
