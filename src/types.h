#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>

// Weight units enumeration
enum class WeightUnit {
    POUNDS,
    KILOGRAMS
};

// System state enumeration
enum class SystemState {
    IDLE,
    WAITING_FOR_SCHEDULE,
    FEEDING,
    ALARM,
    MANUAL_OVERRIDE,
    ERROR
};

// Feeding stage enumeration
enum class FeedingStage {
    STOPPED,
    CHAIN_ONLY,
    BOTH_RUNNING,
    COMPLETED,
    FAILED
};

// Configuration structure
struct Config {
    // Network settings
    char bintracIP[16] = "192.168.1.100";
    uint8_t bintracDeviceID = 1;  // Device ID from HouseLink discovery

    // Feeding schedule (minutes from midnight)
    uint16_t feedTimes[4] = {360, 720, 1080, 1440};  // 6am, 12pm, 6pm, 12am

    // Feeding parameters
    float targetWeight = 50.0;
    WeightUnit weightUnit = WeightUnit::POUNDS;
    uint16_t chainPreRunTime = 10;  // seconds

    // Alarm settings
    float alarmThreshold = 10.0;  // weight per minute
    uint16_t maxRuntime = 600;    // maximum feeding time in seconds

    // Telegram settings
    char telegramToken[50] = "";
    char telegramChatID[20] = "";
    char telegramAllowedUsers[200] = "";  // Comma-separated usernames
    bool telegramEnabled = false;

    // System settings
    bool autoFeedEnabled = true;
    int8_t timezone = 0;  // UTC offset in hours (-12 to +12)
};

// Feed event record
struct FeedEvent {
    unsigned long timestamp;
    uint8_t feedCycle;        // 0-3 for the 4 daily cycles
    float targetWeight;
    float actualWeight;
    uint16_t duration;        // seconds
    bool alarmTriggered;
    char alarmReason[64];
};

// Real-time status
struct SystemStatus {
    SystemState state;
    FeedingStage feedingStage;
    unsigned long feedStartTime;
    float currentWeight[4];   // A, B, C, D bins
    float weightAtStart;
    float weightDispensed;
    float flowRate;           // lbs/min
    bool augerRunning;
    bool chainRunning;
    bool bintracConnected;
    bool networkConnected;
    char lastError[128];
    unsigned long lastBintracUpdate;
};

#endif // TYPES_H
