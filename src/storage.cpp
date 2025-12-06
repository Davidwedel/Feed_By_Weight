#include "storage.h"
#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

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
    if (!_initialized) return false;

    if (!LittleFS.exists(CONFIG_FILE)) {
        Serial.println("Config file not found, using defaults");
        return saveConfig(config);  // Save default config
    }

    File file = LittleFS.open(CONFIG_FILE, "r");
    if (!file) {
        Serial.println("Failed to open config file");
        return false;
    }

    String json = file.readString();
    file.close();

    return jsonToConfig(json, config);
}

bool Storage::saveConfig(const Config& config) {
    if (!_initialized) return false;

    String json;
    if (!configToJson(config, json)) {
        Serial.println("Failed to serialize config");
        return false;
    }

    File file = LittleFS.open(CONFIG_FILE, "w");
    if (!file) {
        Serial.println("Failed to open config file for writing");
        return false;
    }

    file.print(json);
    file.close();

    Serial.println("Config saved successfully");
    return true;
}

bool Storage::configToJson(const Config& config, String& json) {
    JsonDocument doc;

    // Network
    doc["bintracIP"] = config.bintracIP;
    doc["bintracDeviceID"] = config.bintracDeviceID;

    // Schedule
    JsonArray times = doc["feedTimes"].to<JsonArray>();
    for (int i = 0; i < 4; i++) {
        times.add(config.feedTimes[i]);
    }

    // Feeding parameters
    doc["targetWeight"] = config.targetWeight;
    doc["weightUnit"] = (int)config.weightUnit;
    doc["chainPreRunTime"] = config.chainPreRunTime;

    // Alarm settings
    doc["alarmThreshold"] = config.alarmThreshold;
    doc["maxRuntime"] = config.maxRuntime;

    // Telegram
    doc["telegramToken"] = config.telegramToken;
    doc["telegramChatID"] = config.telegramChatID;
    doc["telegramEnabled"] = config.telegramEnabled;

    // System
    doc["autoFeedEnabled"] = config.autoFeedEnabled;
    doc["timezone"] = config.timezone;

    json = "";
    serializeJson(doc, json);
    return true;
}

bool Storage::jsonToConfig(const String& json, Config& config) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);

    if (error) {
        Serial.printf("JSON parse error: %s\n", error.c_str());
        return false;
    }

    // Network
    strlcpy(config.bintracIP, doc["bintracIP"] | "192.168.1.100", sizeof(config.bintracIP));
    config.bintracDeviceID = doc["bintracDeviceID"] | 0;

    // Schedule
    if (doc.containsKey("feedTimes")) {
        for (int i = 0; i < 4; i++) {
            config.feedTimes[i] = doc["feedTimes"][i] | config.feedTimes[i];
        }
    }

    // Feeding parameters
    config.targetWeight = doc["targetWeight"] | 50.0;
    config.weightUnit = (WeightUnit)(doc["weightUnit"] | 0);
    config.chainPreRunTime = doc["chainPreRunTime"] | 10;

    // Alarm settings
    config.alarmThreshold = doc["alarmThreshold"] | 10.0;
    config.maxRuntime = doc["maxRuntime"] | 600;

    // Telegram
    strlcpy(config.telegramToken, doc["telegramToken"] | "", sizeof(config.telegramToken));
    strlcpy(config.telegramChatID, doc["telegramChatID"] | "", sizeof(config.telegramChatID));
    config.telegramEnabled = doc["telegramEnabled"] | false;

    // System
    config.autoFeedEnabled = doc["autoFeedEnabled"] | true;
    config.timezone = doc["timezone"] | 0;

    return true;
}

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
