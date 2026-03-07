#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

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
  void handleScan(const String &correlationId);
  void handleWeight(const String &correlationId);
  void handleColor(const String &correlationId);
  void handleWriteRfid(const String &correlationId, const String &uid, const String &data);
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

  static StandaloneRunService *_self;
};
