#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include <Arduino.h>
#include <UniversalTelegramBot.h>
#include <SSLClient.h>
#include <EthernetClient.h>
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
    void sendStatus(const SystemStatus& status);

    // Check if bot is enabled and configured
    bool isEnabled();

private:
    Config& _config;
    EthernetClient _ethClient;
    SSLClient _client;
    UniversalTelegramBot* _bot;
    unsigned long _lastUpdateTime;
    bool _initialized;

    // Handle incoming commands
    void handleNewMessages(int numNewMessages);
    void sendMessage(const String& text);
};

#endif // TELEGRAM_BOT_H
