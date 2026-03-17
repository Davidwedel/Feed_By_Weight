#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <WiFi.h>
#include "config.h"
#include "types.h"
#include "storage.h"
#include "bintrac.h"
#include "auger_control.h"
#include "scheduler.h"
#include "web_server.h"
#include "telegram_bot.h"

// Global Objects - Core System Components
Storage storage;              // LittleFS file storage for config and feed history
BinTrac bintrac;             // Modbus TCP client for bin weight monitoring
AugerControl augerControl;   // Controls feeding motors (augers and chains)
Scheduler scheduler;         // NTP time sync and scheduled feed triggering
Config config;               // System configuration loaded from storage
SystemStatus systemStatus;   // Current system state shared with web interface
FeedWebServer* webServer;    // Async HTTP server for web UI and API
TelegramBot* telegramBot;    // Optional Telegram notifications and commands

// State Tracking Variables
uint8_t currentFeedCycle = 0;        // Which feeding cycle (0-3) is currently active
float currentFeedTarget = 0;         // Target weight for current feeding (captured at start)
unsigned long lastBintracRead = 0;   // Timestamp of last bin weight read (for 3-second interval)
unsigned long lastProgressSave = 0;  // Timestamp of last feed progress save to NVS
bool networkConnected = false;       // True if Ethernet has valid IP address
float totalDispensedToday = 0;       // Running total of feed dispensed today (lbs)
uint8_t lastDayForTotal = 0;         // Day-of-month for detecting midnight rollover
volatile bool historyChanged = false;// Flag set when history is restored/cleared

// Weight Smoothing Configuration
// We use Exponential Moving Average (EMA) to smooth noisy bin weight readings
bool emaInitialized = false;         // True after first weight reading
WeightLog weightLog;                 // Ring buffer of recent weight readings for web UI
const float EMA_ALPHA = 0.1;         // EMA smoothing factor (0.3 = 30% new, 70% old)

// Function declarations
void setupNetwork();
void updateBinWeights();
void updateSystemStatus();
void runStateMachine();
void handleFeedingComplete();
void handleFeedingFailed();
void handleFeedingTerminated();
float calculateFeedTarget(uint8_t feedCycle);
void calculateTotalDispensedToday();
void getTodaysFeedEvents(FeedEvent* out, int& count);

/**
 * SYSTEM INITIALIZATION
 *
 * Setup sequence:
 * 1. Initialize storage (LittleFS) and load configuration
 * 2. Check for interrupted feeds from previous boot (power loss recovery)
 * 3. Initialize network (Ethernet with DHCP, then static IP .205)
 * 4. Initialize hardware (auger/chain relays, BinTrac Modbus connection)
 * 5. Sync time via NTP (required for scheduled feeding)
 * 6. Start web server and optional Telegram bot
 */
void setup() {
    Serial.begin(115200);
    delay(1000);

    // Initialize status LED (blinks every 3 seconds during normal operation)
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    // Initialize storage
    if (!storage.begin()) {
        Serial.println("FATAL: Storage initialization failed!");
        systemStatus.state = SystemState::ERROR;
        strcpy(systemStatus.lastError, "Storage init failed");
        while (1) {
            digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
            delay(200);
        }
    }

    // Load configuration
    if (!storage.loadConfig(config)) {
        Serial.println("Using default configuration");
    }

    // Power Loss Recovery
    // During feeding, we save progress to NVS every 60 seconds. If the ESP rebooted
    // unexpectedly (power loss, crash), we recover that partial feed and log it to history.
    {
        float pfStartWt, pfDispensed, pfTarget;
        uint8_t pfCycle;
        unsigned long pfTimestamp;
        if (storage.loadFeedProgress(pfStartWt, pfDispensed, pfTarget, pfCycle, pfTimestamp)) {
            Serial.printf("Recovered interrupted feed: cycle=%d, dispensed=%.2f/%.2f lbs\n",
                         pfCycle + 1, pfDispensed, pfTarget);

            // Create a feed event record marking it as interrupted
            FeedEvent event;
            event.timestamp = pfTimestamp;
            event.feedCycle = pfCycle;
            event.targetWeight = pfTarget;
            event.actualWeight = pfDispensed;  // Whatever was dispensed before reboot
            event.duration = 0;  // Unknown - we don't track start time across reboots
            event.alarmTriggered = true;
            strcpy(event.alarmReason, "Interrupted by reboot");

            storage.addFeedEvent(event);
            storage.clearFeedProgress();  // Clear NVS so we don't recover it again
            Serial.println("Partial feed event logged to history");
        }
    }

    // Initialize Network
    setupNetwork();

    // Initialize auger control
    augerControl.begin();

    // Initialize BinTrac
    Serial.printf("Connecting to BinTrac at %s:502...\n", config.bintracIP);
    if (bintrac.begin(config.bintracIP, 502, config.bintracDeviceID)) {
        Serial.println("BinTrac connected");
    } else {
        Serial.printf("BinTrac connection failed: %s\n", bintrac.getLastError());
    }

    // Initialize scheduler
    scheduler.begin(config.timezone);

    // Wait a bit for network stack to stabilize, then start NTP sync
    // Do this after BinTrac connection proves network is working
    delay(3000);
    scheduler.startNTPSync();

    // Initialize web server
    webServer = new FeedWebServer(storage, augerControl, bintrac, config, systemStatus, weightLog);
    webServer->begin();

    // WiFi for Telegram (runs alongside Ethernet)
    // Telegram bot requires SSL/TLS which works better over WiFi than Ethernet.
    // We connect WiFi in addition to Ethernet if Telegram is enabled.
    if (config.telegramEnabled && strlen(config.wifiSSID) > 0) {
        Serial.printf("Connecting WiFi to '%s' for Telegram...\n", config.wifiSSID);
        WiFi.mode(WIFI_STA);
        WiFi.begin(config.wifiSSID, config.wifiPassword);
        unsigned long wifiStart = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
            delay(250);
            Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("\nWiFi connection failed - Telegram bot will not work");
        }
    }

    // Initialize Telegram bot
    telegramBot = new TelegramBot(config);
    if (config.telegramEnabled && WiFi.status() == WL_CONNECTED) {
        telegramBot->begin();
    }

    // Initialize system status
    systemStatus.state = SystemState::IDLE;
    systemStatus.feedingStage = FeedingStage::STOPPED;
    systemStatus.feedStartTime = 0;
    systemStatus.weightAtStart = 0;
    systemStatus.weightDispensed = 0;
    systemStatus.augerRunning = false;
    systemStatus.chainRunning = false;
    systemStatus.bintracConnected = false;
    systemStatus.networkConnected = networkConnected;
    systemStatus.lastBintracUpdate = 0;
    strcpy(systemStatus.lastError, "");

    systemStatus.totalDispensedToday = 0;

    // Calculate total dispensed today from history (requires time sync)
    // This scans the feed history and sums all feeds from today
    if (scheduler.isTimeSynced()) {
        calculateTotalDispensedToday();
    }

    digitalWrite(STATUS_LED_PIN, HIGH);
    Serial.println("\nSystem initialization complete\n");
}

/**
 * MAIN LOOP
 *
 * Execution flow:
 * 1. Update scheduler (checks for day rollover to reset feed completion flags)
 * 2. Process Telegram bot commands (/status, /disable, /clearalarm, /startfeed)
 * 3. Handle web server requests (async, non-blocking)
 * 4. Read bin weights every 3 seconds, update system status immediately after
 * 5. Run state machine (checks for scheduled feeds, monitors active feeding)
 *
 * Loop runs as fast as possible (~100Hz with 10ms delay) to ensure responsive
 * auger control during feeding. BinTrac reads are rate-limited to 3 seconds.
 */
void loop() {
    // Update scheduler time and check for day rollover
    scheduler.update();
    scheduler.getCurrentTimeStr(systemStatus.currentTime, sizeof(systemStatus.currentTime));

    // Periodic Bin Weight Reading (every 3 seconds, or immediately on first loop)
    if (millis() - lastBintracRead > 3000 || lastBintracRead == 0) {
        updateBinWeights();        // Read from BinTrac, apply EMA smoothing
        lastBintracRead = millis();

        updateSystemStatus();      // Update systemStatus structure for web UI

        // Blink status LED to show system is alive
        digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
    }

    // Update Telegram bot
    if (config.telegramEnabled) {
        telegramBot->update();

        // Send status if requested
        if (telegramBot->isStatusRequested()) {
            String chatId = telegramBot->getStatusRequestChatId();
            telegramBot->sendStatus(systemStatus, chatId);
        }

        // Stop feeding if /disable was issued
        if (telegramBot->isStopRequested() && augerControl.isFeeding()) {
            augerControl.stopAll();
            systemStatus.state = SystemState::IDLE;
            Serial.println("Feeding stopped via Telegram /disable");
        }

        // Clear alarm if /clearalarm was issued
        if (telegramBot->isClearAlarmRequested() && systemStatus.state == SystemState::ALARM) {
            systemStatus.state = SystemState::IDLE;
            strcpy(systemStatus.lastError, "");
            Serial.println("Alarm cleared via Telegram /clearalarm");
        }

        // Add manual feed event if /logoldfeed was issued
        if (telegramBot->isAddFeedRequested()) {
            FeedEvent event = telegramBot->getAddFeedEvent();
            storage.addFeedEvent(event);
            calculateTotalDispensedToday();
            Serial.printf("Feed event added via Telegram: cycle %d, %.2f lbs\n",
                         event.feedCycle + 1, event.actualWeight);
        }

        // Start feed if /startfeed was issued
        if (telegramBot->isStartFeedRequested()) {
            if (augerControl.isFeeding() || systemStatus.state == SystemState::FEEDING) {
                Serial.println("Telegram /startfeed ignored - already feeding");
				telegramBot->sendMessage("Telegram /startfeed ignored - already feeding");
            } else if (systemStatus.state == SystemState::ALARM) {
                Serial.println("Telegram /startfeed ignored - in alarm state");
				telegramBot->sendMessage("Telegram /startfeed ignored - in alarm state");
            } else {
                systemStatus.weightAtStart = systemStatus.totalCurrentWeight;
                currentFeedTarget = telegramBot->getStartFeedWeight();
                currentFeedCycle = 0;

                augerControl.startFeeding(currentFeedTarget, config.chainPreRunTime,
                    config.maxRuntime);

                systemStatus.state = SystemState::FEEDING;
                systemStatus.feedStartTime = millis();

                unsigned long ts = scheduler.isTimeSynced() ? scheduler.getCurrentTime() : 0;
                storage.saveFeedProgress(systemStatus.weightAtStart, 0, currentFeedTarget, currentFeedCycle, ts);
                lastProgressSave = millis();

                Serial.printf("Feed started via Telegram: %.2f lbs target\n", currentFeedTarget);
            }
        }

        // Send daily summary if /dailysummary was issued
        if (telegramBot->isDailySummaryRequested()) {
            FeedEvent todayEvents[20];
            int todayCount = 0;
            getTodaysFeedEvents(todayEvents, todayCount);
            telegramBot->sendDailySummary(todayEvents, todayCount);
        }
    }

    // Handle web server requests
    webServer->handleClient();

    // Run main state machine
    runStateMachine();

    delay(10);
}

/**
 * NETWORK INITIALIZATION (W5500 Ethernet)
 *
 * Strategy:
 * 1. Hardware reset the W5500 chip (required for reliable initialization)
 * 2. Try DHCP first to learn network configuration (gateway, DNS, subnet)
 * 3. If DHCP succeeds, reconnect with static IP ending in .205 using learned config
 * 4. If DHCP fails, use fallback static IP 192.168.1.205
 *
 * Why static IP .205? Makes the feeder easy to find on the network at a consistent
 * address, while still benefiting from DHCP to learn the correct gateway/subnet.
 */
void setupNetwork() {
    Serial.println("Initializing W5500 Ethernet...");
	/*
    Serial.println("Pin configuration:");
    Serial.printf("  CS:   GPIO %d\n", W5500_CS_PIN);
    Serial.printf("  MISO: GPIO %d\n", W5500_MISO_PIN);
    Serial.printf("  MOSI: GPIO %d\n", W5500_MOSI_PIN);
    Serial.printf("  SCK:  GPIO %d\n", W5500_SCK_PIN);
    Serial.printf("  RST:  GPIO %d\n", W5500_RESET_PIN);
	*/

    // Hardware reset W5500 (pull reset low for 50ms, then high)
    pinMode(W5500_RESET_PIN, OUTPUT);
    digitalWrite(W5500_RESET_PIN, LOW);
    delay(50);
    digitalWrite(W5500_RESET_PIN, HIGH);
    delay(200);  // Wait for chip to initialize

    // Initialize SPI with custom pins
    SPI.begin(W5500_SCK_PIN, W5500_MISO_PIN, W5500_MOSI_PIN, W5500_CS_PIN);

    // Initialize Ethernet library with CS pin
    Ethernet.init(W5500_CS_PIN);

    // MAC address
    byte mac[] = W5500_MAC;

    // Phase 1: Try DHCP to learn network configuration
    Serial.println("Getting network info via DHCP...");
    Ethernet.begin(mac);
    delay(5000);  // Wait for DHCP negotiation to complete

    IPAddress dhcpIP = Ethernet.localIP();

    // Verify we got a valid private network IP (not 0.0.0.0 or public IP)
    // Valid private ranges: 192.168.x.x, 10.x.x.x, 172.16-31.x.x
    if (!((dhcpIP[0] == 192 && dhcpIP[1] == 168) ||
          (dhcpIP[0] == 10) ||
          (dhcpIP[0] == 172 && dhcpIP[1] >= 16 && dhcpIP[1] <= 31))) {
        // DHCP failed - use hardcoded fallback configuration
        Serial.println("DHCP failed, using fallback static IP");

        // Fallback to basic static IP
        IPAddress ip(192, 168, 1, 205);
        IPAddress dns(192, 168, 1, 1);
        IPAddress gateway(192, 168, 1, 1);
        IPAddress subnet(255, 255, 255, 0);

        Ethernet.begin(mac, ip, dns, gateway, subnet);
        delay(1000);

        networkConnected = true;

        Serial.print("Fallback IP Address: ");
        Serial.println(Ethernet.localIP());
        return;
    }

    // Give W5500 time to stabilize after DHCP
    delay(1000);

    // Phase 2: Read learned network configuration
    IPAddress gateway = Ethernet.gatewayIP();
    IPAddress subnet = Ethernet.subnetMask();
    IPAddress dns = Ethernet.dnsServerIP();

    Serial.println("DHCP configuration obtained:");
    Serial.print("  IP: ");
    Serial.println(dhcpIP);
    Serial.print("  Gateway: ");
    Serial.println(gateway);
    Serial.print("  Subnet: ");
    Serial.println(subnet);
    Serial.print("  DNS: ");
    Serial.println(dns);

    // Phase 3: Reconnect with static IP .205
    // Use the same subnet as DHCP gave us, but change last octet to 205
    IPAddress staticIP(dhcpIP[0], dhcpIP[1], dhcpIP[2], 205);

    Serial.print("Reconnecting with static IP: ");
    Serial.println(staticIP);

    Ethernet.begin(mac, staticIP, dns, gateway, subnet);
    delay(1000);  // Wait for connection to establish

    // Verify connection
    Serial.println("Ethernet connected with static IP");
    Serial.print("Final IP Address: ");
    Serial.println(Ethernet.localIP());
    networkConnected = true;
}

/**
 * READ AND SMOOTH BIN WEIGHTS
 *
 * Reads all 4 bins from BinTrac via single Modbus TCP transaction, then applies
 * Exponential Moving Average (EMA) smoothing to reduce noise from vibration/settling.
 *
 * EMA formula: smoothed = alpha * new + (1-alpha) * previous
 * With alpha=0.3, we get 30% responsiveness to new readings while filtering noise.
 * also detects bin fills
 */
void updateBinWeights() {
    if (bintrac.readAllBins(systemStatus.currentWeight)) {
        systemStatus.bintracConnected = true;

		//reset lastBintracUpdate
        systemStatus.lastBintracUpdate = millis();

        // Calculate total weight across all bins
		//systemStatus.totalCurrentWeight = 0;
		float newTotal = 0;
        for (int i = 0; i < 4; i++) newTotal += systemStatus.currentWeight[i];

		//ema smooth totalCurrentWeight
        if (!emaInitialized) {// initialize
            systemStatus.totalCurrentWeight = newTotal;
            emaInitialized = true;
        } else {
            systemStatus.totalCurrentWeight = EMA_ALPHA * newTotal + (1.0 - EMA_ALPHA) * systemStatus.totalCurrentWeight; 
        }

		// Store new totalCurrentWeight in circular buffer
		systemStatus.weightHistory[systemStatus.historyIndex] = systemStatus.totalCurrentWeight;

		// Advance circular buffer index
		systemStatus.historyIndex = (systemStatus.historyIndex + 1) % SystemStatus::WEIGHT_HISTORY_SIZE;
		if (systemStatus.historyCount < SystemStatus::WEIGHT_HISTORY_SIZE) systemStatus.historyCount++;

        // Calculate rate of change (lbs/min) using oldest vs newest in history
        // This gives smoother readings over a longer window (up to 60 seconds)
        if (systemStatus.historyCount >= 2) {
            int newestIdx = (systemStatus.historyIndex - 1 + SystemStatus::WEIGHT_HISTORY_SIZE) % SystemStatus::WEIGHT_HISTORY_SIZE;
            int oldestIdx = (systemStatus.historyIndex - systemStatus.historyCount + SystemStatus::WEIGHT_HISTORY_SIZE) % SystemStatus::WEIGHT_HISTORY_SIZE;

            float newestWeight = systemStatus.weightHistory[newestIdx];
            float oldestWeight = systemStatus.weightHistory[oldestIdx];

            // Time span = (number of samples - 1) * 3 seconds per sample
            float timeDeltaMinutes = (systemStatus.historyCount - 1) * 3.0 / 60.0;
            float weightChange = oldestWeight - newestWeight;
            systemStatus.flowRate = weightChange / timeDeltaMinutes;
        }

        // (weightLog is used by web UI to show recent weight trends)
        weightLog.add(millis(), systemStatus.currentWeight, systemStatus.totalCurrentWeight);

        // Debug: print weights every read
		/*
        static int readCount = 0;
        if (++readCount % 1 == 0) {
            Serial.printf("Bins: A=%.0f B=%.0f C=%.0f D=%.0f Total=%.0f Rate=%.0f lb/min\n",
                systemStatus.currentWeight[0],
                systemStatus.currentWeight[1],
                systemStatus.currentWeight[2],
                systemStatus.currentWeight[3],
				systemStatus.totalCurrentWeight,
				systemStatus.flowRate);
        }*/

		// do projected Weight calcs
		systemStatus.projectedWeightDispensed = systemStatus.totalCurrentWeight + config.projectedWeight;

		//do bin fill detection
		// Track previous state to detect transitions
		static bool prevBinFillDetected = false;

		//how many readings back to go
		int howFarBack = 5;

		//default to false
		systemStatus.binFillDetected = false;

		// check if we have enough samples
		if (systemStatus.historyCount >= howFarBack) {
			// Check if last howFarBack readings are progressively heavier (>10 lbs per step)
			bool allPassed = true;
			for (int i = 0; i < howFarBack - 1; i++) {
				int newerIdx = (systemStatus.historyIndex - 1 - i + SystemStatus::WEIGHT_HISTORY_SIZE) % SystemStatus::WEIGHT_HISTORY_SIZE;
				int olderIdx = (systemStatus.historyIndex - 2 - i + SystemStatus::WEIGHT_HISTORY_SIZE) % SystemStatus::WEIGHT_HISTORY_SIZE;

				float delta = systemStatus.weightHistory[newerIdx] - systemStatus.weightHistory[olderIdx];
				if (delta <= 10) {  // Not increasing by enough
					allPassed = false;
					break;
				}
			}
			//if all checks passed, then bin is being filled
			if (allPassed) {
				systemStatus.binFillDetected = true;
			}
		}

		// Track fill end time for post-fill wait period
		static unsigned long fillEndedTime = 0;

		// Notify on bin fill start
		if (systemStatus.binFillDetected && !prevBinFillDetected) {
			if (config.telegramEnabled && telegramBot) {
				telegramBot->sendMessage("Bin fill detected - feeding paused");
			}
			Serial.println("Bin fill detected - notification sent");
			fillEndedTime = 0;  // Reset wait timer
		}

		// When bin fill ends, start wait timer
		if (!systemStatus.binFillDetected && prevBinFillDetected) {
			fillEndedTime = millis();
			Serial.printf("Bin fill ended - starting %d minute wait\n", config.fillSettlingTime);
		}

		// Check if wait time has elapsed after fill ended
		if (fillEndedTime > 0 && (millis() - fillEndedTime) / 1000 >= (config.fillSettlingTime * 60)) {
			if (config.telegramEnabled && telegramBot) {
				telegramBot->sendMessage("Bin fill wait complete - normal operation");
			}
			Serial.println("Bin fill wait complete - notification sent");
			fillEndedTime = 0;  // Reset timer
		}

		prevBinFillDetected = systemStatus.binFillDetected;

    } else {
        // Read failed - log error and attempt reconnection if down for 30+ seconds
        systemStatus.bintracConnected = false;
        Serial.printf("BinTrac read failed: %s\n", bintrac.getLastError());

        // Auto-reconnect if connection has been down for more than 30 seconds
        if (millis() - systemStatus.lastBintracUpdate > 30000) {
            Serial.println("Attempting BinTrac reconnection...");
            bintrac.reconnect();
        }
    }
}

/**
 * UPDATE SYSTEM STATUS STRUCTURE
 *
 * Collects current state from all subsystems into the systemStatus structure,
 * which is used by the web interface for real-time display.
 *
 * Also handles:
 * - Day rollover detection (resets totalDispensedToday at midnight)
 * - Including in-progress feed weight in daily total
 * - Network connection status monitoring
 */
void updateSystemStatus() {
    // Copy current state from auger controller
    systemStatus.augerRunning = augerControl.isAugerRunning();
    systemStatus.chainRunning = augerControl.isChainRunning();
    systemStatus.feedingStage = augerControl.getStage();
    systemStatus.weightDispensed = augerControl.getWeightDispensed();

    // Recalculate daily total if history was restored/cleared via web UI
    if (historyChanged && scheduler.isTimeSynced()) {
        historyChanged = false;
        calculateTotalDispensedToday();
    }

    // Day Rollover Detection
    // Check if the day has changed and reset the daily total at midnight
    if (scheduler.isTimeSynced()) {
        if (lastDayForTotal == 0) {
            // First time sync after boot - calculate total from history
            calculateTotalDispensedToday();
        } else {
            // Check if day-of-month has changed (midnight rollover)
            unsigned long now = scheduler.getCurrentTime() + (config.timezone * 3600L);
            struct tm nowInfo;
            time_t nowTime = (time_t)now;
            gmtime_r(&nowTime, &nowInfo);
            uint8_t currentDay = nowInfo.tm_mday;
            if (currentDay != lastDayForTotal) {
                totalDispensedToday = 0;  // Reset at midnight
                lastDayForTotal = currentDay;
            }
        }
    }

    // Include in-progress feeding in daily total (for live web UI display)
    systemStatus.totalDispensedToday = totalDispensedToday;
    if (systemStatus.state == SystemState::FEEDING && systemStatus.weightDispensed > 0) {
        systemStatus.totalDispensedToday += systemStatus.weightDispensed;
    }

    // Update network connection status (check if we have a valid IP)
    IPAddress ip = Ethernet.localIP();
    networkConnected = (ip[0] != 0);
    systemStatus.networkConnected = networkConnected;
}

/**
 * MAIN STATE MACHINE
 *
 * Controls high-level system behavior based on current state:
 *
 * - IDLE / WAITING_FOR_SCHEDULE: Check if it's time for scheduled feeding
 * - FEEDING: Call augerControl.update() every loop, handle completion/failure
 * - MANUAL_OVERRIDE: Wait for manual control to finish
 * - ALARM: Require user intervention to clear
 * - ERROR: Fatal error, system halted
 *
 * The state machine drives transitions, but AugerControl manages the actual
 * feeding sequence (chain pre-run, then both auger+chain).
 */
void runStateMachine() {
    switch (systemStatus.state) {
        case SystemState::IDLE:
        case SystemState::WAITING_FOR_SCHEDULE:
            if (config.autoFeedEnabled && scheduler.isTimeSynced()) {
                // Check if current time matches any scheduled feed time (within 1-minute window)
                if (scheduler.shouldFeed(config.feedTimes, config.numFeedings, currentFeedCycle)) {
                    Serial.printf("Starting scheduled feeding cycle %d\n", currentFeedCycle + 1);

                    // Capture starting weight from all bins
                    systemStatus.weightAtStart = systemStatus.totalCurrentWeight;

                    // Calculate target for this feeding
                    // Note: Last feed of the day auto-balances to hit dailyTotal exactly
                    currentFeedTarget = calculateFeedTarget(currentFeedCycle);

                    // Start feeding sequence (chain pre-run, then both auger+chain)
                    augerControl.startFeeding(currentFeedTarget, config.chainPreRunTime, config.maxRuntime);
                    systemStatus.state = SystemState::FEEDING;
                    systemStatus.feedStartTime = millis();

                    // Save initial feed progress to NVS (for power loss recovery)
                    unsigned long ts = scheduler.isTimeSynced() ? scheduler.getCurrentTime() : 0;
                    storage.saveFeedProgress(systemStatus.weightAtStart, 0, currentFeedTarget, currentFeedCycle, ts);
                    lastProgressSave = millis();

                    // Note: scheduler.markFeedingComplete() is called later after successful completion
                }
            }
            break;

        case SystemState::FEEDING: {
            // Active Feeding - Call augerControl.update() every loop
            // AugerControl manages the 2-stage sequence and returns its current stage.
            // We check for completion/failure/termination and handle accordingly.

            // Update auger control (returns current stage: CHAIN_ONLY, BOTH_RUNNING, COMPLETED, etc.)
            FeedingStage stage = augerControl.update(systemStatus.totalCurrentWeight);

            // Check for warnings (low feed rate, weight reading failures, etc.)
            const char* warning = augerControl.getNewWarning();
            if (warning != nullptr && config.telegramEnabled) {
                String msg = String("Feed Cycle ") + String(currentFeedCycle + 1) + "\n" + String(warning);
                telegramBot->sendMessage(msg);
            }

            // Save feed progress to NVS every 60 seconds (for power loss recovery)
            if (millis() - lastProgressSave >= 60000) {
                unsigned long ts = scheduler.isTimeSynced() ? scheduler.getCurrentTime() : 0;
                storage.saveFeedProgress(systemStatus.weightAtStart, augerControl.getWeightDispensed(),
                                        currentFeedTarget, currentFeedCycle, ts);
                lastProgressSave = millis();
            }

            // Handle feeding completion/failure
            if (stage == FeedingStage::COMPLETED) {
                handleFeedingComplete();
            } else if (stage == FeedingStage::FAILED) {
                handleFeedingFailed();
            } else if (stage == FeedingStage::TERMINATED) {
                handleFeedingTerminated();
            }
            break;
        }

        case SystemState::MANUAL_OVERRIDE:
            // Manual control is active - don't auto-feed
            // Check if manual control has stopped
            if (!augerControl.isFeeding()) {
                systemStatus.state = SystemState::IDLE;
            }
            break;

        case SystemState::ALARM:
            // Alarm state - require user intervention
            // Can be cleared via web interface or Telegram
            break;

        case SystemState::ERROR:
            // Error state - system halted
            break;
    }
}

/**
 * HANDLE SUCCESSFUL FEEDING COMPLETION
 *
 * Called when augerControl.update() returns FeedingStage::COMPLETED.
 * Creates a feed event record, saves to history, marks the feed cycle as complete
 * (preventing re-trigger today), and sends Telegram notification.
 */
void handleFeedingComplete() {
    Serial.println("=== Feeding Complete ===");

    // Create feed event record with actual weight dispensed and duration
    FeedEvent event;
    event.timestamp = scheduler.isTimeSynced() ? scheduler.getCurrentTime() : 0;
    event.feedCycle = currentFeedCycle;
    event.targetWeight = currentFeedTarget;
    event.actualWeight = augerControl.getWeightDispensed();
    event.duration = augerControl.getDuration();
    event.alarmTriggered = false;
    strcpy(event.alarmReason, "");

    // Save to CSV history and clear NVS progress (no longer needed)
    storage.addFeedEvent(event);
    storage.clearFeedProgress();

    // Update running total for today
	calculateTotalDispensedToday();

    if (!scheduler.isTimeSynced()) {
        Serial.println("Warning: Time not synced, event saved with timestamp 0");
    }

    // Mark this feed cycle as complete (prevents re-triggering today)
    scheduler.markFeedingComplete(currentFeedCycle);

    // Send Telegram notification with summary
    if (config.telegramEnabled) {
        telegramBot->sendFeedingComplete(currentFeedCycle, event.targetWeight, event.actualWeight, event.duration, totalDispensedToday);

        // If this was the last feeding of the day, also send daily summary
        if (currentFeedCycle == config.numFeedings - 1) {
            FeedEvent todayEvents[20];
            int todayCount = 0;
            getTodaysFeedEvents(todayEvents, todayCount);
            telegramBot->sendDailySummary(todayEvents, todayCount);
        }
    }

    // Stop all motors and reset auger control state
    augerControl.stopAll();

    // Return to idle state (ready for next scheduled feed)
    systemStatus.state = SystemState::IDLE;

    Serial.printf("Dispensed: %.2f lbs in %d:%02d\n", event.actualWeight, event.duration / 60, event.duration % 60);
}

/**
 * HANDLE FEEDING FAILURE (ALARM)
 *
 * Called when augerControl.update() returns FeedingStage::FAILED.
 * This happens when maximum runtime is exceeded.
 *
 * Creates a feed event with alarm flag set, saves to history, stops motors,
 * enters ALARM state (requires user intervention to clear), and sends Telegram alert.
 */
void handleFeedingFailed() {
    Serial.println("=== Feeding Failed ===");

    // Create feed event record with alarm information
    FeedEvent event;
    event.timestamp = scheduler.isTimeSynced() ? scheduler.getCurrentTime() : 0;
    event.feedCycle = currentFeedCycle;
    event.targetWeight = currentFeedTarget;
    event.actualWeight = augerControl.getWeightDispensed();
    event.duration = augerControl.getDuration();
    event.alarmTriggered = true;
    strncpy(event.alarmReason, augerControl.getAlarmReason(), sizeof(event.alarmReason) - 1);

    // Save to CSV history and clear NVS progress
    storage.addFeedEvent(event);
    storage.clearFeedProgress();

    // Update running total for today
	calculateTotalDispensedToday();

    if (!scheduler.isTimeSynced()) {
        Serial.println("Warning: Time not synced, event saved with timestamp 0");
    }

    // Send Telegram alarm notification
    if (config.telegramEnabled) {
        telegramBot->sendAlarm(currentFeedCycle, event.targetWeight,
                               event.actualWeight, event.alarmReason);
    }

    // Stop all motors immediately
    augerControl.stopAll();

    // Enter ALARM state (requires manual /clearalarm or web UI action)
    systemStatus.state = SystemState::ALARM;
    strncpy(systemStatus.lastError, event.alarmReason, sizeof(systemStatus.lastError) - 1);

    Serial.printf("Alarm: %s\n", event.alarmReason);
}

/**
 * HANDLE USER-INITIATED FEEDING TERMINATION
 *
 * Called when user clicks "Stop Feeding" in web UI or issues Telegram /disable.
 * Creates a normal (non-alarm) feed event with whatever was dispensed so far,
 * marks the cycle as complete, and returns to IDLE.
 */
void handleFeedingTerminated() {
    Serial.println("=== Feeding Terminated by User ===");

    // Create feed event with partial weight dispensed
    FeedEvent event;
    event.timestamp = scheduler.isTimeSynced() ? scheduler.getCurrentTime() : 0;
    event.feedCycle = currentFeedCycle;
    event.targetWeight = currentFeedTarget;
    event.actualWeight = augerControl.getWeightDispensed();
    event.duration = augerControl.getDuration();
    event.alarmTriggered = false;
    strcpy(event.alarmReason, "User terminated");

    storage.addFeedEvent(event);
    storage.clearFeedProgress();

	//recalculate because we added an event
	calculateTotalDispensedToday();

    scheduler.markFeedingComplete(currentFeedCycle);

    if (config.telegramEnabled) {
        telegramBot->sendFeedingComplete(currentFeedCycle, event.targetWeight, event.actualWeight, event.duration,
                                         totalDispensedToday);
    }

    augerControl.stopAll();
    systemStatus.state = SystemState::IDLE;

    Serial.printf("Terminated: %.2f lbs dispensed in %d:%02d\n",
                  event.actualWeight, event.duration / 60, event.duration % 60);
}

/**
 * CALCULATE TOTAL FEED DISPENSED TODAY
 *
 * Scans the feed history and sums all actualWeight values from events that
 * occurred today (same day-of-year and year). This is called:
 * - At boot after time sync
 * - After day rollover (midnight)
 * - After history restore/clear
 */
void getTodaysFeedEvents(FeedEvent* out, int& count) {
    count = 0;
    if (!scheduler.isTimeSynced()) return;
    FeedEvent all[20];
    int total = 0;
    storage.getFeedHistory(all, total, 20);
    unsigned long now = scheduler.getCurrentTime() + (config.timezone * 3600L);
    struct tm todayInfo;
    time_t nowTime = (time_t)now;
    gmtime_r(&nowTime, &todayInfo);
    for (int i = 0; i < total; i++) {
        unsigned long eventTime = all[i].timestamp + (config.timezone * 3600L);
        struct tm eventInfo;
        time_t evTime = (time_t)eventTime;
        gmtime_r(&evTime, &eventInfo);
        if (eventInfo.tm_yday == todayInfo.tm_yday && eventInfo.tm_year == todayInfo.tm_year) {
            out[count++] = all[i];
        }
    }
}

void calculateTotalDispensedToday() {
    totalDispensedToday = 0;
    FeedEvent events[20];
    int count = 0;
    storage.getFeedHistory(events, count, 20);

    // Get today's day-of-year and year (adjusted for timezone)
    unsigned long now = scheduler.getCurrentTime() + (config.timezone * 3600L);
    struct tm todayInfo;
    time_t nowTime = (time_t)now;
    gmtime_r(&nowTime, &todayInfo);
    int todayDay = todayInfo.tm_yday;    // Day of year (0-365)
    int todayYear = todayInfo.tm_year;   // Years since 1900
    lastDayForTotal = todayInfo.tm_mday; // Day of month (1-31) for rollover detection

    // Sum all feed events from today
    for (int i = 0; i < count; i++) {
        unsigned long eventTime = events[i].timestamp + (config.timezone * 3600L);
        struct tm eventInfo;
        time_t evTime = (time_t)eventTime;
        gmtime_r(&evTime, &eventInfo);

        if (eventInfo.tm_yday == todayDay && eventInfo.tm_year == todayYear) {
            totalDispensedToday += events[i].actualWeight;
        }
    }
    Serial.printf("Total dispensed today (from history): %.1f lbs\n", totalDispensedToday);
}

/**
 * CALCULATE FEED TARGET FOR CURRENT CYCLE
 *
 * For feeds 1-3: Returns the configured feedAmounts[feedCycle] directly.
 *
 * For the last feed of the day: Auto-calculates target to hit config.dailyTotal exactly.
 * This compensates for any under/over-dispensing in earlier feeds.
 *
 * Example: If dailyTotal=100 and first 3 feeds dispensed 78 lbs, the 4th feed
 * will target 22 lbs (regardless of what config.feedAmounts[3] says).
 */
float calculateFeedTarget(uint8_t feedCycle) {
    uint8_t lastActiveFeeding = config.numFeedings - 1;

    // Not the last feeding — use configured amount directly
    if (feedCycle < lastActiveFeeding) {
        Serial.printf("Feed %d target: %.1f lbs (configured)\n", feedCycle + 1, config.feedAmounts[feedCycle]);
        return config.feedAmounts[feedCycle];
    }

    // Last feeding of the day — auto-balance
	
    // Calculate how much we've already dispensed today from feed events
	// Should already be up to date but why not.
	calculateTotalDispensedToday();

    // Target = remaining amount to hit daily total
    float remaining = config.dailyTotal - totalDispensedToday;
    if (remaining < 0) remaining = 0;  // Don't dispense negative weight!

    Serial.printf("Last feed calc: dailyTotal=%.1f, dispensed=%.1f, target=%.1f\n",
                  config.dailyTotal, totalDispensedToday, remaining);

    return remaining;
}
