#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include <Ethernet.h>
#include "types.h"
#include "storage.h"
#include "auger_control.h"
#include "bintrac.h"

class FeedWebServer {
public:
    FeedWebServer(Storage& storage, AugerControl& augerControl, BinTrac& bintrac,
                  Config& config, SystemStatus& status);

    // Initialize web server
    void begin();

    // Handle client requests (call in main loop)
    void handleClient();

private:
    uint16_t _port;
    Storage& _storage;
    AugerControl& _augerControl;
    BinTrac& _bintrac;
    Config& _config;
    SystemStatus& _status;

    // HTTP request handling
    void handleRequest(EthernetClient& client);
    void sendResponse(EthernetClient& client, int code, const char* contentType, const String& body);
    void sendJsonResponse(EthernetClient& client, const String& json);
    void sendNotFound(EthernetClient& client);

    // HTTP handlers
    void handleRoot(EthernetClient& client);
    void handleGetStatus(EthernetClient& client);
    void handleGetConfig(EthernetClient& client);
    void handleSetConfig(EthernetClient& client, const String& body);
    void handleGetHistory(EthernetClient& client);
    void handleClearHistory(EthernetClient& client);
    void handleManualControl(EthernetClient& client, const String& body);
    void handleStartFeed(EthernetClient& client);
    void handleStopFeed(EthernetClient& client);

    // Utility functions
    String configToJson();
    String statusToJson();
    String historyToJson();
};

#endif // WEB_SERVER_H
