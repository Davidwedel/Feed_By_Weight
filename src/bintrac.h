#ifndef BINTRAC_H
#define BINTRAC_H

#include <Arduino.h>
#include "types.h"

class BinTrac {
public:
    BinTrac();

    // Initialize Modbus TCP client
    bool begin(const char* ipAddress, uint8_t deviceID = 1);

    // Read all bin weights (returns true if successful)
    bool readAllBins(float weights[4]);

    // Read individual bin weight
    bool readBin(uint8_t binIndex, float& weight);

    // Check connection status
    bool isConnected();

    // Reconnect
    bool reconnect();

    // Get last error message
    const char* getLastError();

    // Update IP address and device ID
    void setConnection(const char* ipAddress, uint8_t deviceID);

private:
    char _ipAddress[16];
    uint8_t _deviceID;
    bool _connected;
    char _lastError[128];
    unsigned long _lastReadTime;
    unsigned long _lastConnectAttempt;

    // Parse 32-bit signed integer from Modbus response
    int32_t parseWeight(uint16_t* data);

    // Low-level Modbus read
    bool modbusRead(uint16_t address, uint16_t length, uint16_t* buffer);
};

#endif // BINTRAC_H
