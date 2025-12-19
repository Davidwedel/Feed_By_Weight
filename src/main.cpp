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
bool networkConnected = false;

// Function declarations
void setupNetwork();
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
    systemStatus.flowRate = 0;
    systemStatus.augerRunning = false;
    systemStatus.chainRunning = false;
    systemStatus.bintracConnected = false;
    systemStatus.networkConnected = networkConnected;
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

        // Send status if requested
        if (telegramBot->isStatusRequested()) {
            String chatId = telegramBot->getStatusRequestChatId();
            telegramBot->sendStatus(systemStatus, chatId);
        }
    }

    // Handle web server requests
    webServer->handleClient();

    // Read bin weights when feeding, or periodically in idle (every 10 seconds to keep connection alive)
    bool needWeightReading = (systemStatus.state == SystemState::FEEDING ||
                              systemStatus.state == SystemState::WAITING_FOR_SCHEDULE);

    unsigned long readInterval = needWeightReading ? WEIGHT_CHECK_INTERVAL : 10000;

    if (millis() - lastBintracRead > readInterval) {
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

void setupNetwork() {
    Serial.println("Initializing W5500 Ethernet...");
    Serial.println("Pin configuration:");
    Serial.printf("  CS:   GPIO %d\n", W5500_CS_PIN);
    Serial.printf("  MISO: GPIO %d\n", W5500_MISO_PIN);
    Serial.printf("  MOSI: GPIO %d\n", W5500_MOSI_PIN);
    Serial.printf("  SCK:  GPIO %d\n", W5500_SCK_PIN);
    Serial.printf("  RST:  GPIO %d\n", W5500_RESET_PIN);

    // Hardware reset W5500
    pinMode(W5500_RESET_PIN, OUTPUT);
    digitalWrite(W5500_RESET_PIN, LOW);
    delay(50);
    digitalWrite(W5500_RESET_PIN, HIGH);
    delay(200);

    // Initialize SPI with custom pins
    SPI.begin(W5500_SCK_PIN, W5500_MISO_PIN, W5500_MOSI_PIN, W5500_CS_PIN);

    // Initialize Ethernet library with CS pin
    Ethernet.init(W5500_CS_PIN);

    // MAC address
    byte mac[] = W5500_MAC;

    // First, get DHCP to learn network configuration
    Serial.println("Getting network info via DHCP...");
    Ethernet.begin(mac);
    delay(5000);  // Wait for DHCP to complete

    IPAddress dhcpIP = Ethernet.localIP();

    // Check if we got a valid private network IP from DHCP
    if (!((dhcpIP[0] == 192 && dhcpIP[1] == 168) ||
          (dhcpIP[0] == 10) ||
          (dhcpIP[0] == 172 && dhcpIP[1] >= 16 && dhcpIP[1] <= 31))) {
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

    // Give W5500 time to initialize
    delay(1000);

    // Read network configuration from DHCP
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

    // Now reconnect with static IP ending in .205, using learned network config
    IPAddress staticIP(dhcpIP[0], dhcpIP[1], dhcpIP[2], 205);

    Serial.print("Reconnecting with static IP: ");
    Serial.println(staticIP);

    Ethernet.begin(mac, staticIP, dns, gateway, subnet);
    delay(1000);

    // Verify connection
    Serial.println("Ethernet connected with static IP");
    Serial.print("Final IP Address: ");
    Serial.println(Ethernet.localIP());
    networkConnected = true;
}

void updateBinWeights() {
    if (bintrac.readAllBins(systemStatus.currentWeight)) {
        systemStatus.bintracConnected = true;
        systemStatus.lastBintracUpdate = millis();

        // Debug: print weights every 5 reads
        static int readCount = 0;
        if (++readCount % 5 == 0) {
            Serial.printf("Bins: A=%.0f B=%.0f C=%.0f D=%.0f\n",
                systemStatus.currentWeight[0],
                systemStatus.currentWeight[1],
                systemStatus.currentWeight[2],
                systemStatus.currentWeight[3]);
        }
    } else {
        systemStatus.bintracConnected = false;
        Serial.printf("BinTrac read failed: %s\n", bintrac.getLastError());

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
    systemStatus.flowRate = augerControl.getFlowRate();

    // Update network connection status (check if we have a valid IP)
    IPAddress ip = Ethernet.localIP();
    networkConnected = (ip[0] != 0);
    systemStatus.networkConnected = networkConnected;
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
                    augerControl.startFeeding(config.targetWeight, config.chainPreRunTime, config.maxRuntime, config.fillDetectionThreshold, config.fillSettlingTime);
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

            // Check for warnings and send to Telegram
            const char* warning = augerControl.getNewWarning();
            if (warning != nullptr && config.telegramEnabled) {
                String msg = String("🔔 Feed Cycle ") + String(currentFeedCycle + 1) + "\n" + String(warning);
                telegramBot->sendMessage(msg);
            }

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
    event.timestamp = scheduler.isTimeSynced() ? scheduler.getCurrentTime() : 0;
    event.feedCycle = currentFeedCycle;
    event.targetWeight = config.targetWeight;
    event.actualWeight = augerControl.getWeightDispensed();
    event.duration = augerControl.getDuration();
    event.alarmTriggered = false;
    strcpy(event.alarmReason, "");

    // Save to history
    storage.addFeedEvent(event);

    if (!scheduler.isTimeSynced()) {
        Serial.println("Warning: Time not synced, event saved with timestamp 0");
    }

    // Mark feeding as complete for this cycle
    scheduler.markFeedingComplete(currentFeedCycle);

    // Send Telegram notification
    if (config.telegramEnabled) {
        telegramBot->sendFeedingComplete(currentFeedCycle, event.actualWeight, event.duration);
    }

    // Reset auger control state for next feeding
    augerControl.stopAll();

    // Return to idle state
    systemStatus.state = SystemState::IDLE;

    Serial.printf("Dispensed: %.2f lbs in %d seconds\n", event.actualWeight, event.duration);
}

void handleFeedingFailed() {
    Serial.println("=== Feeding Failed ===");

    // Create feed event record with alarm
    FeedEvent event;
    event.timestamp = scheduler.isTimeSynced() ? scheduler.getCurrentTime() : 0;
    event.feedCycle = currentFeedCycle;
    event.targetWeight = config.targetWeight;
    event.actualWeight = augerControl.getWeightDispensed();
    event.duration = augerControl.getDuration();
    event.alarmTriggered = true;
    strncpy(event.alarmReason, augerControl.getAlarmReason(), sizeof(event.alarmReason) - 1);

    // Save to history
    storage.addFeedEvent(event);

    if (!scheduler.isTimeSynced()) {
        Serial.println("Warning: Time not synced, event saved with timestamp 0");
    }

    // Send Telegram alarm
    if (config.telegramEnabled) {
        telegramBot->sendAlarm(currentFeedCycle, event.targetWeight,
                               event.actualWeight, event.alarmReason);
    }

    // Reset auger control state
    augerControl.stopAll();

    // Enter alarm state
    systemStatus.state = SystemState::ALARM;
    strncpy(systemStatus.lastError, event.alarmReason, sizeof(systemStatus.lastError) - 1);

    Serial.printf("Alarm: %s\n", event.alarmReason);
}
