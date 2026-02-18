#pragma once

#include <Arduino.h>

#include "PreferenceService.h"
#include "MqttService.h"
#include "EspNowService.h"

// RunService (V14)
// - Keeps the V13 behavior intact (WiFi+NTP, token refresh, MQTT register/confirm, status/telemetry cadence)
// - Adds ESPNOW polling for probes on command: TelemetryDevice
// - Does NOT change topic names; topics remain exactly as in V13.

class RunService {
public:
  struct Config {
    const char* apiBase = "https://api.fluxspool.app";

    // register retry while waiting confirm
    uint32_t registerRetryMs = 2000;

    // periodic status/telemetry when registered
    uint32_t statusEveryMs = 60000;
    uint32_t telemetryEveryMs = 60000;

    // token refresh
    uint32_t tokenCheckEveryMs = 30000;
    uint32_t tokenSkewSec = 60;

    // espnow
    uint32_t espnowTimeoutMs = 1200;
    uint8_t  espnowRetries = 1;
  };

  RunService(PreferenceService& prefs, MqttService& mqtt, const Config& cfg);

  void begin();
  void loop();

private:
  // core lifecycle
  void ensureWifiAndTime();
  bool wifiConnectSTA(uint32_t timeoutMs = 15000);
  bool ensureTimeSynced(uint32_t timeoutMs = 8000);

  // auth
  bool tokenValidSoon() const;
  bool ensureValidToken();
  bool authRefresh();
  bool httpPostJson(const String& url, const String& body, String& outResp, int& outCode);

  // mqtt
  void mqttBeginIfNeeded();
  void mqttSubscribeAll();

  void publishRegister();
  void publishStatusIfDue();
  void publishTelemetryIfDue();

  // mqtt handlers
  static void onRegisterConfirmStatic(char* topic, byte* payload, unsigned int length);
  static void onCommandStatic(char* topic, byte* payload, unsigned int length);
  static void onTopologyResultStatic(char* topic, byte* payload, unsigned int length);

  void onRegisterConfirm(char* topic, byte* payload, unsigned int length);
  void onCommand(char* topic, byte* payload, unsigned int length);
  void onTopologyResult(char* topic, byte* payload, unsigned int length);

  // topics
  String deviceKey() const; // auth_dkey
  String topicOf(const char* suffix) const; // device/{deviceKey}/{suffix}

  // topology -> espnow
  void loadTopologyFromNvs();

private:
  PreferenceService& _prefs;
  MqttService& _mqtt;
  Config _cfg;

  EspNowService _esp;

  bool _running = false;
  bool _mqttStarted = false;
  bool _registerConfirmed = false;

  uint32_t _lastMqttAttemptMs = 0;
  uint32_t _lastRegisterMs = 0;
  uint32_t _lastStatusMs = 0;
  uint32_t _lastTelemetryMs = 0;
  uint32_t _lastTokenCheckMs = 0;

  // static bridge for mqtt callbacks (PubSubClient style)
  static RunService* _self;
};
