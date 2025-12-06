#include "scheduler.h"
#include "config.h"
#include <time.h>
#include <sys/time.h>

Scheduler::Scheduler() {
    _initialized = false;
    _timezoneOffset = 0;
    _lastDay = 0;

    for (int i = 0; i < 4; i++) {
        _feedingCompleted[i] = false;
    }
}

void Scheduler::begin(int timezoneOffset) {
    _timezoneOffset = timezoneOffset;

    // Configure SNTP (non-blocking time sync for ESP32)
    // Format: GMT offset in seconds, daylight offset, NTP server
    configTime(timezoneOffset * 3600, 0, NTP_SERVER, "time.nist.gov");

    Serial.println("SNTP time sync initialized (non-blocking)");
    Serial.printf("Timezone offset: UTC%+d\n", timezoneOffset);
    Serial.println("Waiting for time sync...");
}

void Scheduler::update() {
    // Check for time sync status (non-blocking)
    static bool firstSync = false;
    if (!firstSync && isTimeSynced()) {
        firstSync = true;
        _initialized = true;
        Serial.println("✓ Time synchronized with NTP");
        char timeStr[32];
        getCurrentTimeStr(timeStr, sizeof(timeStr));
        Serial.printf("Current time: %s (timestamp: %lu)\n", timeStr, getCurrentTime());
    }

    // Check for day rollover to reset feeding completions
    if (isTimeSynced()) {
        checkDayRollover();
    }
}

bool Scheduler::shouldFeed(const uint16_t feedTimes[4], uint8_t& feedCycle) {
    if (!isTimeSynced()) {
        return false;
    }

    uint16_t currentMinutes = getCurrentMinutes();

    // Check each feeding time
    for (int i = 0; i < 4; i++) {
        // Skip if already completed today
        if (_feedingCompleted[i]) {
            continue;
        }

        // Check if current time matches feeding time (within 1 minute window)
        if (currentMinutes >= feedTimes[i] && currentMinutes < feedTimes[i] + 1) {
            feedCycle = i;
            return true;
        }
    }

    return false;
}

void Scheduler::markFeedingComplete(uint8_t feedCycle) {
    if (feedCycle < 4) {
        _feedingCompleted[feedCycle] = true;
        Serial.printf("Feeding cycle %d marked complete\n", feedCycle);
    }
}

unsigned long Scheduler::getCurrentTime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

void Scheduler::getCurrentTimeStr(char* buffer, size_t size) {
    if (!isTimeSynced()) {
        snprintf(buffer, size, "Time not synced");
        return;
    }

    time_t now = getCurrentTime();
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

bool Scheduler::isTimeSynced() {
    struct tm timeinfo;
    time_t now = getCurrentTime();
    localtime_r(&now, &timeinfo);

    // If year is less than 2020, time is not synced yet
    return (timeinfo.tm_year + 1900) >= 2020;
}

uint16_t Scheduler::getCurrentMinutes() {
    struct tm timeinfo;
    time_t now = getCurrentTime();
    localtime_r(&now, &timeinfo);

    return timeToMinutes(timeinfo.tm_hour, timeinfo.tm_min);
}

void Scheduler::checkDayRollover() {
    if (!isTimeSynced()) return;

    struct tm timeinfo;
    time_t now = getCurrentTime();
    localtime_r(&now, &timeinfo);

    uint8_t currentDay = timeinfo.tm_mday;

    if (_lastDay == 0) {
        _lastDay = currentDay;
        return;
    }

    if (currentDay != _lastDay) {
        // New day - reset all feeding completions
        Serial.println("New day detected - resetting feeding schedule");
        for (int i = 0; i < 4; i++) {
            _feedingCompleted[i] = false;
        }
        _lastDay = currentDay;
    }
}

uint16_t Scheduler::timeToMinutes(uint8_t hour, uint8_t minute) {
    return hour * 60 + minute;
}

void Scheduler::minutesToTime(uint16_t minutes, uint8_t& hour, uint8_t& minute) {
    hour = minutes / 60;
    minute = minutes % 60;
}
