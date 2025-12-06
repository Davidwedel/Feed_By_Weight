#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#ifdef USE_WIFI
#include <WiFi.h>
#endif
#ifdef USE_ETHERNET
#include <Ethernet.h>
#endif
#include <ESPAsyncWebServer.h>
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
    void handleClient();  // No-op for async server, kept for compatibility

private:
    AsyncWebServer* _server;
    Storage& _storage;
    AugerControl& _augerControl;
    BinTrac& _bintrac;
    Config& _config;
    SystemStatus& _status;

    // HTTP handlers
    void handleRoot(AsyncWebServerRequest *request);
    void handleGetStatus(AsyncWebServerRequest *request);
    void handleGetConfig(AsyncWebServerRequest *request);
    void handleSetConfig(AsyncWebServerRequest *request, uint8_t *data, size_t len);
    void handleGetHistory(AsyncWebServerRequest *request);
    void handleClearHistory(AsyncWebServerRequest *request);
    void handleManualControl(AsyncWebServerRequest *request, uint8_t *data, size_t len);
    void handleStartFeed(AsyncWebServerRequest *request);
    void handleStopFeed(AsyncWebServerRequest *request);
    void handleNotFound(AsyncWebServerRequest *request);

    // Utility functions
    String configToJson();
    String statusToJson();
    String historyToJson();
};

#endif // WEB_SERVER_H
