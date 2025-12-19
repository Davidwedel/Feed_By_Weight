#include "storage.h"
#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

Preferences prefs;

Storage::Storage() {
    _initialized = false;
}

bool Storage::begin() {
    if (!LittleFS.begin(true)) {  // true = format on fail
        Serial.println("LittleFS mount failed");
        return false;
    }

    _initialized = true;
    Serial.println("LittleFS initialized");
    printFileSystemInfo();

    return true;
}

bool Storage::loadConfig(Config& config) {
    prefs.begin("config", true);  // read-only

    // Network
    strlcpy(config.bintracIP, prefs.getString("bintracIP", "192.168.1.100").c_str(), sizeof(config.bintracIP));
    config.bintracDeviceID = prefs.getUChar("bintracID", 1);

    // Schedule - feed times (4 values)
    for (int i = 0; i < 4; i++) {
        String key = "feedTime" + String(i);
        config.feedTimes[i] = prefs.getUShort(key.c_str(), config.feedTimes[i]);
    }

    // Feeding parameters
    config.targetWeight = prefs.getFloat("targetWeight", 50.0);
    config.weightUnit = (WeightUnit)prefs.getUChar("weightUnit", 0);
    config.chainPreRunTime = prefs.getUShort("chainPreRun", 10);

    // Alarm settings
    config.alarmThreshold = prefs.getFloat("alarmThresh", 10.0);
    config.maxRuntime = prefs.getUShort("maxRuntime", 600);

    // Bin filling detection
    config.fillDetectionThreshold = prefs.getFloat("fillThresh", 20.0);
    config.fillSettlingTime = prefs.getUShort("fillSettle", 60);

    // Telegram
    strlcpy(config.telegramToken, prefs.getString("tgToken", "").c_str(), sizeof(config.telegramToken));
    strlcpy(config.telegramChatID, prefs.getString("tgChatID", "").c_str(), sizeof(config.telegramChatID));
    strlcpy(config.telegramAllowedUsers, prefs.getString("tgAllowed", "").c_str(), sizeof(config.telegramAllowedUsers));
    config.telegramEnabled = prefs.getBool("tgEnabled", false);

    // System
    config.autoFeedEnabled = prefs.getBool("autoFeed", true);
    config.timezone = prefs.getChar("timezone", 0);

    prefs.end();

    Serial.println("Config loaded from NVS");
    return true;
}

bool Storage::saveConfig(const Config& config) {
    prefs.begin("config", false);  // read-write

    // Network
    prefs.putString("bintracIP", config.bintracIP);
    prefs.putUChar("bintracID", config.bintracDeviceID);

    // Schedule - feed times (4 values)
    for (int i = 0; i < 4; i++) {
        String key = "feedTime" + String(i);
        prefs.putUShort(key.c_str(), config.feedTimes[i]);
    }

    // Feeding parameters
    prefs.putFloat("targetWeight", config.targetWeight);
    prefs.putUChar("weightUnit", (uint8_t)config.weightUnit);
    prefs.putUShort("chainPreRun", config.chainPreRunTime);

    // Alarm settings
    prefs.putFloat("alarmThresh", config.alarmThreshold);
    prefs.putUShort("maxRuntime", config.maxRuntime);

    // Bin filling detection
    prefs.putFloat("fillThresh", config.fillDetectionThreshold);
    prefs.putUShort("fillSettle", config.fillSettlingTime);

    // Telegram
    prefs.putString("tgToken", config.telegramToken);
    prefs.putString("tgChatID", config.telegramChatID);
    prefs.putString("tgAllowed", config.telegramAllowedUsers);
    prefs.putBool("tgEnabled", config.telegramEnabled);

    // System
    prefs.putBool("autoFeed", config.autoFeedEnabled);
    prefs.putChar("timezone", config.timezone);

    prefs.end();

    Serial.println("Config saved to NVS");
    return true;
}

// Removed configToJson and jsonToConfig - no longer needed with NVS

bool Storage::addFeedEvent(const FeedEvent& event) {
    if (!_initialized) return false;

    // Append to CSV file
    File file = LittleFS.open(HISTORY_FILE, "a");
    if (!file) {
        Serial.println("Failed to open history file");
        return false;
    }

    // Write CSV line: timestamp,cycle,target,actual,duration,alarm,reason
    file.printf("%lu,%d,%.2f,%.2f,%d,%d,%s\n",
                event.timestamp,
                event.feedCycle,
                event.targetWeight,
                event.actualWeight,
                event.duration,
                event.alarmTriggered ? 1 : 0,
                event.alarmReason);

    file.close();

    // TODO: Implement circular buffer (keep only last MAX_HISTORY_ENTRIES)
    // This would require reading the file, removing oldest entries if > MAX_HISTORY_ENTRIES

    return true;
}

bool Storage::getFeedHistory(FeedEvent* events, int& count, int maxCount) {
    if (!_initialized) return false;

    if (!LittleFS.exists(HISTORY_FILE)) {
        count = 0;
        return true;
    }

    File file = LittleFS.open(HISTORY_FILE, "r");
    if (!file) {
        Serial.println("Failed to open history file");
        return false;
    }

    count = 0;
    String line;

    // Read lines and parse (note: this reads from beginning, should read from end for latest)
    while (file.available() && count < maxCount) {
        line = file.readStringUntil('\n');
        line.trim();

        if (line.length() == 0) continue;

        // Parse CSV: timestamp,cycle,target,actual,duration,alarm,reason
        int pos = 0;
        int nextPos;

        nextPos = line.indexOf(',', pos);
        events[count].timestamp = line.substring(pos, nextPos).toInt();
        pos = nextPos + 1;

        nextPos = line.indexOf(',', pos);
        events[count].feedCycle = line.substring(pos, nextPos).toInt();
        pos = nextPos + 1;

        nextPos = line.indexOf(',', pos);
        events[count].targetWeight = line.substring(pos, nextPos).toFloat();
        pos = nextPos + 1;

        nextPos = line.indexOf(',', pos);
        events[count].actualWeight = line.substring(pos, nextPos).toFloat();
        pos = nextPos + 1;

        nextPos = line.indexOf(',', pos);
        events[count].duration = line.substring(pos, nextPos).toInt();
        pos = nextPos + 1;

        nextPos = line.indexOf(',', pos);
        events[count].alarmTriggered = line.substring(pos, nextPos).toInt() == 1;
        pos = nextPos + 1;

        String reason = line.substring(pos);
        strlcpy(events[count].alarmReason, reason.c_str(), sizeof(events[count].alarmReason));

        count++;
    }

    file.close();
    return true;
}

bool Storage::clearHistory() {
    if (!_initialized) return false;

    if (LittleFS.exists(HISTORY_FILE)) {
        return LittleFS.remove(HISTORY_FILE);
    }
    return true;
}

bool Storage::formatFilesystem() {
    return LittleFS.format();
}

void Storage::printFileSystemInfo() {
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();

    Serial.println("=== LittleFS Info ===");
    Serial.printf("Total: %d bytes\n", total);
    Serial.printf("Used: %d bytes\n", used);
    Serial.printf("Free: %d bytes\n", total - used);
    Serial.println("====================");
}
