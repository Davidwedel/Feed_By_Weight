#ifndef CONFIG_H
#define CONFIG_H

// Version
#define FIRMWARE_VERSION "1.0.0"

// Network Configuration
// Comment out one of these to select network type
#define USE_ETHERNET
// #define USE_WIFI

#ifdef USE_WIFI
// WiFi credentials
#define WIFI_SSID "CW Wifi"
#define WIFI_PASSWORD "Thewedels"
#endif

// Relay pins (LilyGo 8-channel board)
#define RELAY_1_PIN 33  // Auger (swapped - physical wiring was backwards)
#define RELAY_2_PIN 32  // Chain
#define RELAY_3_PIN 25
#define RELAY_4_PIN 26
#define RELAY_5_PIN 27
#define RELAY_6_PIN 14
#define RELAY_7_PIN 12
#define RELAY_8_PIN 13

// W5500 Ethernet SPI pins
// Default SPI pins for ESP32:
// MISO: GPIO19
// MOSI: GPIO23
// SCK:  GPIO18
// CS:   GPIO5

#define W5500_CS_PIN 5
#define W5500_RESET_PIN -1  // Not connected

// Status LED (built-in)
#define STATUS_LED_PIN 2

// Network settings
#define WEB_SERVER_PORT 80
#define MODBUS_PORT 502
#define BINTRAC_TIMEOUT 5000    // milliseconds
#define BINTRAC_RETRY_DELAY 2000

// BinTrac Modbus addresses
// NOTE: This HouseLink firmware differs from manual!
// - Only supports reading 6 registers max (bins A, B, C)
// - Bin D not accessible via single read
#define MODBUS_BIN_A_ADDR 1000
#define MODBUS_BIN_B_ADDR 1002
#define MODBUS_BIN_C_ADDR 1004
#define MODBUS_BIN_D_ADDR 1006
#define MODBUS_ALL_BINS_ADDR 1000
#define MODBUS_ALL_BINS_LEN 6  // Changed from 8 - this HouseLink only supports 6!
#define MODBUS_FUNCTION_CODE 4  // Input register

// Feeding control constants
#define WEIGHT_CHECK_INTERVAL 1000  // Check weight every second
#define MIN_WEIGHT_CHANGE 0.1       // Minimum detectable weight change
#define ALARM_CHECK_WINDOW 60000    // Check alarm condition over 1 minute
#define EMERGENCY_STOP_WEIGHT -50.0 // Stop if weight increases (bin filling error)

// Storage
#define CONFIG_FILE "/config.json"
#define HISTORY_FILE "/history.csv"
#define MAX_HISTORY_ENTRIES 1000

// Time settings
#define NTP_SERVER "pool.ntp.org"
#define NTP_UPDATE_INTERVAL 3600000  // Update time every hour

// Watchdog
#define WATCHDOG_TIMEOUT 30  // seconds

// Status update intervals
#define STATUS_UPDATE_INTERVAL 5000    // 5 seconds
#define TELEGRAM_UPDATE_INTERVAL 1000  // 1 second (for responsive bot commands)

#endif // CONFIG_H
