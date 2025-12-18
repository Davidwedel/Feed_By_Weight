#include "web_server.h"
#include "config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

// Concrete server class to workaround ESP32 abstract Server issue
class ConcreteEthernetServer : public EthernetServer {
public:
    ConcreteEthernetServer(uint16_t port) : EthernetServer(port) {}
    void begin(uint16_t port = 0) override {
        EthernetServer::begin();
    }
};

// Global server instance
static ConcreteEthernetServer webServer(WEB_SERVER_PORT);

FeedWebServer::FeedWebServer(Storage& storage, AugerControl& augerControl, BinTrac& bintrac,
                             Config& config, SystemStatus& status)
    : _storage(storage), _augerControl(augerControl), _bintrac(bintrac),
      _config(config), _status(status), _port(WEB_SERVER_PORT) {
}

void FeedWebServer::begin() {
    webServer.begin();
    Serial.printf("Web server started on port %d\n", WEB_SERVER_PORT);
}

void FeedWebServer::handleClient() {
    EthernetClient client = webServer.available();
    if (client) {
        if (client.connected()) {
            handleRequest(client);
        }
        client.stop();
    }
}

void FeedWebServer::handleRequest(EthernetClient& client) {
    // Read the HTTP request
    String currentLine = "";
    String requestLine = "";
    String method = "";
    String path = "";
    String body = "";
    int contentLength = 0;
    bool headersDone = false;

    // Read request with timeout
    unsigned long startTime = millis();
    while (client.connected() && (millis() - startTime < 5000)) {
        if (client.available()) {
            char c = client.read();

            if (c == '\n') {
                if (currentLine.length() == 0) {
                    headersDone = true;
                    break;  // End of headers
                } else {
                    // Process header line
                    if (requestLine == "") {
                        requestLine = currentLine;
                        // Parse method and path
                        int firstSpace = requestLine.indexOf(' ');
                        int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
                        if (firstSpace > 0 && secondSpace > 0) {
                            method = requestLine.substring(0, firstSpace);
                            path = requestLine.substring(firstSpace + 1, secondSpace);
                        }
                    } else if (currentLine.startsWith("Content-Length: ")) {
                        contentLength = currentLine.substring(16).toInt();
                    }
                    currentLine = "";
                }
            } else if (c != '\r') {
                currentLine += c;
            }
        }
    }

    // Read body if present
    if (headersDone && contentLength > 0) {
        body.reserve(contentLength);
        startTime = millis();
        while (body.length() < contentLength && (millis() - startTime < 5000)) {
            if (client.available()) {
                body += (char)client.read();
            }
        }
    }

    // Route the request
    if (method == "GET") {
        if (path == "/" || path == "/index.html") {
            handleRoot(client);
        } else if (path == "/api/status") {
            handleGetStatus(client);
        } else if (path == "/api/config") {
            handleGetConfig(client);
        } else if (path == "/api/history") {
            handleGetHistory(client);
        } else {
            sendNotFound(client);
        }
    } else if (method == "POST") {
        if (path == "/api/config") {
            handleSetConfig(client, body);
        } else if (path == "/api/manual") {
            handleManualControl(client, body);
        } else if (path == "/api/feed/start") {
            handleStartFeed(client);
        } else if (path == "/api/feed/stop") {
            handleStopFeed(client);
        } else {
            sendNotFound(client);
        }
    } else if (method == "DELETE") {
        if (path == "/api/history") {
            handleClearHistory(client);
        } else {
            sendNotFound(client);
        }
    } else {
        sendNotFound(client);
    }
}

void FeedWebServer::sendResponse(EthernetClient& client, int code, const char* contentType, const String& body) {
    client.print("HTTP/1.1 ");
    client.print(code);
    client.println(code == 200 ? " OK" : code == 400 ? " Bad Request" : code == 404 ? " Not Found" : " Error");
    client.print("Content-Type: ");
    client.println(contentType);
    client.println("Connection: close");
    client.print("Content-Length: ");
    client.println(body.length());
    client.println("Access-Control-Allow-Origin: *");
    client.println();
    client.print(body);
}

void FeedWebServer::sendJsonResponse(EthernetClient& client, const String& json) {
    sendResponse(client, 200, "application/json", json);
}

void FeedWebServer::sendNotFound(EthernetClient& client) {
    sendResponse(client, 404, "application/json", "{\"error\":\"Not found\"}");
}

void FeedWebServer::handleRoot(EthernetClient& client) {
    // Serve index.html from LittleFS
    if (!LittleFS.exists("/index.html")) {
        String html = "<html><body><h1>Weight Feeder Control</h1>"
                     "<p>Web interface not installed. Use API endpoints:</p>"
                     "<ul><li>/api/status</li><li>/api/config</li><li>/api/history</li></ul>"
                     "</body></html>";
        sendResponse(client, 200, "text/html", html);
        return;
    }

    // Read file from LittleFS
    File file = LittleFS.open("/index.html", "r");
    if (!file) {
        sendNotFound(client);
        return;
    }

    String html = file.readString();
    file.close();
    sendResponse(client, 200, "text/html", html);
}

void FeedWebServer::handleGetStatus(EthernetClient& client) {
    String json = statusToJson();
    sendJsonResponse(client, json);
}

void FeedWebServer::handleGetConfig(EthernetClient& client) {
    String json = configToJson();
    sendJsonResponse(client, json);
}

void FeedWebServer::handleSetConfig(EthernetClient& client, const String& body) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
        sendResponse(client, 400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    // Update configuration
    if (doc["bintracIP"].is<const char*>()) {
        strlcpy(_config.bintracIP, doc["bintracIP"], sizeof(_config.bintracIP));
    }
    if (doc["bintracDeviceID"].is<int>()) {
        _config.bintracDeviceID = doc["bintracDeviceID"];
    }
    if (doc["feedTimes"].is<JsonArray>()) {
        JsonArray times = doc["feedTimes"];
        for (int i = 0; i < 4 && i < times.size(); i++) {
            _config.feedTimes[i] = times[i];
        }
    }
    if (doc["targetWeight"].is<float>()) {
        _config.targetWeight = doc["targetWeight"];
    }
    if (doc["weightUnit"].is<int>()) {
        _config.weightUnit = (WeightUnit)(int)doc["weightUnit"];
    }
    if (doc["chainPreRunTime"].is<int>()) {
        _config.chainPreRunTime = doc["chainPreRunTime"];
    }
    if (doc["alarmThreshold"].is<float>()) {
        _config.alarmThreshold = doc["alarmThreshold"];
    }
    if (doc["maxRuntime"].is<int>()) {
        _config.maxRuntime = doc["maxRuntime"];
    }
    if (doc["telegramToken"].is<const char*>()) {
        strlcpy(_config.telegramToken, doc["telegramToken"], sizeof(_config.telegramToken));
    }
    if (doc["telegramChatID"].is<const char*>()) {
        strlcpy(_config.telegramChatID, doc["telegramChatID"], sizeof(_config.telegramChatID));
    }
    if (doc["telegramAllowedUsers"].is<const char*>()) {
        strlcpy(_config.telegramAllowedUsers, doc["telegramAllowedUsers"], sizeof(_config.telegramAllowedUsers));
    }
    if (doc["telegramEnabled"].is<bool>()) {
        _config.telegramEnabled = doc["telegramEnabled"];
        Serial.printf("Set telegramEnabled = %d\n", _config.telegramEnabled);
    }
    if (doc["autoFeedEnabled"].is<bool>()) {
        _config.autoFeedEnabled = doc["autoFeedEnabled"];
    }
    if (doc["timezone"].is<int>()) {
        _config.timezone = doc["timezone"];
    }

    // Save to filesystem
    Serial.println("Saving configuration to filesystem...");
    if (_storage.saveConfig(_config)) {
        Serial.println("Configuration saved successfully");
        sendJsonResponse(client, "{\"success\":true}");
    } else {
        Serial.println("ERROR: Failed to save configuration");
        sendResponse(client, 500, "application/json", "{\"error\":\"Failed to save configuration\"}");
    }
}

void FeedWebServer::handleGetHistory(EthernetClient& client) {
    String json = historyToJson();
    sendJsonResponse(client, json);
}

void FeedWebServer::handleClearHistory(EthernetClient& client) {
    if (_storage.clearHistory()) {
        sendJsonResponse(client, "{\"success\":true}");
    } else {
        sendResponse(client, 500, "application/json", "{\"error\":\"Failed to clear history\"}");
    }
}

void FeedWebServer::handleManualControl(EthernetClient& client, const String& body) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
        sendResponse(client, 400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    String action = doc["action"].as<String>();

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
        sendResponse(client, 400, "application/json", "{\"error\":\"Unknown action\"}");
        return;
    }

    sendJsonResponse(client, "{\"success\":true}");
}

void FeedWebServer::handleStartFeed(EthernetClient& client) {
    Serial.println("Start feed request received");

    if (_augerControl.isFeeding()) {
        Serial.println("ERROR: Feeding already in progress");
        sendResponse(client, 400, "application/json", "{\"error\":\"Feeding already in progress\"}");
        return;
    }

    // Read fresh weight data before starting
    Serial.println("Reading bin weights...");
    if (_bintrac.readAllBins(_status.currentWeight)) {
        _status.bintracConnected = true;
        _status.lastBintracUpdate = millis();
        Serial.printf("Weights read: A=%.0f B=%.0f C=%.0f D=%.0f\n",
                     _status.currentWeight[0], _status.currentWeight[1],
                     _status.currentWeight[2], _status.currentWeight[3]);
    } else {
        Serial.printf("ERROR: Failed to read bin weights: %s\n", _bintrac.getLastError());
        sendResponse(client, 500, "application/json", "{\"error\":\"Failed to read bin weights\"}");
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

    sendJsonResponse(client, "{\"success\":true}");
}

void FeedWebServer::handleStopFeed(EthernetClient& client) {
    _augerControl.stopAll();
    sendJsonResponse(client, "{\"success\":true}");
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
    doc["flowRate"] = _status.flowRate;
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
