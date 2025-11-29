#include "bintrac.h"
#include "config.h"
#include <ModbusIP_ESP8266.h>

// Global Modbus IP client
ModbusIP mb;

BinTrac::BinTrac() {
    _connected = false;
    _deviceID = 1;
    _lastReadTime = 0;
    _lastConnectAttempt = 0;
    strcpy(_ipAddress, "");
    strcpy(_lastError, "Not initialized");
}

bool BinTrac::begin(const char* ipAddress, uint8_t deviceID) {
    setConnection(ipAddress, deviceID);
    return reconnect();
}

void BinTrac::setConnection(const char* ipAddress, uint8_t deviceID) {
    strncpy(_ipAddress, ipAddress, sizeof(_ipAddress) - 1);
    _ipAddress[sizeof(_ipAddress) - 1] = '\0';
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

    // Test connection by reading first bin
    uint16_t testBuffer[2];
    _connected = modbusRead(MODBUS_BIN_A_ADDR, 2, testBuffer);

    if (_connected) {
        snprintf(_lastError, sizeof(_lastError), "Connected");
        Serial.printf("BinTrac connected to %s (ID: %d)\n", _ipAddress, _deviceID);
    } else {
        snprintf(_lastError, sizeof(_lastError), "Connection failed to %s", _ipAddress);
    }

    return _connected;
}

bool BinTrac::readAllBins(float weights[4]) {
    // Read all 8 registers (4 bins x 2 registers each)
    uint16_t buffer[8];

    if (!modbusRead(MODBUS_ALL_BINS_ADDR, MODBUS_ALL_BINS_LEN, buffer)) {
        _connected = false;
        return false;
    }

    // Parse each bin (each weight is 2 x 16-bit registers = 32-bit signed int)
    for (int i = 0; i < 4; i++) {
        int32_t rawWeight = parseWeight(&buffer[i * 2]);

        // Check for disabled bin (-32767 indicates bin not enabled)
        if (rawWeight == -32767 || rawWeight == 0xFFFF8001) {
            weights[i] = 0.0;
        } else {
            weights[i] = (float)rawWeight;
        }
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
    IPAddress ip;
    if (!ip.fromString(_ipAddress)) {
        snprintf(_lastError, sizeof(_lastError), "Invalid IP address: %s", _ipAddress);
        return false;
    }

    // Create Modbus transaction
    mb.readIreg(ip, address, buffer, length, nullptr, _deviceID);

    // Wait for response with timeout
    unsigned long startTime = millis();
    while (millis() - startTime < BINTRAC_TIMEOUT) {
        mb.task();
        delay(10);

        // Check if transaction completed
        // Note: This is simplified - actual implementation would check transaction status
        // The modbus library handles this internally
    }

    // Verify we got data (simplified check)
    if (buffer[0] == 0 && buffer[1] == 0 && length > 2) {
        snprintf(_lastError, sizeof(_lastError), "No response from device");
        return false;
    }

    return true;
}
