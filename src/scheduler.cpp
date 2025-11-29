#include "scheduler.h"
#include "config.h"
#include <time.h>

Scheduler::Scheduler() {
    _timeClient = nullptr;
    _initialized = false;
    _timezoneOffset = 0;
    _lastDay = 0;

    for (int i = 0; i < 4; i++) {
        _feedingCompleted[i] = false;
    }
}

void Scheduler::begin(int timezoneOffset) {
    _timezoneOffset = timezoneOffset;

    // Initialize NTP client
    _timeClient = new NTPClient(_ntpUDP, NTP_SERVER, timezoneOffset * 3600, NTP_UPDATE_INTERVAL);
    _timeClient->begin();

    Serial.println("Scheduler initialized");
    Serial.printf("Timezone offset: UTC%+d\n", timezoneOffset);
}

void Scheduler::update() {
    if (!_timeClient) return;

    _timeClient->update();

    // Check for day rollover to reset feeding completions
    checkDayRollover();

    if (_timeClient->isTimeSet() && !_initialized) {
        _initialized = true;
        Serial.println("Time synchronized with NTP");
        char timeStr[32];
        getCurrentTimeStr(timeStr, sizeof(timeStr));
        Serial.printf("Current time: %s\n", timeStr);
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
    if (!_timeClient) return 0;
    return _timeClient->getEpochTime();
}

void Scheduler::getCurrentTimeStr(char* buffer, size_t size) {
    if (!isTimeSynced()) {
        snprintf(buffer, size, "Time not synced");
        return;
    }

    unsigned long epochTime = getCurrentTime();
    time_t rawtime = epochTime;
    struct tm* timeinfo = localtime(&rawtime);

    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", timeinfo);
}

bool Scheduler::isTimeSynced() {
    return _initialized && _timeClient && _timeClient->isTimeSet();
}

uint16_t Scheduler::getCurrentMinutes() {
    if (!_timeClient) return 0;

    // Get hours and minutes from NTP client
    uint8_t hours = _timeClient->getHours();
    uint8_t minutes = _timeClient->getMinutes();

    return timeToMinutes(hours, minutes);
}

void Scheduler::checkDayRollover() {
    if (!isTimeSynced()) return;

    uint8_t currentDay = _timeClient->getDay();

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
