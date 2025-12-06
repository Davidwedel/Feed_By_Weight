#include "telegram_bot.h"
#include "config.h"
#include <time.h>

TelegramBot::TelegramBot(Config& config) : _config(config)
#ifdef USE_ETHERNET
    , _client(_ethClient, nullptr, 0, A0)  // SSLClient with insecure mode
#endif
{
    _bot = nullptr;
    _initialized = false;
    _lastUpdateTime = 0;
}

bool TelegramBot::begin() {
    if (!isEnabled()) {
        Serial.println("Telegram bot not configured or disabled");
        return false;
    }

#ifdef USE_ETHERNET
    Serial.println("Initializing Telegram bot over Ethernet...");
    // Note: Using nullptr trust anchors = no certificate validation (insecure)
    // For production, add proper Telegram API certificates
#endif

#ifdef USE_WIFI
    Serial.println("Initializing Telegram bot over WiFi...");
    _client.setInsecure();  // Skip certificate validation (insecure)
    // For production, use _client.setCACert() with Telegram API certificate
#endif

    // Initialize Telegram bot with SSL client
    _bot = new UniversalTelegramBot(_config.telegramToken, _client);

    _initialized = true;
#ifdef USE_ETHERNET
    Serial.println("Telegram bot initialized (SSL over Ethernet)");
    sendMessage("🤖 Weight Feeder System Online (Ethernet)");
#endif
#ifdef USE_WIFI
    Serial.println("Telegram bot initialized (SSL over WiFi)");
    sendMessage("🤖 Weight Feeder System Online (WiFi)");
#endif

    return true;
}

void TelegramBot::update() {
    if (!isEnabled() || !_initialized) return;

    // Check for new messages every minute
    if (millis() - _lastUpdateTime > TELEGRAM_UPDATE_INTERVAL) {
        int numNewMessages = _bot->getUpdates(_bot->last_message_received + 1);

        if (numNewMessages > 0) {
            handleNewMessages(numNewMessages);
        }

        _lastUpdateTime = millis();
    }
}

void TelegramBot::sendAlarm(uint8_t feedCycle, float targetWeight, float actualWeight, const char* reason) {
    if (!isEnabled()) return;

    char message[256];
    snprintf(message, sizeof(message),
             "🚨 *FEEDING ALARM*\n\n"
             "Feed Cycle: %d\n"
             "Target: %.2f lbs\n"
             "Actual: %.2f lbs\n"
             "Reason: %s",
             feedCycle + 1,
             targetWeight,
             actualWeight,
             reason);

    sendMessage(message);
}

void TelegramBot::sendFeedingComplete(uint8_t feedCycle, float weight, uint16_t duration) {
    if (!isEnabled()) return;

    char message[256];
    snprintf(message, sizeof(message),
             "✅ *Feeding Complete*\n\n"
             "Cycle: %d\n"
             "Dispensed: %.2f lbs\n"
             "Duration: %d seconds",
             feedCycle + 1,
             weight,
             duration);

    sendMessage(message);
}

void TelegramBot::sendDailySummary(FeedEvent* events, int count) {
    if (!isEnabled()) return;

    String message = "📊 *Daily Feeding Summary*\n\n";

    float totalWeight = 0;
    int alarmCount = 0;

    for (int i = 0; i < count; i++) {
        totalWeight += events[i].actualWeight;
        if (events[i].alarmTriggered) alarmCount++;

        message += "Cycle ";
        message += String(events[i].feedCycle + 1);
        message += ": ";
        message += String(events[i].actualWeight, 2);
        message += " lbs";

        if (events[i].alarmTriggered) {
            message += " ⚠️";
        }
        message += "\n";
    }

    message += "\nTotal: ";
    message += String(totalWeight, 2);
    message += " lbs\n";
    message += "Alarms: ";
    message += String(alarmCount);

    sendMessage(message);
}

void TelegramBot::sendStatus(const SystemStatus& status) {
    if (!isEnabled()) return;

    char message[512];
    const char* stateStr[] = {"IDLE", "WAITING", "FEEDING", "ALARM", "MANUAL", "ERROR"};
    const char* stageStr[] = {"STOPPED", "CHAIN_ONLY", "BOTH_RUNNING", "COMPLETED", "FAILED"};

    snprintf(message, sizeof(message),
             "📈 *System Status*\n\n"
             "State: %s\n"
             "Stage: %s\n"
             "Bin Weights:\n"
             "  A: %.2f lbs\n"
             "  B: %.2f lbs\n"
             "  C: %.2f lbs\n"
             "  D: %.2f lbs\n"
             "Auger: %s\n"
             "Chain: %s\n"
             "BinTrac: %s\n"
             "Network: %s",
             stateStr[(int)status.state],
             stageStr[(int)status.feedingStage],
             status.currentWeight[0],
             status.currentWeight[1],
             status.currentWeight[2],
             status.currentWeight[3],
             status.augerRunning ? "ON" : "OFF",
             status.chainRunning ? "ON" : "OFF",
             status.bintracConnected ? "Connected" : "Disconnected",
             status.networkConnected ? "Connected" : "Disconnected");

    sendMessage(message);
}

bool TelegramBot::isEnabled() {
    return _config.telegramEnabled &&
           strlen(_config.telegramToken) > 0 &&
           strlen(_config.telegramChatID) > 0;
}

void TelegramBot::sendMessage(const String& text) {
    if (!_bot || strlen(_config.telegramChatID) == 0) return;

    _bot->sendMessage(_config.telegramChatID, text, "Markdown");
    Serial.printf("Telegram: %s\n", text.c_str());
}

void TelegramBot::handleNewMessages(int numNewMessages) {
    for (int i = 0; i < numNewMessages; i++) {
        String chat_id = _bot->messages[i].chat_id;
        String text = _bot->messages[i].text;

        Serial.printf("Telegram command: %s from %s\n", text.c_str(), chat_id.c_str());

        if (text == "/start") {
            sendMessage("👋 Welcome to Weight Feeder Control!\n\n"
                       "Available commands:\n"
                       "/status - System status\n"
                       "/lastfeed - Last feeding info\n"
                       "/disable - Disable auto-feeding\n"
                       "/enable - Enable auto-feeding");
        }
        else if (text == "/status") {
            // Status will be sent by main program
            sendMessage("Fetching status...");
        }
        else if (text == "/disable") {
            _config.autoFeedEnabled = false;
            sendMessage("✋ Auto-feeding disabled");
        }
        else if (text == "/enable") {
            _config.autoFeedEnabled = true;
            sendMessage("✅ Auto-feeding enabled");
        }
    }
}
