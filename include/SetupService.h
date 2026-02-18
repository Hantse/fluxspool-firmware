#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "PreferenceService.h"

class SetupService {
public:
  struct Config {
    const char* apSsid = "FluxSpool-Setup";
    const char* apiBase = "https://api.fluxspool.app";
  };

  SetupService(PreferenceService& prefs, WebServer& server, const Config& cfg);

  // Returns true when the device is ready to run runtime mode (STA creds + tokens, no pending codes)
  bool isSetupComplete() const;

  // Starts either AP portal (if needed) or runs provisioning (if codes exist)
  void begin();

  // Call frequently from loop() when NOT setup complete
  void loop();

private:
  void startPortal();
  void stopPortal();

  bool wifiConnectSTA(uint32_t timeoutMs = 15000);
  bool ensureTimeSynced(uint32_t timeoutMs = 8000);
  bool authProvision();

  static String readPayloadToString(const uint8_t* payload, size_t len);

private:
  PreferenceService& _prefs;
  WebServer& _server;
  Config _cfg;

  bool _portalStarted = false;
};
