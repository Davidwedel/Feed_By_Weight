#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include "config.h"
#include "types.h"
#include "storage.h"
#include "bintrac.h"
#include "auger_control.h"
#include "scheduler.h"
#include "web_server.h"
#include "telegram_bot.h"

// Global objects
Storage storage;
BinTrac bintrac;
AugerControl augerControl;
Scheduler scheduler;
Config config;
SystemStatus systemStatus;
FeedWebServer* webServer;
TelegramBot* telegramBot;

// State tracking
uint8_t currentFeedCycle = 0;
unsigned long lastBintracRead = 0;
unsigned long lastStatusUpdate = 0;
bool ethConnected = false;

// Function declarations
void setupEthernet();
void updateBinWeights();
void updateSystemStatus();
void runStateMachine();
void handleFeedingComplete();
void handleFeedingFailed();

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n\n=================================");
    Serial.println("Weight Feeder Control System");
    Serial.printf("Version: %s\n", FIRMWARE_VERSION);
    Serial.println("=================================\n");

    // Initialize status LED
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

    // Initialize Ethernet
    setupEthernet();

    // Initialize auger control
    augerControl.begin();

    // Initialize BinTrac
    Serial.printf("Connecting to BinTrac at %s...\n", config.bintracIP);
    if (bintrac.begin(config.bintracIP, config.bintracDeviceID)) {
        Serial.println("BinTrac connected");
    } else {
        Serial.printf("BinTrac connection failed: %s\n", bintrac.getLastError());
    }

    // Initialize scheduler
    scheduler.begin(config.timezone);

    // Initialize web server
    webServer = new FeedWebServer(storage, augerControl, bintrac, config, systemStatus);
    webServer->begin();

    // Initialize Telegram bot
    telegramBot = new TelegramBot(config);
    if (config.telegramEnabled) {
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
    systemStatus.networkConnected = ethConnected;
    systemStatus.lastBintracUpdate = 0;
    strcpy(systemStatus.lastError, "");

    digitalWrite(STATUS_LED_PIN, HIGH);
    Serial.println("\n✓ System initialization complete\n");
}

void loop() {
    // Update scheduler time
    scheduler.update();

    // Update Telegram bot
    if (config.telegramEnabled) {
        telegramBot->update();
    }

    // Handle web server requests
    webServer->handleClient();

    // Read bin weights periodically
    if (millis() - lastBintracRead > WEIGHT_CHECK_INTERVAL) {
        updateBinWeights();
        lastBintracRead = millis();
    }

    // Run main state machine
    runStateMachine();

    // Update system status periodically
    if (millis() - lastStatusUpdate > STATUS_UPDATE_INTERVAL) {
        updateSystemStatus();
        lastStatusUpdate = millis();

        // Blink status LED
        digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
    }

    delay(10);
}

void setupEthernet() {
    Serial.println("Initializing W5500 Ethernet...");

    // Initialize SPI
    SPI.begin();

    // Configure W5500 chip select pin
    pinMode(W5500_CS_PIN, OUTPUT);
    digitalWrite(W5500_CS_PIN, HIGH);

    // Use DHCP (no MAC address = use default)
    if (Ethernet.begin(NULL, 5000, 1000) == 0) {
        Serial.println("Failed to configure Ethernet using DHCP");

        // Try with a static IP as fallback
        IPAddress ip(192, 168, 1, 177);
        IPAddress dns(192, 168, 1, 1);
        IPAddress gateway(192, 168, 1, 1);
        IPAddress subnet(255, 255, 255, 0);

        Ethernet.begin(NULL, ip, dns, gateway, subnet);
        Serial.println("Using static IP configuration");
    }

    // Give W5500 time to initialize
    delay(1000);

    // Check link status
    if (Ethernet.linkStatus() == LinkON) {
        Serial.println("Ethernet connected");
        Serial.print("IP Address: ");
        Serial.println(Ethernet.localIP());
        ethConnected = true;
    } else {
        Serial.println("Ethernet cable not connected");
        Serial.print("IP Address: ");
        Serial.println(Ethernet.localIP());
        ethConnected = false;
    }
}

void updateBinWeights() {
    if (bintrac.readAllBins(systemStatus.currentWeight)) {
        systemStatus.bintracConnected = true;
        systemStatus.lastBintracUpdate = millis();
    } else {
        systemStatus.bintracConnected = false;

        // Try to reconnect
        if (millis() - systemStatus.lastBintracUpdate > 30000) {
            Serial.println("Attempting BinTrac reconnection...");
            bintrac.reconnect();
        }
    }
}

void updateSystemStatus() {
    systemStatus.augerRunning = augerControl.isAugerRunning();
    systemStatus.chainRunning = augerControl.isChainRunning();
    systemStatus.feedingStage = augerControl.getStage();
    systemStatus.weightDispensed = augerControl.getWeightDispensed();
    systemStatus.networkConnected = ethConnected;
}

void runStateMachine() {
    switch (systemStatus.state) {
        case SystemState::IDLE:
        case SystemState::WAITING_FOR_SCHEDULE:
            if (config.autoFeedEnabled && scheduler.isTimeSynced()) {
                // Check if it's time to feed
                if (scheduler.shouldFeed(config.feedTimes, currentFeedCycle)) {
                    Serial.printf("Starting scheduled feeding cycle %d\n", currentFeedCycle + 1);

                    // Calculate total weight from all bins
                    float totalWeight = 0;
                    for (int i = 0; i < 4; i++) {
                        totalWeight += systemStatus.currentWeight[i];
                    }
                    systemStatus.weightAtStart = totalWeight;

                    // Start feeding
                    augerControl.startFeeding(config.targetWeight, config.chainPreRunTime, config.maxRuntime);
                    systemStatus.state = SystemState::FEEDING;
                    systemStatus.feedStartTime = millis();

                    // Mark as started (will be marked complete when done)
                    // scheduler.markFeedingComplete is called after successful completion
                }
            }
            break;

        case SystemState::FEEDING: {
            // Update feeding progress
            float totalWeight = 0;
            for (int i = 0; i < 4; i++) {
                totalWeight += systemStatus.currentWeight[i];
            }

            FeedingStage stage = augerControl.update(totalWeight);

            if (stage == FeedingStage::COMPLETED) {
                handleFeedingComplete();
            } else if (stage == FeedingStage::FAILED) {
                handleFeedingFailed();
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

void handleFeedingComplete() {
    Serial.println("=== Feeding Complete ===");

    // Create feed event record
    FeedEvent event;
    event.timestamp = scheduler.getCurrentTime();
    event.feedCycle = currentFeedCycle;
    event.targetWeight = config.targetWeight;
    event.actualWeight = augerControl.getWeightDispensed();
    event.duration = augerControl.getDuration();
    event.alarmTriggered = false;
    strcpy(event.alarmReason, "");

    // Save to history
    storage.addFeedEvent(event);

    // Mark feeding as complete for this cycle
    scheduler.markFeedingComplete(currentFeedCycle);

    // Send Telegram notification
    if (config.telegramEnabled) {
        telegramBot->sendFeedingComplete(currentFeedCycle, event.actualWeight, event.duration);
    }

    // Return to idle state
    systemStatus.state = SystemState::IDLE;

    Serial.printf("Dispensed: %.2f lbs in %d seconds\n", event.actualWeight, event.duration);
}

void handleFeedingFailed() {
    Serial.println("=== Feeding Failed ===");

    // Create feed event record with alarm
    FeedEvent event;
    event.timestamp = scheduler.getCurrentTime();
    event.feedCycle = currentFeedCycle;
    event.targetWeight = config.targetWeight;
    event.actualWeight = augerControl.getWeightDispensed();
    event.duration = augerControl.getDuration();
    event.alarmTriggered = true;
    strncpy(event.alarmReason, augerControl.getAlarmReason(), sizeof(event.alarmReason) - 1);

    // Save to history
    storage.addFeedEvent(event);

    // Send Telegram alarm
    if (config.telegramEnabled) {
        telegramBot->sendAlarm(currentFeedCycle, event.targetWeight,
                               event.actualWeight, event.alarmReason);
    }

    // Enter alarm state
    systemStatus.state = SystemState::ALARM;
    strncpy(systemStatus.lastError, event.alarmReason, sizeof(systemStatus.lastError) - 1);

    Serial.printf("Alarm: %s\n", event.alarmReason);
}
