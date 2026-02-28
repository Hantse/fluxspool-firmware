#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>

#include "PreferenceService.h"

class OtaService
{
public:
  enum class Result : uint8_t
  {
    Ok = 0,
    BadArgs,
    WifiMissing,
    WifiConnectFailed,
    HttpBeginFailed,
    HttpGetFailed,
    NoContentLength,
    UpdateBeginFailed,
    StreamError,
    UpdateWriteFailed,
    UpdateEndFailed
  };

  struct Config
  {
    uint32_t wifiTimeoutMs;
    uint32_t httpTimeoutMs;
    bool allowInsecureIfNoCa;

    Config()
        : wifiTimeoutMs(20000),
          httpTimeoutMs(30000),
          allowInsecureIfNoCa(true)
    {
    }
  };

  using LogFn = void (*)(const char *msg);

  OtaService(PreferenceService &prefs);
  OtaService(PreferenceService &prefs, const Config &cfg);

  Result runGateway(const String &url, LogFn log = nullptr);
  Result runProbe(const String &url, LogFn log = nullptr);

private:
  bool ensureWifiConnected(LogFn log);
  Result runUpdate(const String &url, LogFn log);

private:
  PreferenceService &_prefs;
  Config _cfg;
};