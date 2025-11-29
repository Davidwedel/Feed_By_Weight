#include "web_server.h"
#include "config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

FeedWebServer::FeedWebServer(Storage& storage, AugerControl& augerControl, BinTrac& bintrac,
                             Config& config, SystemStatus& status)
    : _storage(storage), _augerControl(augerControl), _bintrac(bintrac),
      _config(config), _status(status) {
    _server = new WebServer(WEB_SERVER_PORT);
}

void FeedWebServer::begin() {
    // API endpoints
    _server->on("/api/status", HTTP_GET, [this]() { handleGetStatus(); });
    _server->on("/api/config", HTTP_GET, [this]() { handleGetConfig(); });
    _server->on("/api/config", HTTP_POST, [this]() { handleSetConfig(); });
    _server->on("/api/history", HTTP_GET, [this]() { handleGetHistory(); });
    _server->on("/api/history", HTTP_DELETE, [this]() { handleClearHistory(); });
    _server->on("/api/manual", HTTP_POST, [this]() { handleManualControl(); });
    _server->on("/api/feed/start", HTTP_POST, [this]() { handleStartFeed(); });
    _server->on("/api/feed/stop", HTTP_POST, [this]() { handleStopFeed(); });

    // Serve main page
    _server->on("/", HTTP_GET, [this]() { handleRoot(); });

    // 404 handler
    _server->onNotFound([this]() { handleNotFound(); });

    _server->begin();
    Serial.printf("Web server started on port %d\n", WEB_SERVER_PORT);
}

void FeedWebServer::handleClient() {
    _server->handleClient();
}

void FeedWebServer::handleRoot() {
    // Serve index.html from LittleFS
    if (!LittleFS.exists("/index.html")) {
        _server->send(200, "text/html",
            "<html><body><h1>Weight Feeder Control</h1>"
            "<p>Web interface not installed. Use API endpoints:</p>"
            "<ul><li>/api/status</li><li>/api/config</li><li>/api/history</li></ul>"
            "</body></html>");
        return;
    }

    File file = LittleFS.open("/index.html", "r");
    _server->streamFile(file, "text/html");
    file.close();
}

void FeedWebServer::handleGetStatus() {
    String json = statusToJson();
    sendJsonResponse(200, json.c_str());
}

void FeedWebServer::handleGetConfig() {
    String json = configToJson();
    sendJsonResponse(200, json.c_str());
}

void FeedWebServer::handleSetConfig() {
    if (!_server->hasArg("plain")) {
        sendErrorResponse(400, "Missing JSON body");
        return;
    }

    String body = _server->arg("plain");
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
        sendErrorResponse(400, "Invalid JSON");
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
    if (doc.containsKey("auger2PreRunTime")) {
        _config.auger2PreRunTime = doc["auger2PreRunTime"];
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
    if (doc.containsKey("telegramEnabled")) {
        _config.telegramEnabled = doc["telegramEnabled"];
    }
    if (doc.containsKey("autoFeedEnabled")) {
        _config.autoFeedEnabled = doc["autoFeedEnabled"];
    }
    if (doc.containsKey("timezone")) {
        _config.timezone = doc["timezone"];
    }

    // Save to filesystem
    if (_storage.saveConfig(_config)) {
        sendJsonResponse(200, "{\"success\":true}");
    } else {
        sendErrorResponse(500, "Failed to save configuration");
    }
}

void FeedWebServer::handleGetHistory() {
    String json = historyToJson();
    sendJsonResponse(200, json.c_str());
}

void FeedWebServer::handleClearHistory() {
    if (_storage.clearHistory()) {
        sendJsonResponse(200, "{\"success\":true}");
    } else {
        sendErrorResponse(500, "Failed to clear history");
    }
}

void FeedWebServer::handleManualControl() {
    if (!_server->hasArg("plain")) {
        sendErrorResponse(400, "Missing JSON body");
        return;
    }

    String body = _server->arg("plain");
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
        sendErrorResponse(400, "Invalid JSON");
        return;
    }

    String action = doc["action"];

    if (action == "auger1_on") {
        _augerControl.setAuger1(true);
    } else if (action == "auger1_off") {
        _augerControl.setAuger1(false);
    } else if (action == "auger2_on") {
        _augerControl.setAuger2(true);
    } else if (action == "auger2_off") {
        _augerControl.setAuger2(false);
    } else if (action == "stop_all") {
        _augerControl.stopAll();
    } else {
        sendErrorResponse(400, "Unknown action");
        return;
    }

    sendJsonResponse(200, "{\"success\":true}");
}

void FeedWebServer::handleStartFeed() {
    if (_augerControl.isFeeding()) {
        sendErrorResponse(400, "Feeding already in progress");
        return;
    }

    _augerControl.startFeeding(_config.targetWeight, _config.auger2PreRunTime, _config.maxRuntime);
    sendJsonResponse(200, "{\"success\":true}");
}

void FeedWebServer::handleStopFeed() {
    _augerControl.stopAll();
    sendJsonResponse(200, "{\"success\":true}");
}

void FeedWebServer::handleNotFound() {
    sendErrorResponse(404, "Not found");
}

void FeedWebServer::sendJsonResponse(int code, const char* json) {
    _server->send(code, "application/json", json);
}

void FeedWebServer::sendErrorResponse(int code, const char* message) {
    char json[128];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", message);
    sendJsonResponse(code, json);
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
    doc["auger2PreRunTime"] = _config.auger2PreRunTime;
    doc["alarmThreshold"] = _config.alarmThreshold;
    doc["maxRuntime"] = _config.maxRuntime;
    doc["telegramToken"] = _config.telegramToken;
    doc["telegramChatID"] = _config.telegramChatID;
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
    doc["auger1Running"] = _status.auger1Running;
    doc["auger2Running"] = _status.auger2Running;
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
