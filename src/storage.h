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

    // Utility
    bool formatFilesystem();
    void printFileSystemInfo();

private:
    bool _initialized;
};

#endif // STORAGE_H
