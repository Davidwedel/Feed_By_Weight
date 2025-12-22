#include "scheduler.h"
#include "config.h"
#include <time.h>
#include <sys/time.h>
#include <EthernetUdp.h>

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
    Serial.printf("Scheduler initialized with timezone offset: UTC%+d\n", timezoneOffset);
}

void Scheduler::startNTPSync() {
    Serial.println("Starting NTP sync via UDP (UTC time)");

    EthernetUDP udp;
    const int NTP_PACKET_SIZE = 48;
    byte packetBuffer[NTP_PACKET_SIZE];

    // Try up to 3 times
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            Serial.printf("Retry attempt %d...\n", attempt + 1);
            delay(2000);
        }

        // Clear buffer
        memset(packetBuffer, 0, NTP_PACKET_SIZE);

        // Initialize NTP request packet (LI=0, VN=3, Mode=3)
        packetBuffer[0] = 0b11100011;   // LI, Version, Mode
        packetBuffer[1] = 0;            // Stratum
        packetBuffer[2] = 6;            // Polling Interval
        packetBuffer[3] = 0xEC;         // Peer Clock Precision
        // bytes 4-11 are zero (Root Delay & Root Dispersion)
        packetBuffer[12] = 49;          // Reference ID
        packetBuffer[13] = 0x4E;
        packetBuffer[14] = 49;
        packetBuffer[15] = 52;

        // Send NTP request
        udp.begin(8888);  // Local port
        if (udp.beginPacket(NTP_SERVER, 123) == 0) {
            Serial.println("Failed to start UDP packet");
            udp.stop();
            continue;
        }

        udp.write(packetBuffer, NTP_PACKET_SIZE);
        if (udp.endPacket() == 0) {
            Serial.println("Failed to send UDP packet");
            udp.stop();
            continue;
        }

        Serial.print("NTP request sent, waiting for response");

        // Wait for response (up to 5 seconds)
        unsigned long startWait = millis();
        while (millis() - startWait < 5000) {
            int size = udp.parsePacket();
            if (size >= NTP_PACKET_SIZE) {
                Serial.println(" received!");

                udp.read(packetBuffer, NTP_PACKET_SIZE);
                udp.stop();

                // Extract timestamp (bytes 40-43)
                unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
                unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
                unsigned long secsSince1900 = highWord << 16 | lowWord;

                // Convert to Unix timestamp (seconds since Jan 1 1970)
                const unsigned long seventyYears = 2208988800UL;
                unsigned long epoch = secsSince1900 - seventyYears;

                // Set system time
                struct timeval tv;
                tv.tv_sec = epoch;
                tv.tv_usec = 0;
                settimeofday(&tv, NULL);

                _initialized = true;
                Serial.println("✓ Time synchronized with NTP");
                char timeStr[32];
                getCurrentTimeStr(timeStr, sizeof(timeStr));
                Serial.printf("Current time: %s (timestamp: %lu)\n", timeStr, epoch);
                return;
            }
            delay(100);
            Serial.print(".");
        }
        Serial.println(" timeout");
        udp.stop();
    }

    Serial.println("✗ NTP sync failed after 3 attempts");
    Serial.println("Scheduled feeding will not work without time sync!");
}

void Scheduler::update() {
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

    // Get UTC time and apply manual offset
    time_t now = getCurrentTime() + (_timezoneOffset * 3600);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);

    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

bool Scheduler::isTimeSynced() {
    struct tm timeinfo;
    time_t now = getCurrentTime();
    gmtime_r(&now, &timeinfo);

    // If year is less than 2020, time is not synced yet
    return (timeinfo.tm_year + 1900) >= 2020;
}

uint16_t Scheduler::getCurrentMinutes() {
    struct tm timeinfo;
    // Get UTC time and apply manual offset
    time_t now = getCurrentTime() + (_timezoneOffset * 3600);
    gmtime_r(&now, &timeinfo);

    return timeToMinutes(timeinfo.tm_hour, timeinfo.tm_min);
}

void Scheduler::checkDayRollover() {
    if (!isTimeSynced()) return;

    struct tm timeinfo;
    // Get UTC time and apply manual offset
    time_t now = getCurrentTime() + (_timezoneOffset * 3600);
    gmtime_r(&now, &timeinfo);

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
