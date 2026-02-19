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
    PAUSED_FOR_FILL,
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
    float dailyTotal = 200.0;                          // total lbs to dispense per day
    float feedAmounts[4] = {50.0, 50.0, 50.0, 50.0};  // per-feeding target (last active auto-calculates)
    uint8_t numFeedings = 4;                           // number of active feedings (1-4)
    WeightUnit weightUnit = WeightUnit::POUNDS;
    uint16_t chainPreRunTime = 10;  // seconds

    // Alarm settings
    float alarmThreshold = 10.0;  // weight per minute
    uint16_t maxRuntime = 600;    // maximum feeding time in seconds

    // Bin filling detection
    float fillDetectionRate = 20.0;   // lb/min increase rate to trigger fill pause
    uint16_t fillSettlingTime = 1;    // minutes to wait after filling stops

    // Weight fluctuation threshold
    float weightFluctuationThreshold = 2.0;  // lbs - ignore weight changes smaller than this

    // Telegram settings
    char telegramToken[50] = "";
    char telegramChatID[20] = "";
    char telegramAllowedUsers[200] = "";  // Comma-separated usernames
    bool telegramEnabled = false;

    // WiFi settings (used for Telegram SSL only)
    char wifiSSID[33] = "";
    char wifiPassword[65] = "";

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

// Weight log circular buffer
#define WEIGHT_LOG_SIZE 60

struct WeightLogEntry {
    unsigned long timestamp;  // millis()
    float weights[4];         // EMA-smoothed bin weights
    float total;              // sum of smoothed weights
};

struct WeightLog {
    WeightLogEntry entries[WEIGHT_LOG_SIZE];
    int head = 0;      // next write position
    int count = 0;     // number of valid entries

    void add(unsigned long ts, const float w[4], float total) {
        entries[head].timestamp = ts;
        for (int i = 0; i < 4; i++) entries[head].weights[i] = w[i];
        entries[head].total = total;
        head = (head + 1) % WEIGHT_LOG_SIZE;
        if (count < WEIGHT_LOG_SIZE) count++;
    }
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
    float totalDispensedToday;
    bool augerRunning;
    bool chainRunning;
    bool bintracConnected;
    bool networkConnected;
    char currentTime[32];
    char lastError[128];
    unsigned long lastBintracUpdate;
};

#endif // TYPES_H
