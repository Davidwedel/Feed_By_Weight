# Feed By Weight — Complete Program Flow

---

## 1. Hardware Overview

```
┌─────────────────────────────────────────────────────────┐
│                    ESP32 (LilyGo T-Relay)               │
│                                                         │
│  ┌──────────────┐    ┌──────────────────────────────┐   │
│  │  W5500 chip  │    │       8-channel Relay Board   │   │
│  │  (Ethernet)  │    │                               │   │
│  │  SPI pins:   │    │  K1 (GPIO33) ── Auger motor  │   │
│  │  CS=27       │    │  K2 (GPIO32) ── Chain A       │   │
│  │  MISO=34     │    │  K3 (GPIO13) ── Chain B       │   │
│  │  MOSI=26     │    │  K4 (GPIO12) ── Chain C       │   │
│  │  SCK=22      │    │  K5 (GPIO21) ── Chain D       │   │
│  │  RST=23      │    │  K6 (GPIO19) ── Auger 2       │   │
│  └──────┬───────┘    │  K7 (GPIO18) ── (unused)      │   │
│         │            │  K8 (GPIO5)  ── (unused)      │   │
│         │            └──────────────────────────────┘   │
│         │                                               │
│  GPIO2 ─── Status LED                                   │
└─────────┼───────────────────────────────────────────────┘
          │
          │ Ethernet (TCP/IP)
          ▼
   ┌──────────────┐         ┌──────────────────────┐
   │  BinTrac     │         │   NTP Server         │
   │  HouseLink   │         │  pool.ntp.org :123   │
   │  HL-10E      │         │  (time sync via UDP) │
   │  :502 Modbus │         └──────────────────────┘
   │              │
   │  Bin A ──── weight sensor
   │  Bin B ──── weight sensor
   │  Bin C ──── weight sensor
   │  Bin D ──── weight sensor
   └──────────────┘
```

---

## 2. Boot / `setup()` Sequence

```
setup() called
    │
    ├─ Serial.begin(115200)
    ├─ STATUS_LED_PIN → OUTPUT, LOW
    │
    ├─ storage.begin()          ← LittleFS mount
    │      │ fail → blink LED forever (FATAL)
    │      └ ok
    ├─ storage.loadConfig()     ← /config.json  (uses defaults if missing)
    │
    ├─ storage.loadFeedProgress()
    │      └ if interrupted feed found from last boot:
    │            create FeedEvent with alarmTriggered=true
    │            reason = "Interrupted by reboot"
    │            save to history, clear progress
    │
    ├─ setupNetwork()
    │      ├─ hardware reset W5500 via RST pin
    │      ├─ SPI.begin(SCK, MISO, MOSI, CS)
    │      ├─ Ethernet.begin(mac)  ← try DHCP first
    │      ├─ if DHCP got private IP:
    │      │      use same subnet but force .205 as last octet
    │      │      Ethernet.begin(mac, staticIP, ...)
    │      └─ else: fallback static 192.168.1.205
    │
    ├─ augerControl.begin()     ← pinMode all 6 relay pins, stopAll()
    ├─ bintrac.begin(ip, 502)   ← attempt TCP connect + test read
    ├─ scheduler.begin(timezone)
    │
    ├─ delay(3000)              ← let network settle
    ├─ scheduler.startNTPSync() ← UDP to pool.ntp.org, settimeofday()
    │
    ├─ new FeedWebServer(...)   ← ESPAsyncWebServer on port 80
    ├─ webServer->begin()       ← register all API endpoints
    │
    ├─ WiFi.begin(ssid, pass)   ← optional, only for Telegram SSL
    ├─ new TelegramBot(config)
    ├─ telegramBot->begin()
    │
    ├─ systemStatus.state = IDLE
    ├─ calculateTotalDispensedToday()   ← read history CSV, sum today's feeds
    └─ STATUS_LED_PIN → HIGH   (boot complete indicator)
```

---

## 3. Main Loop Architecture

Every ~10ms, `loop()` runs through this sequence:

```
loop() ─────────────────────────────────────────────────────
         │
         ├─[every call]─ scheduler.update()
         │                    └─ checkDayRollover() → reset _feedingCompleted[4]
         │
         ├─[every call]─ telegramBot->update()
         │                    ├─ poll Telegram API for new messages
         │                    ├─ /status   → sendStatus()
         │                    ├─ /disable  → augerControl.stopAll()
         │                    ├─ /clearalarm → state = IDLE
         │                    ├─ /logoldfeed → storage.addFeedEvent()
         │                    └─ /startfeed → augerControl.startFeeding()
         │
         ├─[every call]─ webServer->handleClient()
         │                    └─ ESPAsyncWebServer processes HTTP requests
         │
         ├─[every 3000ms]─ updateBinWeights()
         │                    ├─ bintrac.readAllBins(rawWeights[4])
         │                    ├─ apply EMA smoothing:
         │                    │      new = 0.3*raw + 0.7*old
         │                    ├─ weightLog.add(millis, weights, total)
         │                    └─ if fail for >30s → bintrac.reconnect()
         │
         ├─[every 3000ms]─ updateSystemStatus()
         │                    ├─ copy augerControl state to systemStatus
         │                    ├─ check day rollover → reset totalDispensedToday
         │                    ├─ add in-progress weight to totalDispensedToday
         │                    └─ check Ethernet IP for networkConnected
         │
         ├─[every call]─ runStateMachine()   ← THE CORE (see below)
         │
         └─ delay(10)
```

---

## 4. Main State Machine

```
                    ┌──────────────────────────────────────┐
                    │         System State Machine         │
                    └──────────────────────────────────────┘

   ┌─────────┐  autoFeedEnabled &&    ┌─────────────────┐
   │  IDLE   │  time synced &&        │                 │
   │  or     ├──scheduler.shouldFeed──►    FEEDING      │
   │WAITING_ │                        │                 │
   │SCHEDULE │◄───────────────────────┤  (see feeding   │
   └────┬────┘  COMPLETED/TERMINATED  │   flow below)   │
        │                             └────────┬────────┘
        │  web API /manual            FAILED   │
        │  (setAuger/setChain)                 │
        ▼                                      ▼
   ┌──────────────┐               ┌────────────────────┐
   │   MANUAL_    │               │       ALARM        │
   │   OVERRIDE   │               │                    │
   │              │               │ Requires human     │
   │ auger/chain  │               │ intervention:      │
   │ under direct │               │  - web /clearalarm │
   │ web control  │               │  - Telegram        │
   └──────┬───────┘               │  /clearalarm       │
          │                       └────────────────────┘
          │ isFeeding() == false
          ▼
       IDLE ◄────────────────────────────────────────────


   ┌──────┐
   │ERROR │  ← only on storage init failure (halts entirely)
   └──────┘
```

---

## 5. Feeding Sequence — `AugerControl` Stage Machine

This is the most complex part. `update(currentWeight)` is called every loop iteration while in `FEEDING` state.

```
startFeeding(target, chainPreRunTime, maxRuntime, ...)
    │
    ├─ controlChain(ON)    ← K2,K3,K4,K5 HIGH
    └─ _stage = CHAIN_ONLY


          CHAIN_ONLY
    ┌─────────────────────────────────────────────────────┐
    │  Chain conveyors running, augers OFF                │
    │  Waiting chainPreRunTime seconds (default: 10s)     │
    │  Purpose: prime the chain before auger starts       │
    └─────────────────────────────────────────────────────┘
          │
          │ elapsed >= chainPreRunTime
          │ controlAuger(ON)  ← K1, K6 HIGH
          ▼

          BOTH_RUNNING
    ┌─────────────────────────────────────────────────────┐
    │  Auger + All 4 chain conveyors running              │
    │                                                     │
    │  Every loop:                                        │
    │    weightDispensed = startWeight - currentWeight    │
    │    checkSafety(currentWeight):                      │
    │      if elapsed>30s && dispensed < fluctThreshold  │
    │          → WARNING "No weight change"               │
    │    every 60s:                                       │
    │      rate = weightLastMinute - currentWeight        │
    │      if rate < alarmThreshold (10 lb/min):          │
    │          → WARNING "Low feed rate"                  │
    │                                                     │
    │  COMPLETION check:                                  │
    │    weightDispensed >= targetWeight                  │
    │          → COMPLETED ✓                              │
    │                                                     │
    │  FAILURE check:                                     │
    │    elapsed >= maxRuntime (600s default)             │
    │          → FAILED ✗                                 │
    └──────────────────┬──────────────────────────────────┘
                       │
            fill detection (every 10s):
            rate = (currentWeight - fillRateWeight) / elapsed
            if rate > fillDetectionRate (20 lb/min)
                       │
                       ▼

          PAUSED_FOR_FILL
    ┌─────────────────────────────────────────────────────┐
    │  All motors OFF                                     │
    │  A bin is being filled from above — wait            │
    │                                                     │
    │  Monitor weight:                                    │
    │    if still increasing → reset stabilize timer      │
    │    if stable/decreasing → start countdown           │
    │                                                     │
    │  After fillSettlingTime seconds stable:             │
    │    adjust _startWeight += weightGain from fill      │
    │    (preserves already-dispensed amount)             │
    │    resume to previous stage (CHAIN_ONLY or BOTH)    │
    └─────────────────────────────────────────────────────┘


     Also possible at any point (user action):

     PAUSED_MANUAL        ← web API /pause
         → motors OFF, waits
         → web API /resume → back to previous stage

     TERMINATED           ← web API /terminate or Telegram /disable
         → motors OFF, logs partial event, returns to IDLE
```

---

## 6. Relay Wiring

```
controlAuger(state):
    GPIO 33  (K1) ── Auger motor 1   ┐
    GPIO 19  (K6) ── Auger motor 2   ┘  both always same state

controlChain(state):
    GPIO 32  (K2) ── Chain conveyor A  ┐
    GPIO 13  (K3) ── Chain conveyor B  │  all always same state
    GPIO 12  (K4) ── Chain conveyor C  │
    GPIO 21  (K5) ── Chain conveyor D  ┘
```

---

## 7. BinTrac Modbus Protocol Deep Dive

```
readAllBins() call path:

  modbusRead(address=1000, length=8, buffer[8])
       │
       ├─ address -= 1  →  999  (1-based config → 0-based wire)
       │
       ├─ open fresh EthernetClient TCP connection to 192.168.1.173:502
       │
       ├─ build 12-byte Modbus TCP request:
       │
       │   Byte 0-1: Transaction ID (auto-increment, big-endian)
       │   Byte 2-3: Protocol ID = 0x0000 (always Modbus TCP)
       │   Byte 4-5: Length = 0x0006 (6 more bytes follow)
       │   Byte 6:   Unit/Device ID = config.bintracDeviceID
       │   Byte 7:   Function Code = 0x04 (Read Input Registers)
       │   Byte 8-9: Starting Address = 999 (0x03E7)
       │   Byte 10-11: Quantity = 8 registers
       │
       ├─ client.write(request, 12)
       ├─ wait for 9+ bytes response (timeout: 5000ms)
       │
       └─ parse response:
              Byte 7: if bit7 set → Modbus exception, fail
              Byte 8: byte count (expected: 16 = 8 registers × 2)
              Bytes 9-24: register values (big-endian pairs)


  Parse 8 registers into 4 bin weights:

     registers[0..1] → Bin A:  int32 = (reg[0] << 16) | reg[1]
     registers[2..3] → Bin B:  int32 = (reg[2] << 16) | reg[3]
     registers[4..5] → Bin C:  int32 = (reg[4] << 16) | reg[5]
     registers[6..7] → Bin D:  int32 = (reg[6] << 16) | reg[7]

     if value == -32767 (0xFFFF8001) → bin disabled → weight = 0.0
     else → weight = (float)value  (in lbs, as configured in HouseLink)


  EMA smoothing (in main.cpp):
     smoothed = 0.3 * raw + 0.7 * previous
     (alpha=0.3 → fairly responsive, some noise filtering)
```

---

## 8. Scheduler Logic

```
NTP sync (once at boot, manual retrigger not implemented):
    EthernetUDP → pool.ntp.org:123
    Extract bytes 40-43 → seconds since 1900
    subtract 2208988800 → Unix epoch
    settimeofday() → sets ESP32 system clock

shouldFeed() (called every loop iteration):
    currentMinutes = (hour * 60 + min) adjusted for timezone
    for each feedTime[0..numFeedings-1]:
        if NOT _feedingCompleted[i]:
            if currentMinutes in [feedTime[i], feedTime[i]+1):
                feedCycle = i
                return true  ← TRIGGER FEED

    Match window is exactly 1 minute wide.
    Once feeding starts, markFeedingComplete(i) prevents re-trigger.
    At midnight: _feedingCompleted[0..3] = false (resets daily)


calculateFeedTarget(feedCycle):
    if NOT the last feed of the day:
        return config.feedAmounts[feedCycle]   ← simple fixed amount

    if IS the last feed:
        sum today's actual dispensed from history
        target = config.dailyTotal - alreadyDispensed
        (auto-balances to hit daily goal)
```

---

## 9. Data Persistence (`Storage`)

```
LittleFS filesystem on ESP32 flash:

  /config.json
     ├─ bintracIP, bintracDeviceID
     ├─ feedTimes[4], feedAmounts[4], numFeedings, dailyTotal
     ├─ chainPreRunTime, maxRuntime, alarmThreshold
     ├─ fillDetectionRate, fillSettlingTime, weightFluctuationThreshold
     ├─ telegramToken, telegramChatID, telegramEnabled
     ├─ wifiSSID, wifiPassword
     └─ autoFeedEnabled, timezone

  /history.csv
     ← one row per FeedEvent:
        timestamp, feedCycle, targetWeight, actualWeight, duration, alarmTriggered, alarmReason

  Feed progress (NVS / preferences, NOT LittleFS):
     ← saved every 60s during active feeding
     ← cleared on completion/failure
     ← checked at boot to recover interrupted feeds
```

---

## 10. End-to-End Feeding Timeline (happy path)

```
T+0:00   Scheduler.shouldFeed() returns true for cycle 1
         main captures totalWeight as weightAtStart
         augerControl.startFeeding(50.0 lbs, preRun=10s, max=600s)
         storage.saveFeedProgress(...)    ← crash recovery checkpoint
         state = FEEDING

T+0:00   CHAIN_ONLY: K2,K3,K4,K5 → HIGH (chains start)

T+0:10   chainPreRunTime elapsed
         BOTH_RUNNING: K1,K6 → HIGH (augers start)
         Feed is actively dispensing

T+0:10   every loop: weightDispensed = startWeight - currentWeight
T+1:00   per-minute rate check: warn if <10 lb/min
T+1:00   storage.saveFeedProgress() checkpoint

         ... feed continues ...

T+X:XX   weightDispensed >= 50.0 lbs
         stopAll() → all 6 relays LOW
         stage = COMPLETED

         handleFeedingComplete():
           ├─ create FeedEvent (actual, duration, no alarm)
           ├─ storage.addFeedEvent() → append to /history.csv
           ├─ storage.clearFeedProgress()
           ├─ totalDispensedToday += actualWeight
           ├─ scheduler.markFeedingComplete(cycle)
           ├─ telegramBot.sendFeedingComplete(...)
           └─ state = IDLE
```

---

## 11. Web API Surface

```
GET  /              → serve /index.html from LittleFS
GET  /api/status    → JSON: systemStatus (state, weights, relays, time, ...)
GET  /api/config    → JSON: current config
POST /api/config    → save new config to /config.json
GET  /api/history   → CSV: feed history
POST /api/history/clear → wipe /history.csv
POST /api/feed/start    → manual feed start (like Telegram /startfeed)
POST /api/feed/stop     → augerControl.terminate()
POST /api/feed/pause    → augerControl.pauseFeeding()
POST /api/feed/resume   → augerControl.resumeFeeding()
POST /api/manual/auger  → setAuger(on/off)  [only when STOPPED]
POST /api/manual/chain  → setChain(on/off)  [only when STOPPED]
GET  /api/weightlog → JSON: last 60 EMA-smoothed weight readings
POST /api/alarm/clear → state = IDLE
```

---

## 12. Dual Network: Ethernet + WiFi

```
Ethernet (W5500) ──── primary network
    ├─ BinTrac Modbus TCP reads
    ├─ NTP UDP sync
    ├─ HTTP web server (port 80)
    └─ DHCP → auto-assign .205 as last octet

WiFi (ESP32 built-in) ──── secondary, Telegram only
    └─ WiFiClientSecure (setInsecure) → Telegram HTTPS API
       (SSL too heavy for Ethernet library)

Both run simultaneously — Ethernet handles everything local,
WiFi handles only the outbound Telegram SSL connection.
```

---

## Key Design Insight

The most important architectural decision: **`main.cpp` reads BinTrac and passes weight into `AugerControl`** — `AugerControl` never touches the network directly. This keeps them decoupled:

- `BinTrac` only knows about Modbus TCP
- `AugerControl` only knows about relays and weight values
- `main.cpp` is the glue that connects them and drives the state machine

`AugerControl.update(weight)` returns a `FeedingStage` enum, and `main.cpp` acts on `COMPLETED`, `FAILED`, or `TERMINATED` to log events, notify Telegram, and transition system state.
