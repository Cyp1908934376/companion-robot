/// Charging station hardware config.
/// GPIO assignments for the ESP32-C3 board.

#pragma once

// IR beacon LEDs (3 channels, GPIOs 0-2)
#define HW_IR_A         0
#define HW_IR_B         1
#define HW_IR_C         2

// Charge controller
#define HW_PWR_EN       4    // MOSFET gate: high=charge enabled
#define HW_VSENSE_ADC   3    // ADC1_CH0: voltage divider 2:1
#define HW_ISENSE_ADC   3    // ADC1_CH0: shared (alternate read)

// Status RGB LED
#define HW_RGB_R        5
#define HW_RGB_G        6
#define HW_RGB_B        7

// I2C (for future INA226 power monitor)
#define HW_I2C_SDA      8
#define HW_I2C_SCL      9

// Default station identity
#define CFG_STATION_ID  1

// WiFi
#define CFG_WIFI_SSID       "CompanionNet"
#define CFG_WIFI_PASSWORD   ""

// Gateway connection
#define CFG_GATEWAY_HOST    "192.168.1.100"
#define CFG_GATEWAY_PORT    8080
