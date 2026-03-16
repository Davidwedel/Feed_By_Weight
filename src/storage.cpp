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
    config.dailyTotal = prefs.getFloat("dailyTotal", 200.0);
    config.numFeedings = prefs.getUChar("numFeedings", 4);
    for (int i = 0; i < 4; i++) {
        String key = "feedAmt" + String(i);
        config.feedAmounts[i] = prefs.getFloat(key.c_str(), 50.0);
    }
    config.weightUnit = (WeightUnit)prefs.getUChar("weightUnit", 0);
    config.chainPreRunTime = prefs.getUShort("chainPreRun", 10);

    // Alarm settings
    config.alarmThreshold = prefs.getFloat("alarmThresh", 10.0);
    config.maxRuntime = prefs.getUShort("maxRuntime", 600);

    // Bin filling detection
    config.fillSettlingTime = prefs.getUShort("fillSettle", 1);

    // Telegram
    strlcpy(config.telegramToken, prefs.getString("tgToken", "").c_str(), sizeof(config.telegramToken));
    strlcpy(config.telegramChatID, prefs.getString("tgChatID", "").c_str(), sizeof(config.telegramChatID));
    strlcpy(config.telegramAllowedUsers, prefs.getString("tgAllowed", "").c_str(), sizeof(config.telegramAllowedUsers));
    config.telegramEnabled = prefs.getBool("tgEnabled", false);

    // WiFi (for Telegram SSL)
    strlcpy(config.wifiSSID, prefs.getString("wifiSSID", "").c_str(), sizeof(config.wifiSSID));
    strlcpy(config.wifiPassword, prefs.getString("wifiPass", "").c_str(), sizeof(config.wifiPassword));

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
    prefs.putFloat("dailyTotal", config.dailyTotal);
    prefs.putUChar("numFeedings", config.numFeedings);
    for (int i = 0; i < 4; i++) {
        String key = "feedAmt" + String(i);
        prefs.putFloat(key.c_str(), config.feedAmounts[i]);
    }
    prefs.putUChar("weightUnit", (uint8_t)config.weightUnit);
    prefs.putUShort("chainPreRun", config.chainPreRunTime);

    // Alarm settings
    prefs.putFloat("alarmThresh", config.alarmThreshold);
    prefs.putUShort("maxRuntime", config.maxRuntime);

    // Bin filling detection
    prefs.putUShort("fillSettle", config.fillSettlingTime);

    // Telegram
    prefs.putString("tgToken", config.telegramToken);
    prefs.putString("tgChatID", config.telegramChatID);
    prefs.putString("tgAllowed", config.telegramAllowedUsers);
    prefs.putBool("tgEnabled", config.telegramEnabled);

    // WiFi (for Telegram SSL)
    prefs.putString("wifiSSID", config.wifiSSID);
    prefs.putString("wifiPass", config.wifiPassword);

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

    // Create file if it doesn't exist
    if (!LittleFS.exists(HISTORY_FILE)) {
        File create = LittleFS.open(HISTORY_FILE, "w");
        if (!create) {
            Serial.println("Failed to create history file");
            return false;
        }
        create.close();
    }

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

    // Trim to most recent 20 entries
    trimHistory(20);

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

bool Storage::writeRawHistory(const String& csvData) {
    if (!_initialized) return false;

    File file = LittleFS.open(HISTORY_FILE, "w");
    if (!file) {
        Serial.println("Failed to open history file for writing");
        return false;
    }

    file.print(csvData);
    file.close();
    return true;
}

void Storage::trimHistory(int maxEntries) {
    if (!LittleFS.exists(HISTORY_FILE)) return;

    // Read entire file
    File file = LittleFS.open(HISTORY_FILE, "r");
    if (!file) return;
    String content = file.readString();
    file.close();

    // Count lines
    int total = 0;
    int pos = 0;
    while (pos < (int)content.length()) {
        int nl = content.indexOf('\n', pos);
        if (nl == -1) break;
        String line = content.substring(pos, nl);
        line.trim();
        if (line.length() > 0) total++;
        pos = nl + 1;
    }

    if (total <= maxEntries) return;

    // Skip oldest lines, keep the rest
    int skip = total - maxEntries;
    int skipped = 0;
    pos = 0;
    while (skipped < skip && pos < (int)content.length()) {
        int nl = content.indexOf('\n', pos);
        if (nl == -1) break;
        String line = content.substring(pos, nl);
        line.trim();
        if (line.length() > 0) skipped++;
        pos = nl + 1;
    }

    file = LittleFS.open(HISTORY_FILE, "w");
    if (!file) return;
    file.print(content.substring(pos));
    file.close();

    Serial.printf("History trimmed: removed %d oldest entries, kept %d\n", skip, maxEntries);
}

bool Storage::deleteHistoryEntry(time_t timestamp) {
    if (!_initialized) return false;
    if (!LittleFS.exists(HISTORY_FILE)) return true;

    File file = LittleFS.open(HISTORY_FILE, "r");
    if (!file) return false;
    String content = file.readString();
    file.close();

    String newContent = "";
    int pos = 0;
    while (pos < (int)content.length()) {
        int nl = content.indexOf('\n', pos);
        String line;
        if (nl == -1) {
            line = content.substring(pos);
            pos = content.length();
        } else {
            line = content.substring(pos, nl);
            pos = nl + 1;
        }
        line.trim();
        if (line.length() == 0) continue;

        int comma = line.indexOf(',');
        time_t ts = (comma > 0) ? (time_t)line.substring(0, comma).toInt() : 0;
        if (ts != timestamp) {
            newContent += line + "\n";
        }
    }

    file = LittleFS.open(HISTORY_FILE, "w");
    if (!file) return false;
    file.print(newContent);
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

void Storage::saveFeedProgress(float startWeight, float dispensed, float target, uint8_t cycle, unsigned long timestamp) {
    Preferences fp;
    fp.begin("feedprog", false);
    fp.putBool("pfActive", true);
    fp.putFloat("pfStartWt", startWeight);
    fp.putFloat("pfDispensed", dispensed);
    fp.putFloat("pfTargetWt", target);
    fp.putUChar("pfCycle", cycle);
    fp.putULong("pfTimestamp", timestamp);
    fp.end();
}

bool Storage::loadFeedProgress(float& startWeight, float& dispensed, float& target, uint8_t& cycle, unsigned long& timestamp) {
    Preferences fp;
    fp.begin("feedprog", true);
    bool active = fp.getBool("pfActive", false);
    if (active) {
        startWeight = fp.getFloat("pfStartWt", 0);
        dispensed = fp.getFloat("pfDispensed", 0);
        target = fp.getFloat("pfTargetWt", 0);
        cycle = fp.getUChar("pfCycle", 0);
        timestamp = fp.getULong("pfTimestamp", 0);
    }
    fp.end();
    return active;
}

void Storage::clearFeedProgress() {
    Preferences fp;
    fp.begin("feedprog", false);
    fp.clear();
    fp.end();
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
