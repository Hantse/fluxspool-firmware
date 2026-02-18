#pragma once
#include <Arduino.h>
#include <WebServer.h>

#include "PreferenceService.h"
#include "ProbeNowLink.h"

// ProbeRunService
// - Connects to WiFi
// - Ensures token is valid (refresh if needed)
// - Calls POST /api/device/register/probe with Bearer token
// - Stores gatewayMac + lmk + gatewayHmac in NVS
// - Disconnects WiFi and switches to ESPNOW-only

class ProbeRunService
{
public:
  struct Config
  {
    const char *apiBase = "https://api.fluxspool.app";
    uint32_t tokenSkewSec = 60;
    uint32_t tokenCheckEveryMs = 30000;
    uint32_t registerRetryMs = 2000;
  };

  ProbeRunService(PreferenceService &prefs, const Config &cfg);

  void begin();
  void loop();

private:
  void ensureWifiAndTime();
  bool wifiConnectSTA(uint32_t timeoutMs = 15000);
  bool ensureTimeSynced(uint32_t timeoutMs = 8000);

  bool tokenValidSoon() const;
  bool ensureValidToken();
  bool authRefresh();

  bool registerProbe();
  bool httpPostJson(const String &url, const String &body, String &outResp, int &outCode);

  bool ensureEspNow();

  // identity
  String probeId() const;
  String deviceKey() const;
  String model() const;
  String firmwareVersion() const;

  static void onRxStatic(const uint8_t *mac, const uint8_t *data, int len);
  void onRx(const uint8_t *mac, const uint8_t *data, int len);

private:
  PreferenceService &_prefs;
  Config _cfg;

  bool _running = false;
  bool _espOnly = false;

  uint32_t _lastTokenCheckMs = 0;
  uint32_t _nextRegisterMs = 0;

  ProbeNowLink _link;
  static ProbeRunService *_self;
};
