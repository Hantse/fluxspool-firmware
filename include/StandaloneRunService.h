#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HX711.h>
#include <MFRC522.h>
#include <Adafruit_AS7341.h>
#include <Adafruit_TCS34725.h>
#include <GY33.h>

#include "FluxspoolHardware.h"
#include "PreferenceService.h"
#include "MqttService.h"
#include "OtaService.h"

// StandaloneRunService
// - Same WiFi/MQTT/token lifecycle as gateway
// - No ESP-NOW: sensors (scale, color, RFID) are local to the device
// - Handles MQTT commands: Scan, Weight, Color, WriteRfid, Ota

class StandaloneRunService
{
public:
  struct Config
  {
    const char *apiBase = "https://api.fluxspool.app";
    const char *mqttBase = "mqtt.fluxspool.app";

    uint32_t registerRetryMs = 2000;
    uint32_t statusEveryMs = 60000;
    uint32_t telemetryEveryMs = 60000;
    uint32_t tokenCheckEveryMs = 30000;
    uint32_t tokenSkewSec = 60;

    float hx711Scale = 1.0f;
    long hx711Offset = 0;
    uint8_t hx711Gain = 128;
    uint8_t hx711Samples = 5;
    uint32_t hx711ReadyTimeoutMs = 1000;

    uint8_t sensorReadRetries = 3;
    uint32_t sensorRetryDelayMs = 100;
    uint32_t rfidScanTimeoutMs = 1000;
    uint32_t rfidPassivePollEveryMs = 250;
    uint32_t rfidCacheTtlMs = 30000;
    uint32_t rfidDetectedPublishCooldownMs = 5000;
    uint8_t rfidDataBlock = 4;
  };

  StandaloneRunService(PreferenceService &prefs, MqttService &mqtt, const Config &cfg);

  void begin();
  void loop();

private:
  // Lifecycle
  void ensureWifiAndTime();
  bool wifiConnectSTA(uint32_t timeoutMs = 15000);
  bool ensureTimeSynced(uint32_t timeoutMs = 8000);

  // Auth
  bool tokenValidSoon() const;
  bool ensureValidToken();
  bool authRefresh();
  bool httpPostJson(const String &url, const String &body, String &outResp, int &outCode);

  // MQTT
  void mqttBeginIfNeeded();
  void mqttSubscribeAll();

  void publishRegister();
  void publishStatusIfDue();
  void publishTelemetryIfDue();

  // MQTT handlers (static bridges + instance methods)
  static void onRegisterConfirmStatic(char *topic, byte *payload, unsigned int length);
  static void onCommandStatic(char *topic, byte *payload, unsigned int length);

  void onRegisterConfirm(char *topic, byte *payload, unsigned int length);
  void onCommand(char *topic, byte *payload, unsigned int length);

  // Command handlers (stub implementations — wire up your hardware here)
  struct ColorReading
  {
    const char *sensor = "";
    uint16_t r = 0;
    uint16_t g = 0;
    uint16_t b = 0;
    uint16_t c = 0;
    uint16_t f1 = 0;
    uint16_t f2 = 0;
    uint16_t f3 = 0;
    uint16_t f4 = 0;
    uint16_t f5 = 0;
    uint16_t f6 = 0;
    uint16_t f7 = 0;
    uint16_t f8 = 0;
    uint16_t nir = 0;
    uint16_t lux = 0;
    uint16_t colorTemp = 0;
    uint16_t colorIndex = 0;
    const char *colorName = "";
  };

  // Hardware
  void loadScaleCalibration();
  void applyScaleCalibration();
  void ensureI2cStarted();
  bool ensureScaleStarted();
  bool ensureAs7341Started();
  bool ensureGy33Started();
  bool ensureTcs34725Started();
  bool ensureRfidStarted();

  bool readWeight(float &weightG, long &raw, String &error, uint8_t samples);
  bool readWeightWithRetries(float &weightG, long &raw, String &error, uint8_t &attempts, uint8_t samples);
  bool readScaleRaw(long &raw, String &error, uint8_t samples);
  bool saveScaleCalibration(float scale, long offset, bool calibrated);
  bool readColor(ColorReading &reading, String &error);
  bool readColorWithRetries(ColorReading &reading, String &error, uint8_t &attempts);
  bool readAs7341Color(ColorReading &reading);
  bool readGy33Color(ColorReading &reading);
  bool readTcs34725Color(ColorReading &reading);
  bool readRfidUid(String &uid, String &error, uint32_t timeoutMs);
  bool readRfidUidWithRetries(String &uid, String &error, uint8_t &attempts, bool &fromCache, uint32_t &cacheAgeMs);
  bool readSelectedRfidData(String &data, String &error, uint8_t blockAddr);
  bool selectRfidCard(String &uid, String &error, uint32_t timeoutMs);
  bool writeRfidBlock(const String &expectedUid, const String &data, uint8_t blockAddr, String &actualUid, String &error);
  void pollRfidCache();
  void rememberRfidUid(const String &uid, bool allowTelemetry);
  void rememberRfidData(const String &data);
  void publishRfidDetected(const String &uid, const String &data, const String &dataError);
  bool cachedRfidUid(String &uid, uint32_t &ageMs) const;
  void addColorJson(JsonObject target, const ColorReading &color) const;
  String currentRfidUid() const;
  String normalizeUid(const String &uid) const;
  String rfidStatusName(MFRC522::StatusCode status) const;
  uint8_t rfidTrailerBlock(uint8_t blockAddr) const;

  // Command handlers
  void handleScan(const String &correlationId);
  void handleWeight(const String &correlationId, uint8_t samples);
  void handleScaleTare(const String &correlationId, uint8_t samples);
  void handleScaleCalibrate(const String &correlationId, float referenceWeightG, uint8_t samples);
  void handleScaleStatus(const String &correlationId);
  void handleColor(const String &correlationId);
  void handleWriteRfid(const String &correlationId, const String &uid, const String &data);
  void handleRfidDetectMode(const String &correlationId, bool enabled);
  void handleOta(const String &correlationId, const String &url);

  // Helpers
  String deviceKey() const;
  String topicOf(const char *suffix) const;
  void publishCommandResult(const JsonDocument &doc);

private:
  PreferenceService &_prefs;
  MqttService &_mqtt;
  Config _cfg;
  OtaService _ota;

  bool _running = false;
  bool _mqttStarted = false;
  bool _registerConfirmed = false;

  uint32_t _lastMqttAttemptMs = 0;
  uint32_t _lastRegisterMs = 0;
  uint32_t _lastStatusMs = 0;
  uint32_t _lastTelemetryMs = 0;
  uint32_t _lastTokenCheckMs = 0;
  uint32_t _lastRfidPassivePollMs = 0;
  uint32_t _lastRfidDetectedPublishMs = 0;

  bool _i2cStarted = false;
  bool _scaleStarted = false;
  bool _scaleTared = false;
  bool _scaleCalibrated = false;
  bool _as7341Started = false;
  bool _gy33Started = false;
  bool _tcs34725Started = false;
  bool _rfidStarted = false;

  HX711 _scale;
  Adafruit_AS7341 _as7341;
  GY33_I2C _gy33;
  Adafruit_TCS34725 _tcs34725{TCS34725_INTEGRATIONTIME_50MS, TCS34725_GAIN_4X};
  MFRC522 _rfid{fluxspool::hw::MFRC522_SS_PIN, fluxspool::hw::MFRC522_RST_PIN};

  bool _hasLastWeight = false;
  float _lastWeightG = 0.0f;
  long _lastWeightRaw = 0;
  uint32_t _lastWeightAtMs = 0;

  bool _hasLastColor = false;
  ColorReading _lastColor;
  uint32_t _lastColorAtMs = 0;

  String _lastUid;
  uint32_t _lastUidAtMs = 0;
  bool _lastUidTelemetryAllowed = false;
  String _lastRfidData;
  uint32_t _lastRfidDataAtMs = 0;
  bool _rfidDetectMode = false;
  String _lastRfidDetectedSignature;

  static StandaloneRunService *_self;
};
