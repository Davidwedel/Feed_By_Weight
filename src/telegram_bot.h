#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include <Arduino.h>
#include <UniversalTelegramBot.h>
#include <SSLClient.h>
#include <Ethernet.h>
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

    // Send feeding complete message
    void sendFeedingComplete(uint8_t feedCycle, float weight, uint16_t duration);

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

private:
    Config& _config;
    EthernetClient _ethClient;
    SSLClient _client;
    UniversalTelegramBot* _bot;
    unsigned long _lastUpdateTime;
    bool _initialized;
    bool _statusRequested;
    String _statusRequestChatId;

    // Handle incoming commands
    void handleNewMessages(int numNewMessages);
    bool isUserAuthorized(const String& chat_id);
};

#endif // TELEGRAM_BOT_H
