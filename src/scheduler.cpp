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

/**
 * NTP TIME SYNCHRONIZATION
 *
 * Contacts NTP server to get current UTC time and sets the ESP32 system clock.
 * Retries up to 3 times on failure. Uses EthernetUDP (works for both Ethernet and WiFi modes).
 *
 * NTP Protocol:
 * - Send 48-byte request packet to NTP server on port 123 (UDP)
 * - Receive 48-byte response containing timestamp in seconds since Jan 1, 1900
 * - Convert NTP timestamp to Unix timestamp (seconds since Jan 1, 1970)
 * - Set ESP32 system clock using settimeofday()
 *
 * After successful sync, scheduled feeding will work and timestamps will be accurate.
 */
void Scheduler::startNTPSync() {
    Serial.println("Starting NTP sync via UDP (UTC time)");

    EthernetUDP udp;
    const int NTP_PACKET_SIZE = 48;
    byte packetBuffer[NTP_PACKET_SIZE];

    // Try up to 3 times (NTP over internet can be unreliable)
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            Serial.printf("Retry attempt %d...\n", attempt + 1);
            delay(2000);  // Wait before retrying
        }

        // Clear buffer
        memset(packetBuffer, 0, NTP_PACKET_SIZE);

        // ========================================
        // Build NTP Request Packet
        // ========================================
        // NTP packet structure: https://tools.ietf.org/html/rfc5905
        packetBuffer[0] = 0b11100011;   // LI=3 (unsync), VN=4 (version), Mode=3 (client)
        packetBuffer[1] = 0;            // Stratum (unspecified)
        packetBuffer[2] = 6;            // Polling Interval
        packetBuffer[3] = 0xEC;         // Peer Clock Precision
        // bytes 4-11 are zero (Root Delay & Root Dispersion)
        packetBuffer[12] = 49;          // Reference ID
        packetBuffer[13] = 0x4E;
        packetBuffer[14] = 49;
        packetBuffer[15] = 52;

        // ========================================
        // Send NTP Request
        // ========================================
        udp.begin(8888);  // Local port for receiving response
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

        // ========================================
        // Wait for NTP Response (up to 5 seconds)
        // ========================================
        unsigned long startWait = millis();
        while (millis() - startWait < 5000) {
            int size = udp.parsePacket();
            if (size >= NTP_PACKET_SIZE) {
                Serial.println(" received!");

                udp.read(packetBuffer, NTP_PACKET_SIZE);
                udp.stop();

                // ========================================
                // Extract Timestamp from NTP Response
                // ========================================
                // Transmit Timestamp is at bytes 40-43 (32-bit seconds) + 44-47 (32-bit fraction)
                // We only care about the seconds field
                unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
                unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
                unsigned long secsSince1900 = highWord << 16 | lowWord;

                // Convert NTP timestamp (seconds since 1900) to Unix timestamp (seconds since 1970)
                const unsigned long seventyYears = 2208988800UL;  // Seconds between 1900 and 1970
                unsigned long epoch = secsSince1900 - seventyYears;

                // ========================================
                // Set ESP32 System Clock
                // ========================================
                struct timeval tv;
                tv.tv_sec = epoch;
                tv.tv_usec = 0;  // Microseconds (we don't have sub-second precision from NTP packet)
                settimeofday(&tv, NULL);

                _initialized = true;
                Serial.println("Time synchronized with NTP");
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

    Serial.println("NTP sync failed after 3 attempts");
    Serial.println("Scheduled feeding will not work without time sync!");
}

void Scheduler::update() {
    // Check for day rollover to reset feeding completions
    if (!isTimeSynced()) return;

    struct tm timeinfo;
    // Get UTC time and apply manual timezone offset to get local time
    time_t now = getCurrentTime() + (_timezoneOffset * 3600);
    gmtime_r(&now, &timeinfo);

    uint8_t currentDay = timeinfo.tm_mday;  // Day of month (1-31)

    if (_lastDay == 0) {
        // First call after boot - initialize tracking
        _lastDay = currentDay;
        return;
    }

    if (currentDay != _lastDay) {
        // New day detected - reset all feeding completion flags
        Serial.println("New day detected - resetting feeding schedule");
        for (int i = 0; i < 4; i++) {
            _feedingCompleted[i] = false;
        }
        _lastDay = currentDay;
    }
}

/**
 * CHECK IF IT'S TIME TO FEED
 *
 * Called every loop from main.cpp while in IDLE state. Checks if current time (minutes from midnight)
 * matches any of the configured feed times that haven't been completed today yet.
 *
 * Uses 1-minute trigger window: if feedTime is 360 (6:00 AM), triggers when currentMinutes is 360-360.
 * This prevents multiple triggers for the same feeding.
 *
 * Returns: true if feeding should start, with feedCycle set to the index (0-3)
 */
bool Scheduler::shouldFeed(const uint16_t feedTimes[4], uint8_t numFeedings, uint8_t& feedCycle) {
    if (!isTimeSynced()) {
        return false;  // Can't schedule without accurate time
    }

    uint16_t currentMinutes = getCurrentMinutes();  // Minutes from midnight in local timezone

    // Check each active feeding time
    for (int i = 0; i < numFeedings; i++) {
        // Skip if already completed today
        if (_feedingCompleted[i]) {
            continue;
        }

        // Check if current time is within 1-minute trigger window
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

/**
 * GET CURRENT TIME AS FORMATTED STRING
 *
 * Returns local time (UTC + timezone offset) in format: "YYYY-MM-DD HH:MM:SS"
 * Used for display in web UI and serial logging.
 */
void Scheduler::getCurrentTimeStr(char* buffer, size_t size) {
    if (!isTimeSynced()) {
        snprintf(buffer, size, "Time not synced");
        return;
    }

    // Get UTC time and apply manual timezone offset
    time_t now = getCurrentTime() + (_timezoneOffset * 3600);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);

    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

/**
 * CHECK IF TIME HAS BEEN SYNCED
 *
 * After boot, ESP32 system clock starts at Jan 1, 1970. After NTP sync, it will be >= 2020.
 * We check if year >= 2020 to determine if NTP sync has succeeded.
 */
bool Scheduler::isTimeSynced() {
    struct tm timeinfo;
    time_t now = getCurrentTime();
    gmtime_r(&now, &timeinfo);

    // If year is less than 2020, time hasn't been synced yet (still at epoch)
    return (timeinfo.tm_year + 1900) >= 2020;
}

/**
 * GET CURRENT TIME AS MINUTES FROM MIDNIGHT
 *
 * Returns: 0-1439 (0 = 00:00, 360 = 06:00, 1439 = 23:59)
 * Used for comparing against configured feed times.
 */
uint16_t Scheduler::getCurrentMinutes() {
    struct tm timeinfo;
    // Get UTC time and apply manual timezone offset to get local time
    time_t now = getCurrentTime() + (_timezoneOffset * 3600);
    gmtime_r(&now, &timeinfo);

    return timeToMinutes(timeinfo.tm_hour, timeinfo.tm_min);
}

uint16_t Scheduler::timeToMinutes(uint8_t hour, uint8_t minute) {
    return hour * 60 + minute;
}

void Scheduler::minutesToTime(uint16_t minutes, uint8_t& hour, uint8_t& minute) {
    hour = minutes / 60;
    minute = minutes % 60;
}
