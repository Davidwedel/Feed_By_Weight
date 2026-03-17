#include "telegram_bot.h"
#include "config.h"
#include <time.h>

TelegramBot::TelegramBot(Config& config) : _config(config)
{
    _bot = nullptr;
    _initialized = false;
    _lastUpdateTime = 0;
    _statusRequested = false;
    _stopRequested = false;
    _clearAlarmRequested = false;
    _addFeedRequested = false;
    _startFeedRequested = false;
    _dailySummaryRequested = false;
    _startFeedWeight = 0;
    memset(&_pendingFeedEvent, 0, sizeof(_pendingFeedEvent));
    _statusRequestChatId = "";
}

bool TelegramBot::begin() {
    if (!isEnabled()) {
        Serial.println("Telegram bot not configured or disabled");
        return false;
    }

    Serial.println("Initializing Telegram bot over WiFi...");
    _client.setInsecure();

    _bot = new UniversalTelegramBot(_config.telegramToken, _client);

    _initialized = true;
    Serial.println("Telegram bot initialized (WiFiClientSecure)");
    sendMessage("Weight Feeder System Online");

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
             "*FEEDING ALARM*\n\n"
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

void TelegramBot::sendFeedingStarted(uint8_t feedCycle, float targetWeight) {
    if (!isEnabled()) return;

    char message[256];
    snprintf(message, sizeof(message),
             "*Feeding Started*\n\n"
             "Cycle: %d\n"
             "Target: %.2f lbs",
             feedCycle + 1,
             targetWeight);

    sendMessage(message);
}

void TelegramBot::sendFeedingComplete(uint8_t feedCycle, float targetWeight, float weight, uint16_t duration, float totalDispensedToday) {
    if (!isEnabled()) return;

    char message[256];
    snprintf(message, sizeof(message),
             "*Feeding Complete*\n\n"
             "Cycle: %d\n"
             "Targeted: %.2f lbs\n"
             "Dispensed: %.2f lbs\n"
             "Duration: %d:%02d\n"
             "Total Dispensed Today: %.2f lbs",
             feedCycle + 1,
             weight,
             duration / 60, duration % 60,
             totalDispensedToday);

    sendMessage(message);
}

void TelegramBot::sendDailySummary(FeedEvent* events, int count) {
    if (!isEnabled()) return;

    String message = "*Daily Feeding Summary*\n\n";

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
            message += " ALARM";
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
    const char* stateStr[] = {"IDLE", "FEEDING", "ALARM", "MANUAL", "ERROR"};
    const char* stageStr[] = {"STOPPED", "CHAIN_ONLY", "BOTH_RUNNING", "POST_AVERAGING", "COMPLETED", "FAILED", "PAUSED", "TERMINATED"};

    snprintf(message, sizeof(message),
             "*System Status*\n\n"
             "Schedule: %s\n"
             "State: %s\n"
             "Stage: %s\n"
             "Bin Weights:\n"
             "  A: %.2f lbs\n"
             "  B: %.2f lbs\n"
             "  C: %.2f lbs\n"
             "  D: %.2f lbs\n"
             "Total Dispensed Today: %.2f lbs\n"
             "Auger: %s\n"
             "Chain: %s\n"
             "BinTrac: %s\n"
             "Network: %s",
             _config.autoFeedEnabled ? "Enabled" : "Disabled",
             stateStr[(int)status.state],
             stageStr[(int)status.feedingStage],
             status.currentWeight[0],
             status.currentWeight[1],
             status.currentWeight[2],
             status.currentWeight[3],
             status.totalDispensedToday,
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
            _bot->sendMessage(chat_id, "Unauthorized. Contact system administrator.", "");
            continue;
        }

        if (text == "/start") {
            _bot->sendMessage(chat_id,
                       "Welcome to Weight Feeder Control!\n\n"
                       "Available commands:\n"
                       "/status - System status\n"
                       "/disable - Disable auto-feeding\n"
                       "/enable - Enable auto-feeding\n"
                       "/clearalarm - Clear alarm state\n"
                       "/startfeed - Start a feed cycle\n"
                       "  Usage: /startfeed <weight>\n"
                       "/dailysummary - Send today's feeding summary\n"
                       "/logoldfeed - Add manual feed entry\n"
                       "  Usage: /logoldfeed <cycle> <weight> [duration] [HH:MM]", "");
        }
        else if (text == "/status") {
            // Trigger status request
            _statusRequested = true;
            _statusRequestChatId = chat_id;
        }
        else if (text == "/disable") {
            _config.autoFeedEnabled = false;
            _stopRequested = true;
            _bot->sendMessage(chat_id, "Auto-feeding disabled - stopping any active feeding", "");
        }
        else if (text == "/enable") {
            _config.autoFeedEnabled = true;
            _bot->sendMessage(chat_id, "Auto-feeding enabled", "");
        }
        else if (text == "/clearalarm") {
            _clearAlarmRequested = true;
            _bot->sendMessage(chat_id, "Alarm cleared", "");
        }
        else if (text.startsWith("/startfeed")) {
            String args = text.substring(11); // skip "/startfeed "
            args.trim();
            if (args.length() == 0) {
                _bot->sendMessage(chat_id, "Usage: /startfeed <weight>\nExample: /startfeed 25", "");
                continue;
            }
            float weight = args.toFloat();
            if (weight <= 0) {
                _bot->sendMessage(chat_id, "Weight must be greater than 0", "");
                continue;
            }
            _startFeedWeight = weight;
            _startFeedRequested = true;
            char msg[64];
            snprintf(msg, sizeof(msg), "Starting feed: %.2f lbs...", weight);
            _bot->sendMessage(chat_id, msg, "");
        }
        else if (text == "/dailysummary") {
            _dailySummaryRequested = true;
        }
        else if (text.startsWith("/logoldfeed")) {
            // Parse: /logoldfeed <cycle> <weight> [duration_seconds] [HH:MM]
            String args = text.substring(12); // skip "/logoldfeed "
            args.trim();

            if (args.length() == 0) {
                _bot->sendMessage(chat_id,
                    "Usage: /logoldfeed <cycle> <weight> [duration] [HH:MM]\n"
                    "Example: /logoldfeed 2 3.5 95 08:30", "");
                continue;
            }

            // Split by spaces
            int argc = 0;
            String argv[4];
            int start = 0;
            for (int a = 0; a < 4 && start < (int)args.length(); a++) {
                int space = args.indexOf(' ', start);
                if (space == -1) {
                    argv[a] = args.substring(start);
                    argc = a + 1;
                    break;
                }
                argv[a] = args.substring(start, space);
                argc = a + 1;
                start = space + 1;
            }

            if (argc < 2) {
                _bot->sendMessage(chat_id, "Need at least cycle and weight.\nUsage: /logoldfeed <cycle> <weight> [duration] [HH:MM]", "");
                continue;
            }

            int cycle = argv[0].toInt();
            float weight = argv[1].toFloat();

            if (cycle < 1 || cycle > 4) {
                _bot->sendMessage(chat_id, "Cycle must be 1-4", "");
                continue;
            }
            if (weight <= 0) {
                _bot->sendMessage(chat_id, "Weight must be greater than 0", "");
                continue;
            }

            uint16_t duration = 0;
            if (argc >= 3) {
                duration = argv[2].toInt();
            }

            // Timestamp: default to now, or parse HH:MM
            time_t ts = time(NULL);
            if (argc >= 4) {
                int colon = argv[3].indexOf(':');
                if (colon > 0) {
                    int hh = argv[3].substring(0, colon).toInt();
                    int mm = argv[3].substring(colon + 1).toInt();
                    if (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59) {
                        struct tm timeinfo;
                        localtime_r(&ts, &timeinfo);
                        timeinfo.tm_hour = hh;
                        timeinfo.tm_min = mm;
                        timeinfo.tm_sec = 0;
                        ts = mktime(&timeinfo);
                    } else {
                        _bot->sendMessage(chat_id, "Invalid time format. Use HH:MM (e.g. 08:30)", "");
                        continue;
                    }
                } else {
                    _bot->sendMessage(chat_id, "Invalid time format. Use HH:MM (e.g. 08:30)", "");
                    continue;
                }
            }

            // Build the event
            memset(&_pendingFeedEvent, 0, sizeof(_pendingFeedEvent));
            _pendingFeedEvent.timestamp = ts;
            _pendingFeedEvent.feedCycle = cycle - 1; // store 0-based
            _pendingFeedEvent.targetWeight = weight;
            _pendingFeedEvent.actualWeight = weight;
            _pendingFeedEvent.duration = duration;
            _pendingFeedEvent.alarmTriggered = false;
            strncpy(_pendingFeedEvent.alarmReason, "Manual", sizeof(_pendingFeedEvent.alarmReason) - 1);
            _addFeedRequested = true;

            // Confirm to user
            char confirm[128];
            snprintf(confirm, sizeof(confirm),
                     "Feed entry added: Cycle %d, %.2f lbs, %d:%02d",
                     cycle, weight, duration / 60, duration % 60);
            _bot->sendMessage(chat_id, confirm, "");
        }
        else {
            _bot->sendMessage(chat_id, "Unknown command. Send /start for help.", "");
        }
    }
}
