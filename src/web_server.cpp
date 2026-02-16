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
                             Config& config, SystemStatus& status, WeightLog& weightLog)
    : _storage(storage), _augerControl(augerControl), _bintrac(bintrac),
      _config(config), _status(status), _weightLog(weightLog), _port(WEB_SERVER_PORT) {
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
        } else if (path == "/weightlog") {
            handleWeightLogPage(client);
        } else if (path == "/api/weightlog") {
            handleGetWeightLog(client);
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

    // Open file from LittleFS
    File file = LittleFS.open("/index.html", "r");
    if (!file) {
        sendNotFound(client);
        return;
    }

    // Get file size
    size_t fileSize = file.size();

    // Send HTTP headers
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.print("Content-Length: ");
    client.println(fileSize);
    client.println("Cache-Control: public, max-age=3600");
    client.println("Access-Control-Allow-Origin: *");
    client.println();

    // Send file in chunks
    const size_t chunkSize = 1460;  // One TCP segment
    uint8_t buffer[chunkSize];
    while (file.available()) {
        size_t bytesRead = file.read(buffer, chunkSize);
        client.write(buffer, bytesRead);
    }

    file.close();
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
    if (doc["dailyTotal"].is<float>()) {
        _config.dailyTotal = doc["dailyTotal"];
    }
    if (doc["numFeedings"].is<int>()) {
        uint8_t n = doc["numFeedings"];
        if (n >= 1 && n <= 4) _config.numFeedings = n;
    }
    if (doc["feedAmounts"].is<JsonArray>()) {
        JsonArray amounts = doc["feedAmounts"];
        for (int i = 0; i < 4 && i < (int)amounts.size(); i++) {
            _config.feedAmounts[i] = amounts[i];
        }
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
    if (doc["fillDetectionRate"].is<float>()) {
        _config.fillDetectionRate = doc["fillDetectionRate"];
    }
    if (doc["fillSettlingTime"].is<int>()) {
        _config.fillSettlingTime = doc["fillSettlingTime"];
    }
    if (doc["weightFluctuationThreshold"].is<float>()) {
        _config.weightFluctuationThreshold = doc["weightFluctuationThreshold"];
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
    if (doc["wifiSSID"].is<const char*>()) {
        strlcpy(_config.wifiSSID, doc["wifiSSID"], sizeof(_config.wifiSSID));
    }
    if (doc["wifiPassword"].is<const char*>()) {
        strlcpy(_config.wifiPassword, doc["wifiPassword"], sizeof(_config.wifiPassword));
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

    _augerControl.startFeeding(_config.feedAmounts[0], _config.chainPreRunTime, _config.maxRuntime, _config.fillDetectionRate, _config.fillSettlingTime, _config.weightFluctuationThreshold);
    _status.state = SystemState::FEEDING;
    _status.feedStartTime = millis();

    sendJsonResponse(client, "{\"success\":true}");
}

void FeedWebServer::handleStopFeed(EthernetClient& client) {
    // Only record if actually feeding
    if (_augerControl.isFeeding()) {
        // Create feed event record for manual stop
        FeedEvent event;
        event.timestamp = time(NULL);  // Get current Unix timestamp
        event.feedCycle = 0;  // Manual feed has no cycle
        event.targetWeight = _config.feedAmounts[0];
        event.actualWeight = _augerControl.getWeightDispensed();
        event.duration = _augerControl.getDuration();
        event.alarmTriggered = true;
        strcpy(event.alarmReason, "Manually stopped");

        // Save to history
        _storage.addFeedEvent(event);
        Serial.println("Manual stop recorded to history");
    }

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

    doc["dailyTotal"] = _config.dailyTotal;
    doc["numFeedings"] = _config.numFeedings;
    JsonArray amounts = doc["feedAmounts"].to<JsonArray>();
    for (int i = 0; i < 4; i++) {
        amounts.add(_config.feedAmounts[i]);
    }
    doc["weightUnit"] = (int)_config.weightUnit;
    doc["chainPreRunTime"] = _config.chainPreRunTime;
    doc["alarmThreshold"] = _config.alarmThreshold;
    doc["maxRuntime"] = _config.maxRuntime;
    doc["fillDetectionRate"] = _config.fillDetectionRate;
    doc["fillSettlingTime"] = _config.fillSettlingTime;
    doc["weightFluctuationThreshold"] = _config.weightFluctuationThreshold;
    doc["telegramToken"] = _config.telegramToken;
    doc["telegramChatID"] = _config.telegramChatID;
    doc["telegramAllowedUsers"] = _config.telegramAllowedUsers;
    doc["telegramEnabled"] = _config.telegramEnabled;
    doc["wifiSSID"] = _config.wifiSSID;
    doc["wifiPassword"] = _config.wifiPassword;
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
    doc["currentTime"] = _status.currentTime;
    doc["lastError"] = _status.lastError;
    doc["lastBintracUpdate"] = _status.lastBintracUpdate;

    String json;
    serializeJson(doc, json);
    return json;
}

void FeedWebServer::handleWeightLogPage(EthernetClient& client) {
    String html = R"rawhtml(<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Weight Log</title>
<style>
body { background: #1a1a2e; color: #e0e0e0; font-family: monospace; margin: 0; padding: 20px; }
h1 { color: #ccc; font-size: 1.2em; margin-bottom: 10px; }
pre { font-size: 13px; line-height: 1.4; white-space: pre; overflow-x: auto; }
a { color: #4da6ff; text-decoration: none; }
a:hover { text-decoration: underline; }
.controls { margin-bottom: 15px; }
</style>
</head><body>
<div class="controls">
  <a href="./">&larr; Back to Dashboard</a>
  <span style="margin-left: 20px; color: #666;" id="updateInfo">Updating every 3s...</span>
</div>
<h1>Weight Log (EMA smoothed, alpha=0.3)</h1>
<pre id="log">Loading...</pre>
<script>
function update() {
  fetch('api/weightlog')
    .then(r => r.text())
    .then(t => {
      const el = document.getElementById('log');
      el.textContent = t;
      el.scrollTop = el.scrollHeight;
    })
    .catch(e => console.error(e));
}
update();
setInterval(update, 3000);
</script>
</body></html>)rawhtml";

    sendResponse(client, 200, "text/html", html);
}

void FeedWebServer::handleGetWeightLog(EthernetClient& client) {
    // Stream response line-by-line to avoid large String allocation
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println("Access-Control-Allow-Origin: *");
    client.println();

    // Header
    client.println("    Time |    A:    +/-   lb/min |    B:    +/-   lb/min |    C:    +/-   lb/min |    D:    +/-   lb/min | Total:   +/-   lb/min");
    client.println("---------+----------------------+----------------------+----------------------+----------------------+----------------------");

    if (_weightLog.count == 0) {
        client.println("  No data yet");
        return;
    }

    // Snapshot time references once
    unsigned long nowMillis = millis();
    time_t nowEpoch = time(NULL);

    // Iterate entries in chronological order
    int start = (_weightLog.count < WEIGHT_LOG_SIZE) ? 0 : _weightLog.head;

    for (int n = 0; n < _weightLog.count; n++) {
        int idx = (start + n) % WEIGHT_LOG_SIZE;
        const WeightLogEntry& entry = _weightLog.entries[idx];

        // Convert millis timestamp to HH:MM:SS
        long millisAgo = (long)(nowMillis - entry.timestamp);
        time_t entryEpoch = nowEpoch - (millisAgo / 1000);
        entryEpoch += _config.timezone * 3600L;
        struct tm ti;
        gmtime_r(&entryEpoch, &ti);

        char line[256];
        int pos = 0;
        pos += snprintf(line + pos, sizeof(line) - pos, "%02d:%02d:%02d |",
                        ti.tm_hour, ti.tm_min, ti.tm_sec);

        // For each bin + total (5 columns)
        for (int col = 0; col < 5; col++) {
            float val = (col < 4) ? entry.weights[col] : entry.total;

            if (n == 0) {
                pos += snprintf(line + pos, sizeof(line) - pos, " %7.0f    --      -- |", val);
            } else {
                int prevIdx = (start + n - 1) % WEIGHT_LOG_SIZE;
                const WeightLogEntry& prev = _weightLog.entries[prevIdx];
                float prevVal = (col < 4) ? prev.weights[col] : prev.total;
                float change = val - prevVal;

                float elapsedSec = (float)(entry.timestamp - prev.timestamp) / 1000.0f;
                float lbMin = (elapsedSec > 0) ? (change / elapsedSec * 60.0f) : 0;

                pos += snprintf(line + pos, sizeof(line) - pos, " %7.0f %+5.0f %+7.1f |", val, change, lbMin);
            }
        }

        client.println(line);
        client.flush();
    }
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
