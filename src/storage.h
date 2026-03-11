#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include "types.h"

class Storage {
public:
    Storage();

    // Initialize filesystem
    bool begin();

    // Configuration management
    bool loadConfig(Config& config);
    bool saveConfig(const Config& config);

    // History management
    bool addFeedEvent(const FeedEvent& event);
    bool getFeedHistory(FeedEvent* events, int& count, int maxCount = 50);
    bool clearHistory();
    bool deleteHistoryEntry(time_t timestamp);
    bool writeRawHistory(const String& csvData);

    // Feed progress (NVS) - survives LittleFS wipe
    void saveFeedProgress(float startWeight, float dispensed, float target, uint8_t cycle, unsigned long timestamp);
    bool loadFeedProgress(float& startWeight, float& dispensed, float& target, uint8_t& cycle, unsigned long& timestamp);
    void clearFeedProgress();

    // Utility
    bool formatFilesystem();
    void printFileSystemInfo();

private:
    bool _initialized;
    void trimHistory(int maxEntries);
};

#endif // STORAGE_H
