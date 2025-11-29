#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include <WebServer.h>
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
    WebServer* _server;
    Storage& _storage;
    AugerControl& _augerControl;
    BinTrac& _bintrac;
    Config& _config;
    SystemStatus& _status;

    // HTTP handlers
    void handleRoot();
    void handleGetStatus();
    void handleGetConfig();
    void handleSetConfig();
    void handleGetHistory();
    void handleClearHistory();
    void handleManualControl();
    void handleStartFeed();
    void handleStopFeed();
    void handleNotFound();

    // Utility functions
    void sendJsonResponse(int code, const char* json);
    void sendErrorResponse(int code, const char* message);
    String configToJson();
    String statusToJson();
    String historyToJson();
};

#endif // WEB_SERVER_H
