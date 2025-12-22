#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <Arduino.h>
#include <time.h>
#include "types.h"

class Scheduler {
public:
    Scheduler();

    // Initialize SNTP time sync
    void begin(int timezoneOffset = 0);

    // Start NTP sync (call after network is fully ready)
    void startNTPSync();

    // Update time sync status (non-blocking)
    void update();

    // Check if it's time to feed
    // Returns true and sets feedCycle (0-3) if a feeding should start
    bool shouldFeed(const uint16_t feedTimes[4], uint8_t& feedCycle);

    // Mark feeding as completed for this cycle
    void markFeedingComplete(uint8_t feedCycle);

    // Get current time
    unsigned long getCurrentTime();  // Unix timestamp
    void getCurrentTimeStr(char* buffer, size_t size);  // Human readable

    // Time conversion utilities
    static uint16_t timeToMinutes(uint8_t hour, uint8_t minute);
    static void minutesToTime(uint16_t minutes, uint8_t& hour, uint8_t& minute);

    // Check if time is synchronized
    bool isTimeSynced();

private:
    bool _initialized;
    int _timezoneOffset;  // hours

    // Track which feedings have been completed today
    bool _feedingCompleted[4];
    uint8_t _lastDay;  // To reset completions at midnight

    // Get current time in minutes from midnight (local time)
    uint16_t getCurrentMinutes();

    // Reset daily tracking at midnight
    void checkDayRollover();
};

#endif // SCHEDULER_H
