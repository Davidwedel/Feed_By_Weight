#include "telegram_bot.h"
#include "config.h"
#include <time.h>

TelegramBot::TelegramBot(Config& config) : _config(config),
    _client(_ethClient, nullptr, 0, A0)  // SSLClient with insecure mode
{
    _bot = nullptr;
    _initialized = false;
    _lastUpdateTime = 0;
    _statusRequested = false;
    _statusRequestChatId = "";
}

bool TelegramBot::begin() {
    if (!isEnabled()) {
        Serial.println("Telegram bot not configured or disabled");
        return false;
    }

    Serial.println("Initializing Telegram bot over Ethernet...");
    // Note: Using nullptr trust anchors = no certificate validation (insecure)
    // For production, add proper Telegram API certificates

    // Initialize Telegram bot with SSL client
    _bot = new UniversalTelegramBot(_config.telegramToken, _client);

    _initialized = true;
    Serial.println("Telegram bot initialized (SSL over Ethernet)");
    sendMessage("🤖 Weight Feeder System Online (Ethernet)");

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

void TelegramBot::sendStatus(const SystemStatus& status, const String& chat_id) {
    if (!_bot) return;

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

    _bot->sendMessage(chat_id, message, "Markdown");
    Serial.printf("Telegram status sent to %s\n", chat_id.c_str());
}

bool TelegramBot::isEnabled() {
    return _config.telegramEnabled &&
           strlen(_config.telegramToken) > 0 &&
           strlen(_config.telegramChatID) > 0;
}

void TelegramBot::sendMessage(const String& text) {
    if (!_bot || !isEnabled() || strlen(_config.telegramChatID) == 0) return;

    _bot->sendMessage(_config.telegramChatID, text, "");
    Serial.printf("Telegram sent: %s\n", text.c_str());
}

bool TelegramBot::isUserAuthorized(const String& chat_id) {
    // If no allowed users configured, allow all
    if (strlen(_config.telegramAllowedUsers) == 0) {
        return true;
    }

    // Parse comma-separated list of allowed chat IDs
    String allowedList = String(_config.telegramAllowedUsers);
    allowedList.trim();

    // Check if chat_id is in the list
    int startPos = 0;
    while (startPos < allowedList.length()) {
        int commaPos = allowedList.indexOf(',', startPos);
        if (commaPos == -1) {
            commaPos = allowedList.length();
        }

        String allowedChatId = allowedList.substring(startPos, commaPos);
        allowedChatId.trim();

        if (allowedChatId == chat_id) {
            return true;
        }

        startPos = commaPos + 1;
    }

    return false;
}

void TelegramBot::handleNewMessages(int numNewMessages) {
    for (int i = 0; i < numNewMessages; i++) {
        String chat_id = _bot->messages[i].chat_id;
        String text = _bot->messages[i].text;
        String from_name = _bot->messages[i].from_name;

        Serial.printf("Telegram command: %s from %s (chat_id: %s)\n",
                     text.c_str(), from_name.c_str(), chat_id.c_str());

        // Check if user is authorized (use chat_id)
        if (!isUserAuthorized(chat_id)) {
            Serial.printf("Unauthorized chat_id: %s (%s)\n", chat_id.c_str(), from_name.c_str());
            _bot->sendMessage(chat_id, "⛔ Unauthorized. Contact system administrator.", "");
            continue;
        }

        if (text == "/start") {
            _bot->sendMessage(chat_id,
                       "👋 Welcome to Weight Feeder Control!\n\n"
                       "Available commands:\n"
                       "/status - System status\n"
                       "/disable - Disable auto-feeding\n"
                       "/enable - Enable auto-feeding", "");
        }
        else if (text == "/status") {
            // Trigger status request
            _statusRequested = true;
            _statusRequestChatId = chat_id;
        }
        else if (text == "/disable") {
            _config.autoFeedEnabled = false;
            _bot->sendMessage(chat_id, "✋ Auto-feeding disabled", "");
        }
        else if (text == "/enable") {
            _config.autoFeedEnabled = true;
            _bot->sendMessage(chat_id, "✅ Auto-feeding enabled", "");
        }
        else {
            _bot->sendMessage(chat_id, "❓ Unknown command. Send /start for help.", "");
        }
    }
}
