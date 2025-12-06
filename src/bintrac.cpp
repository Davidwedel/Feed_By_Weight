#include "bintrac.h"
#include "config.h"

#ifdef USE_WIFI
#include <WiFi.h>
#include <WiFiClient.h>
#endif

#ifdef USE_ETHERNET
#include <Ethernet.h>
#include <EthernetClient.h>
#endif

BinTrac::BinTrac() {
    _connected = false;
    _port = 502;
    _deviceID = 1;
    _lastReadTime = 0;
    _lastConnectAttempt = 0;
    strcpy(_ipAddress, "");
    strcpy(_lastError, "Not initialized");
}

bool BinTrac::begin(const char* ipAddress, uint16_t port, uint8_t deviceID) {
    setConnection(ipAddress, port, deviceID);
    return reconnect();
}

void BinTrac::setConnection(const char* ipAddress, uint16_t port, uint8_t deviceID) {
    strncpy(_ipAddress, ipAddress, sizeof(_ipAddress) - 1);
    _ipAddress[sizeof(_ipAddress) - 1] = '\0';
    _port = port;
    _deviceID = deviceID;
}

bool BinTrac::reconnect() {
    // Prevent connection spam
    if (millis() - _lastConnectAttempt < BINTRAC_RETRY_DELAY) {
        return _connected;
    }
    _lastConnectAttempt = millis();

    if (strlen(_ipAddress) == 0) {
        snprintf(_lastError, sizeof(_lastError), "No IP address configured");
        _connected = false;
        return false;
    }

    // Test connection by reading first bin (2 registers = 1 weight value)
    uint16_t testBuffer[2];
    bool success = modbusRead(MODBUS_BIN_A_ADDR, 2, testBuffer);

    if (success) {
        // Verify we got valid data (not just zeros or timeout)
        // Valid data should have at least some non-zero values or be -32767 (disabled bin marker)
        int32_t testValue = parseWeight(testBuffer);
        if (testValue != 0 || testBuffer[0] == 0xFFFF) {
            _connected = true;
            snprintf(_lastError, sizeof(_lastError), "Connected");
            Serial.printf("BinTrac connected to %s:%d (ID: %d)\n", _ipAddress, _port, _deviceID);
        } else {
            _connected = false;
            snprintf(_lastError, sizeof(_lastError), "Connected but no valid data from %s:%d", _ipAddress, _port);
        }
    } else {
        _connected = false;
        // Error message already set by modbusRead
    }

    return _connected;
}

bool BinTrac::readAllBins(float weights[4]) {
    // NOTE: This HouseLink only allows reading 6 registers (3 bins)
    // Bins A, B, C work. Bin D must be read separately or returns error.
    uint16_t buffer[6];

    if (!modbusRead(MODBUS_ALL_BINS_ADDR, MODBUS_ALL_BINS_LEN, buffer)) {
        _connected = false;
        return false;
    }

    // Parse bins A, B, C (format: each is 2 registers, but only first register is the value)
    // This HouseLink doesn't match the manual - it's not 32-bit big-endian!
    for (int i = 0; i < 3; i++) {
        int32_t rawWeight = (int16_t)buffer[i * 2];  // Cast to signed 16-bit

        // Check for disabled bin (-32767 indicates bin not enabled)
        if (rawWeight == -32767) {
            weights[i] = 0.0;
        } else {
            weights[i] = (float)rawWeight;
        }
    }

    // Try to read bin D separately
    uint16_t binDBuffer[2];
    if (modbusRead(MODBUS_BIN_D_ADDR, 2, binDBuffer)) {
        int32_t rawWeight = (int16_t)binDBuffer[0];
        weights[3] = (rawWeight == -32767) ? 0.0 : (float)rawWeight;
    } else {
        // Bin D not available
        weights[3] = 0.0;
    }

    _connected = true;
    _lastReadTime = millis();
    return true;
}

bool BinTrac::readBin(uint8_t binIndex, float& weight) {
    if (binIndex > 3) {
        snprintf(_lastError, sizeof(_lastError), "Invalid bin index: %d", binIndex);
        return false;
    }

    uint16_t address = MODBUS_BIN_A_ADDR + (binIndex * 2);
    uint16_t buffer[2];

    if (!modbusRead(address, 2, buffer)) {
        _connected = false;
        return false;
    }

    int32_t rawWeight = parseWeight(buffer);

    // Check for disabled bin
    if (rawWeight == -32767 || rawWeight == 0xFFFF8001) {
        weight = 0.0;
    } else {
        weight = (float)rawWeight;
    }

    _connected = true;
    _lastReadTime = millis();
    return true;
}

bool BinTrac::isConnected() {
    // Consider disconnected if no successful read in last 30 seconds
    if (_connected && (millis() - _lastReadTime > 30000)) {
        _connected = false;
        snprintf(_lastError, sizeof(_lastError), "Connection timeout");
    }
    return _connected;
}

const char* BinTrac::getLastError() {
    return _lastError;
}

int32_t BinTrac::parseWeight(uint16_t* data) {
    // Combine two 16-bit registers into 32-bit signed integer
    // Big-endian format (high word first)
    int32_t value = ((int32_t)data[0] << 16) | data[1];
    return value;
}

bool BinTrac::modbusRead(uint16_t address, uint16_t length, uint16_t* buffer) {
    // Clear buffer before reading
    memset(buffer, 0, length * sizeof(uint16_t));

    // Create TCP client
#ifdef USE_WIFI
    WiFiClient client;
#else
    EthernetClient client;
#endif

    // Parse IP address
    IPAddress ip;
    if (!ip.fromString(_ipAddress)) {
        snprintf(_lastError, sizeof(_lastError), "Invalid IP address: %s", _ipAddress);
        return false;
    }

    // Connect to Modbus server
    Serial.printf("Attempting TCP connection to %s:%d...\n", _ipAddress, _port);
    if (!client.connect(ip, _port)) {
        snprintf(_lastError, sizeof(_lastError), "TCP connection failed to %s:%d", _ipAddress, _port);
        Serial.printf("Connection failed. Client status: %d\n", client.connected());
        return false;
    }
    Serial.printf("TCP connected successfully!\n");

    // Build Modbus TCP request
    static uint16_t transactionID = 1;
    uint8_t request[12];

    // Transaction ID (2 bytes)
    request[0] = (transactionID >> 8) & 0xFF;
    request[1] = transactionID & 0xFF;
    transactionID++;

    // Protocol ID (2 bytes, always 0 for Modbus TCP)
    request[2] = 0;
    request[3] = 0;

    // Length (2 bytes) - remaining bytes after this field
    request[4] = 0;
    request[5] = 6;  // Unit ID (1) + Function Code (1) + Address (2) + Count (2)

    // Unit ID (1 byte)
    request[6] = _deviceID;

    // Function Code (1 byte) - 4 = Read Input Registers
    request[7] = 4;

    // Starting Address (2 bytes)
    request[8] = (address >> 8) & 0xFF;
    request[9] = address & 0xFF;

    // Quantity of Registers (2 bytes)
    request[10] = (length >> 8) & 0xFF;
    request[11] = length & 0xFF;

    // Send request
    client.write(request, 12);
    client.flush();

    // Wait for response with timeout
    unsigned long startTime = millis();
    while (client.available() < 9 && (millis() - startTime < BINTRAC_TIMEOUT)) {
        delay(10);
    }

    if (client.available() < 9) {
        client.stop();
        snprintf(_lastError, sizeof(_lastError), "Timeout waiting for response from %s:%d", _ipAddress, _port);
        return false;
    }

    // Read response header (9 bytes)
    uint8_t response[9];
    client.readBytes(response, 9);

    // Check function code for errors
    if (response[7] & 0x80) {
        uint8_t exceptionCode = response[8];
        client.stop();
        snprintf(_lastError, sizeof(_lastError), "Modbus exception code %d from %s:%d",
                 exceptionCode, _ipAddress, _port);
        return false;
    }

    // Byte count
    uint8_t byteCount = response[8];

    if (byteCount != length * 2) {
        client.stop();
        snprintf(_lastError, sizeof(_lastError), "Unexpected byte count: expected %d, got %d",
                 length * 2, byteCount);
        return false;
    }

    // Wait for data bytes
    startTime = millis();
    while (client.available() < byteCount && (millis() - startTime < BINTRAC_TIMEOUT)) {
        delay(10);
    }

    if (client.available() < byteCount) {
        client.stop();
        snprintf(_lastError, sizeof(_lastError), "Timeout waiting for data bytes");
        return false;
    }

    // Read register values (big-endian)
    for (uint16_t i = 0; i < length; i++) {
        uint8_t high = client.read();
        uint8_t low = client.read();
        buffer[i] = (high << 8) | low;
    }

    client.stop();
    return true;
}
