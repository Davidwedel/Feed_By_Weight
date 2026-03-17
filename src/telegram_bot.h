#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include <Arduino.h>
#include <UniversalTelegramBot.h>
#include <WiFiClientSecure.h>
#include "config.h"
#include "types.h"

class TelegramBot {
public:
    TelegramBot(Config& config);

    // Initialize bot
    bool begin();

    // Update - check for messages (call periodically)
    void update();

    // Send alarm message
    void sendAlarm(uint8_t feedCycle, float targetWeight, float actualWeight, const char* reason);

    // Send feeding started message
    void sendFeedingStarted(uint8_t feedCycle, float targetWeight);

    // Send feeding complete message
    void sendFeedingComplete(uint8_t feedCycle, float targetWeight, float weight, uint16_t duration, float totalDispensedToday);

    // Send daily summary
    void sendDailySummary(FeedEvent* events, int count);

    // Send status update
    void sendStatus(const SystemStatus& status, const String& chat_id);

    // Send a simple message (for warnings)
    void sendMessage(const String& text);

    // Check if bot is enabled and configured
    bool isEnabled();

    // Check if status was requested
    bool isStatusRequested() { return _statusRequested; }
    String getStatusRequestChatId() { _statusRequested = false; return _statusRequestChatId; }

    // Check if stop was requested (via /disable)
    bool isStopRequested() { bool r = _stopRequested; _stopRequested = false; return r; }

    // Check if alarm clear was requested (via /clearalarm)
    bool isClearAlarmRequested() { bool r = _clearAlarmRequested; _clearAlarmRequested = false; return r; }

    // Check if a manual feed event was submitted (via /logoldfeed)
    bool isAddFeedRequested() { bool r = _addFeedRequested; _addFeedRequested = false; return r; }
    FeedEvent getAddFeedEvent() { return _pendingFeedEvent; }

    // Check if start feed was requested (via /startfeed)
    bool isStartFeedRequested() { bool r = _startFeedRequested; _startFeedRequested = false; return r; }
    float getStartFeedWeight() { return _startFeedWeight; }

    // Check if daily summary was requested (via /dailysummary)
    bool isDailySummaryRequested() { bool r = _dailySummaryRequested; _dailySummaryRequested = false; return r; }

private:
    Config& _config;
    WiFiClientSecure _client;
    UniversalTelegramBot* _bot;
    unsigned long _lastUpdateTime;
    bool _initialized;
    bool _statusRequested;
    bool _stopRequested;
    bool _clearAlarmRequested;
    bool _addFeedRequested;
    bool _startFeedRequested;
    bool _dailySummaryRequested;
    float _startFeedWeight;
    FeedEvent _pendingFeedEvent;
    String _statusRequestChatId;

    // Handle incoming commands
    void handleNewMessages(int numNewMessages);
    bool isUserAuthorized(const String& chat_id);
};

#endif // TELEGRAM_BOT_H
