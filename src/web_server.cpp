#include "web_server.h"
#include "config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

FeedWebServer::FeedWebServer(Storage& storage, AugerControl& augerControl, BinTrac& bintrac,
                             Config& config, SystemStatus& status)
    : _storage(storage), _augerControl(augerControl), _bintrac(bintrac),
      _config(config), _status(status) {
    _server = new AsyncWebServer(WEB_SERVER_PORT);
}

void FeedWebServer::begin() {
    // API endpoints
    _server->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request) { handleGetStatus(request); });
    _server->on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request) { handleGetConfig(request); });
    _server->on("/api/config", HTTP_POST, [this](AsyncWebServerRequest *request) {}, NULL,
        [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            handleSetConfig(request, data, len);
        });
    _server->on("/api/history", HTTP_GET, [this](AsyncWebServerRequest *request) { handleGetHistory(request); });
    _server->on("/api/history", HTTP_DELETE, [this](AsyncWebServerRequest *request) { handleClearHistory(request); });
    _server->on("/api/manual", HTTP_POST, [this](AsyncWebServerRequest *request) {}, NULL,
        [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            handleManualControl(request, data, len);
        });
    _server->on("/api/feed/start", HTTP_POST, [this](AsyncWebServerRequest *request) { handleStartFeed(request); });
    _server->on("/api/feed/stop", HTTP_POST, [this](AsyncWebServerRequest *request) { handleStopFeed(request); });

    // Serve main page
    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest *request) { handleRoot(request); });

    // 404 handler
    _server->onNotFound([this](AsyncWebServerRequest *request) { handleNotFound(request); });

    _server->begin();
    Serial.printf("Web server started on port %d\n", WEB_SERVER_PORT);
}

void FeedWebServer::handleClient() {
    // No-op for async server
}

void FeedWebServer::handleRoot(AsyncWebServerRequest *request) {
    // Serve index.html from LittleFS
    if (!LittleFS.exists("/index.html")) {
        request->send(200, "text/html",
            "<html><body><h1>Weight Feeder Control</h1>"
            "<p>Web interface not installed. Use API endpoints:</p>"
            "<ul><li>/api/status</li><li>/api/config</li><li>/api/history</li></ul>"
            "</body></html>");
        return;
    }

    request->send(LittleFS, "/index.html", "text/html");
}

void FeedWebServer::handleGetStatus(AsyncWebServerRequest *request) {
    String json = statusToJson();
    request->send(200, "application/json", json);
}

void FeedWebServer::handleGetConfig(AsyncWebServerRequest *request) {
    String json = configToJson();
    request->send(200, "application/json", json);
}

void FeedWebServer::handleSetConfig(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, data, len);

    if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    // Update configuration
    if (doc.containsKey("bintracIP")) {
        strlcpy(_config.bintracIP, doc["bintracIP"], sizeof(_config.bintracIP));
    }
    if (doc.containsKey("bintracDeviceID")) {
        _config.bintracDeviceID = doc["bintracDeviceID"];
    }
    if (doc.containsKey("feedTimes")) {
        for (int i = 0; i < 4; i++) {
            _config.feedTimes[i] = doc["feedTimes"][i];
        }
    }
    if (doc.containsKey("targetWeight")) {
        _config.targetWeight = doc["targetWeight"];
    }
    if (doc.containsKey("weightUnit")) {
        _config.weightUnit = (WeightUnit)(int)doc["weightUnit"];
    }
    if (doc.containsKey("chainPreRunTime")) {
        _config.chainPreRunTime = doc["chainPreRunTime"];
    }
    if (doc.containsKey("alarmThreshold")) {
        _config.alarmThreshold = doc["alarmThreshold"];
    }
    if (doc.containsKey("maxRuntime")) {
        _config.maxRuntime = doc["maxRuntime"];
    }
    if (doc.containsKey("telegramToken")) {
        strlcpy(_config.telegramToken, doc["telegramToken"], sizeof(_config.telegramToken));
    }
    if (doc.containsKey("telegramChatID")) {
        strlcpy(_config.telegramChatID, doc["telegramChatID"], sizeof(_config.telegramChatID));
    }
    if (doc.containsKey("telegramAllowedUsers")) {
        strlcpy(_config.telegramAllowedUsers, doc["telegramAllowedUsers"], sizeof(_config.telegramAllowedUsers));
    }
    if (doc.containsKey("telegramEnabled")) {
        _config.telegramEnabled = doc["telegramEnabled"];
        Serial.printf("Set telegramEnabled = %d\n", _config.telegramEnabled);
    }
    if (doc.containsKey("autoFeedEnabled")) {
        _config.autoFeedEnabled = doc["autoFeedEnabled"];
    }
    if (doc.containsKey("timezone")) {
        _config.timezone = doc["timezone"];
    }

    // Save to filesystem
    Serial.println("Saving configuration to filesystem...");
    if (_storage.saveConfig(_config)) {
        Serial.println("Configuration saved successfully");
        request->send(200, "application/json", "{\"success\":true}");
    } else {
        Serial.println("ERROR: Failed to save configuration");
        request->send(500, "application/json", "{\"error\":\"Failed to save configuration\"}");
    }
}

void FeedWebServer::handleGetHistory(AsyncWebServerRequest *request) {
    String json = historyToJson();
    request->send(200, "application/json", json);
}

void FeedWebServer::handleClearHistory(AsyncWebServerRequest *request) {
    if (_storage.clearHistory()) {
        request->send(200, "application/json", "{\"success\":true}");
    } else {
        request->send(500, "application/json", "{\"error\":\"Failed to clear history\"}");
    }
}

void FeedWebServer::handleManualControl(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, data, len);

    if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    String action = doc["action"];

    if (action == "auger_on") {
        _augerControl.setAuger(true);
    } else if (action == "auger_off") {
        _augerControl.setAuger(false);
    } else if (action == "chain_on") {
        _augerControl.setChain(true);
    } else if (action == "chain_off") {
        _augerControl.setChain(false);
    } else if (action == "stop_all") {
        _augerControl.stopAll();
    } else {
        request->send(400, "application/json", "{\"error\":\"Unknown action\"}");
        return;
    }

    request->send(200, "application/json", "{\"success\":true}");
}

void FeedWebServer::handleStartFeed(AsyncWebServerRequest *request) {
    if (_augerControl.isFeeding()) {
        request->send(400, "application/json", "{\"error\":\"Feeding already in progress\"}");
        return;
    }

    // Read fresh weight data before starting
    if (_bintrac.readAllBins(_status.currentWeight)) {
        _status.bintracConnected = true;
        _status.lastBintracUpdate = millis();
    } else {
        request->send(500, "application/json", "{\"error\":\"Failed to read bin weights\"}");
        return;
    }

    // Calculate total weight from all bins
    float totalWeight = 0;
    for (int i = 0; i < 4; i++) {
        totalWeight += _status.currentWeight[i];
    }
    _status.weightAtStart = totalWeight;

    _augerControl.startFeeding(_config.targetWeight, _config.chainPreRunTime, _config.maxRuntime);
    _status.state = SystemState::FEEDING;
    _status.feedStartTime = millis();

    request->send(200, "application/json", "{\"success\":true}");
}

void FeedWebServer::handleStopFeed(AsyncWebServerRequest *request) {
    _augerControl.stopAll();
    request->send(200, "application/json", "{\"success\":true}");
}

void FeedWebServer::handleNotFound(AsyncWebServerRequest *request) {
    request->send(404, "application/json", "{\"error\":\"Not found\"}");
}

String FeedWebServer::configToJson() {
    JsonDocument doc;

    doc["bintracIP"] = _config.bintracIP;
    doc["bintracDeviceID"] = _config.bintracDeviceID;

    JsonArray times = doc["feedTimes"].to<JsonArray>();
    for (int i = 0; i < 4; i++) {
        times.add(_config.feedTimes[i]);
    }

    doc["targetWeight"] = _config.targetWeight;
    doc["weightUnit"] = (int)_config.weightUnit;
    doc["chainPreRunTime"] = _config.chainPreRunTime;
    doc["alarmThreshold"] = _config.alarmThreshold;
    doc["maxRuntime"] = _config.maxRuntime;
    doc["telegramToken"] = _config.telegramToken;
    doc["telegramChatID"] = _config.telegramChatID;
    doc["telegramAllowedUsers"] = _config.telegramAllowedUsers;
    doc["telegramEnabled"] = _config.telegramEnabled;
    doc["autoFeedEnabled"] = _config.autoFeedEnabled;
    doc["timezone"] = _config.timezone;

    String json;
    serializeJson(doc, json);
    return json;
}

String FeedWebServer::statusToJson() {
    JsonDocument doc;

    doc["state"] = (int)_status.state;
    doc["feedingStage"] = (int)_status.feedingStage;
    doc["feedStartTime"] = _status.feedStartTime;

    JsonArray bins = doc["currentWeight"].to<JsonArray>();
    for (int i = 0; i < 4; i++) {
        bins.add(_status.currentWeight[i]);
    }

    doc["weightAtStart"] = _status.weightAtStart;
    doc["weightDispensed"] = _status.weightDispensed;
    doc["augerRunning"] = _status.augerRunning;
    doc["chainRunning"] = _status.chainRunning;
    doc["bintracConnected"] = _status.bintracConnected;
    doc["networkConnected"] = _status.networkConnected;
    doc["lastError"] = _status.lastError;
    doc["lastBintracUpdate"] = _status.lastBintracUpdate;

    String json;
    serializeJson(doc, json);
    return json;
}

String FeedWebServer::historyToJson() {
    FeedEvent events[50];
    int count = 0;

    _storage.getFeedHistory(events, count, 50);

    JsonDocument doc;
    JsonArray arr = doc["history"].to<JsonArray>();

    for (int i = 0; i < count; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["timestamp"] = events[i].timestamp;
        obj["feedCycle"] = events[i].feedCycle;
        obj["targetWeight"] = events[i].targetWeight;
        obj["actualWeight"] = events[i].actualWeight;
        obj["duration"] = events[i].duration;
        obj["alarmTriggered"] = events[i].alarmTriggered;
        obj["alarmReason"] = events[i].alarmReason;
    }

    String json;
    serializeJson(doc, json);
    return json;
}
