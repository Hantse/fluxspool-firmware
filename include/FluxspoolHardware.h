#pragma once

#include <Arduino.h>

namespace fluxspool
{
namespace hw
{
// Fluxspool V1.0 production PCB, ESP32-S3-WROOM-1-N8.
constexpr uint8_t HX711_DOUT_PIN = 9; // PCB net HX_DT, HX711 pin DOUT.
constexpr uint8_t HX711_SCK_PIN = 10; // PCB net HX_SCK, HX711 pin PD_SCK.

constexpr uint8_t I2C_SDA_PIN = 14; // Shared AS7341 / GY-33 header SDA.
constexpr uint8_t I2C_SCL_PIN = 21; // Shared AS7341 / GY-33 header SCL.

constexpr uint8_t MFRC522_RST_PIN = 38;
constexpr uint8_t MFRC522_MISO_PIN = 39;
constexpr uint8_t MFRC522_MOSI_PIN = 40;
constexpr uint8_t MFRC522_SCK_PIN = 41;
constexpr uint8_t MFRC522_SS_PIN = 42; // PCB label MFRC_SDA, RC522 SPI SDA/SS.

constexpr uint8_t BOOT_PIN = 0;
constexpr uint8_t USB_D_MINUS_PIN = 19;
constexpr uint8_t USB_D_PLUS_PIN = 20;
}
}
