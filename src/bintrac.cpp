#include "bintrac.h"
#include "config.h"

#include <Ethernet.h>

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

/**
 * RECONNECT TO BINTRAC
 *
 * Attempts to establish Modbus TCP connection by doing a test read of bin A.
 * Rate-limited to prevent connection spam (min BINTRAC_RETRY_DELAY between attempts).
 *
 * Connection is considered successful if:
 * - TCP connection succeeds
 * - Modbus response is valid
 * - Data is non-zero OR is the special -32767 disabled marker
 */
bool BinTrac::reconnect() {
    // Prevent connection spam (rate limit reconnection attempts)
    if (millis() - _lastConnectAttempt < BINTRAC_RETRY_DELAY) {
        return _connected;
    }
    _lastConnectAttempt = millis();

    if (strlen(_ipAddress) == 0) {
        snprintf(_lastError, sizeof(_lastError), "No IP address configured");
        _connected = false;
        return false;
    }

    // Test connection by reading first bin (2 registers = 1 32-bit weight value)
    uint16_t testBuffer[2];
    bool success = modbusRead(MODBUS_BIN_A_ADDR, 2, testBuffer);

    if (success) {
        // Verify we got valid data (not just zeros which might indicate wrong address)
        // Valid data: non-zero weight OR -32767 (0xFFFF8001) disabled bin marker
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

/**
 * READ ALL 4 BINS IN ONE REQUEST
 *
 * Reads 8 Modbus registers (4 bins × 2 registers each) starting from MODBUS_ALL_BINS_ADDR.
 * Each bin weight is a 32-bit signed integer stored in 2 consecutive registers (big-endian).
 *
 * Special value -32767 (0xFFFF8001) indicates bin is disabled/not connected - returns 0.0 for those.
 *
 * Returns: true if read succeeded, false if Modbus error or network failure
 */
bool BinTrac::readAllBins(float weights[4]) {
    // Read all 4 bins in a single 8-register Modbus TCP transaction
    uint16_t buffer[8];

    if (!modbusRead(MODBUS_ALL_BINS_ADDR, MODBUS_ALL_BINS_LEN, buffer)) {
        _connected = false;
        return false;
    }

    // Parse each bin (2 registers each = 32-bit signed integer, big-endian)
    for (int i = 0; i < 4; i++) {
        int32_t rawWeight = parseWeight(&buffer[i * 2]);

        // Check for disabled bin marker: -32767 (0xFFFF8001)
        if (rawWeight == -32767) {
            weights[i] = 0.0;  // Treat disabled bins as 0 weight
        } else {
            weights[i] = (float)rawWeight;
        }
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

/**
 * PARSE 32-BIT WEIGHT FROM TWO 16-BIT REGISTERS
 *
 * Modbus registers are 16-bit, but BinTrac weights are 32-bit signed integers.
 * Stored in big-endian format: [high word, low word]
 *
 * Example: Weight 12345 = 0x00003039
 *   Register 0: 0x0000 (high word)
 *   Register 1: 0x3039 (low word)
 */
int32_t BinTrac::parseWeight(uint16_t* data) {
    // Combine two 16-bit registers into 32-bit signed integer (big-endian)
    int32_t value = ((int32_t)data[0] << 16) | data[1];
    return value;
}

/**
 * RAW MODBUS TCP READ
 *
 * Builds and sends a Modbus TCP request manually (no library used).
 *
 * Modbus TCP Frame Structure:
 * [MBAP Header: 7 bytes] [PDU: Function Code + Address + Count]
 *
 * MBAP Header:
 *   Transaction ID (2 bytes) - increments with each request
 *   Protocol ID (2 bytes) - always 0 for Modbus
 *   Length (2 bytes) - bytes following this field
 *   Unit ID (1 byte) - device ID (usually 1)
 *
 * PDU (Protocol Data Unit):
 *   Function Code (1 byte) - 4 = Read Input Registers
 *   Starting Address (2 bytes) - 0-based register address
 *   Quantity (2 bytes) - number of registers to read
 *
 * IMPORTANT: Config uses 1-based addresses (address 1000 in config),
 * but Modbus wire protocol uses 0-based (999 on the wire).
 * This function subtracts 1 from the address.
 */
bool BinTrac::modbusRead(uint16_t address, uint16_t length, uint16_t* buffer) {
    // Convert from 1-based config address to 0-based Modbus wire protocol address
    // Example: Config says 1000, we send 999 on the wire
    address = address - 1;

    // Clear buffer before reading
    memset(buffer, 0, length * sizeof(uint16_t));

    // Create TCP client (WiFiClient or EthernetClient depending on build config)
#ifdef USE_WIFI
    WiFiClient client;
#else
    EthernetClient client;
#endif

    // Parse IP address string to IPAddress object
    IPAddress ip;
    if (!ip.fromString(_ipAddress)) {
        snprintf(_lastError, sizeof(_lastError), "Invalid IP address: %s", _ipAddress);
        return false;
    }

    // Connect to Modbus TCP server (BinTrac HL-10E)
    if (!client.connect(ip, _port)) {
        snprintf(_lastError, sizeof(_lastError), "TCP connection failed to %s:%d", _ipAddress, _port);
        return false;
    }

    // ========================================
    // Build Modbus TCP Request Packet (12 bytes total)
    // ========================================
    static uint16_t transactionID = 1;  // Increments with each request
    uint8_t request[12];

    // MBAP Header - Transaction ID (2 bytes)
    // Used to match responses to requests (not critical for single-threaded use)
    request[0] = (transactionID >> 8) & 0xFF;
    request[1] = transactionID & 0xFF;
    transactionID++;

    // MBAP Header - Protocol ID (2 bytes, always 0 for Modbus TCP)
    request[2] = 0;
    request[3] = 0;

    // MBAP Header - Length (2 bytes) - number of bytes following this field
    request[4] = 0;
    request[5] = 6;  // Unit ID (1) + Function Code (1) + Address (2) + Count (2) = 6 bytes

    // MBAP Header - Unit ID (1 byte) - device/slave ID (configurable, usually 1)
    request[6] = _deviceID;

    // PDU - Function Code (1 byte) - 4 = Read Input Registers
    request[7] = 4;

    // PDU - Starting Address (2 bytes, big-endian)
    request[8] = (address >> 8) & 0xFF;
    request[9] = address & 0xFF;

    // PDU - Quantity of Registers (2 bytes, big-endian)
    request[10] = (length >> 8) & 0xFF;
    request[11] = length & 0xFF;

    // Send request over TCP
    client.write(request, 12);
    client.flush();  // Ensure all bytes are sent before waiting for response

    // ========================================
    // Wait for Response Header (9 bytes minimum)
    // ========================================
    unsigned long startTime = millis();
    while (client.available() < 9 && (millis() - startTime < BINTRAC_TIMEOUT)) {
        delay(10);
    }

    if (client.available() < 9) {
        client.stop();
        snprintf(_lastError, sizeof(_lastError), "Timeout waiting for response from %s:%d", _ipAddress, _port);
        return false;
    }

    // ========================================
    // Parse Response Header
    // ========================================
    // Response structure: [MBAP Header: 7 bytes] [Function Code: 1 byte] [Byte Count: 1 byte] [Data...]
    uint8_t response[9];
    client.readBytes(response, 9);

    // Check for Modbus exception (function code high bit set)
    // Normal response: function code = 4 (0x04)
    // Error response: function code = 4 | 0x80 = 0x84, followed by exception code
    if (response[7] & 0x80) {
        uint8_t exceptionCode = response[8];
        client.stop();
        snprintf(_lastError, sizeof(_lastError), "Modbus exception code %d from %s:%d",
                 exceptionCode, _ipAddress, _port);
        return false;
    }

    // Byte count = number of data bytes following (should be length * 2)
    uint8_t byteCount = response[8];

    if (byteCount != length * 2) {
        client.stop();
        snprintf(_lastError, sizeof(_lastError), "Unexpected byte count: expected %d, got %d",
                 length * 2, byteCount);
        return false;
    }

    // ========================================
    // Wait for Data Bytes
    // ========================================
    startTime = millis();
    while (client.available() < byteCount && (millis() - startTime < BINTRAC_TIMEOUT)) {
        delay(10);
    }

    if (client.available() < byteCount) {
        client.stop();
        snprintf(_lastError, sizeof(_lastError), "Timeout waiting for data bytes");
        return false;
    }

    // ========================================
    // Read Register Values (big-endian format)
    // ========================================
    // Each register is 2 bytes: [high byte, low byte]
    for (uint16_t i = 0; i < length; i++) {
        uint8_t high = client.read();
        uint8_t low = client.read();
        buffer[i] = (high << 8) | low;
    }

    client.stop();
    return true;
}
