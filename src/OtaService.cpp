#include "OtaService.h"

OtaService::OtaService(PreferenceService &prefs)
    : _prefs(prefs), _cfg()
{
}

OtaService::OtaService(PreferenceService &prefs, const Config &cfg)
    : _prefs(prefs), _cfg(cfg)
{
}

static void logLine(OtaService::LogFn log, const String &s)
{
  if (log)
    log(s.c_str());
  Serial.println(s);
}

bool OtaService::ensureWifiConnected(LogFn log)
{
  if (WiFi.status() == WL_CONNECTED)
    return true;

  if (!_prefs.hasWifi())
  {
    logLine(log, "[OTA] WiFi config missing in NVS");
    return false;
  }

  auto w = _prefs.loadWifi();
  if (w.ssid.length() == 0)
  {
    logLine(log, "[OTA] WiFi SSID empty");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  logLine(log, String("[OTA] Connecting WiFi SSID=") + w.ssid);
  WiFi.begin(w.ssid.c_str(), w.password.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < _cfg.wifiTimeoutMs)
  {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    logLine(log, "[OTA] WiFi connect FAILED");
    return false;
  }

  logLine(log, String("[OTA] WiFi connected IP=") + WiFi.localIP().toString());
  return true;
}

OtaService::Result OtaService::runGateway(const String &url, LogFn log)
{
  if (url.length() < 8)
    return Result::BadArgs;

  // Gateway is already connected; but if it dropped, try reconnect using stored creds
  if (!ensureWifiConnected(log))
    return Result::WifiConnectFailed;

  return runUpdate(url, log);
}

OtaService::Result OtaService::runProbe(const String &url, LogFn log)
{
  if (url.length() < 8)
    return Result::BadArgs;

  if (!_prefs.hasWifi())
    return Result::WifiMissing;

  if (!ensureWifiConnected(log))
    return Result::WifiConnectFailed;

  auto r = runUpdate(url, log);

  // If OTA failed, we can disconnect WiFi so ESPNOW can be re-started by caller
  if (r != Result::Ok)
  {
    logLine(log, "[OTA] Failure -> WiFi disconnect (probe)");
    WiFi.disconnect(true, true);
    delay(100);
  }

  return r;
}

OtaService::Result OtaService::runUpdate(const String &url, LogFn log)
{
  logLine(log, String("[OTA] Start URL=") + url);

  HTTPClient http;
  WiFiClientSecure client;

  client.setTimeout(_cfg.httpTimeoutMs / 1000);

  String ca = _prefs.loadCaCertPem();
  if (ca.length() > 0)
  {
    client.setCACert(ca.c_str());
    logLine(log, "[OTA] Using stored CA cert");
  }
  else
  {
    if (_cfg.allowInsecureIfNoCa)
    {
      client.setInsecure();
      logLine(log, "[OTA] No CA cert -> INSECURE HTTPS");
    }
    else
    {
      logLine(log, "[OTA] No CA cert and insecure disabled");
      return Result::HttpBeginFailed;
    }
  }

  http.setTimeout(_cfg.httpTimeoutMs);

  if (!http.begin(client, url))
  {
    logLine(log, "[OTA] http.begin failed");
    http.end();
    return Result::HttpBeginFailed;
  }

  int code = http.GET();
  if (code <= 0)
  {
    logLine(log, String("[OTA] HTTP GET failed code=") + code);
    http.end();
    return Result::HttpGetFailed;
  }

  if (code < 200 || code >= 300)
  {
    logLine(log, String("[OTA] HTTP non-2xx code=") + code);
    String body = http.getString();
    if (body.length())
      logLine(log, String("[OTA] body: ") + body.substring(0, 200));
    http.end();
    return Result::HttpGetFailed;
  }

  int len = http.getSize();
  if (len <= 0)
  {
    // Some servers might stream chunked without content-length.
    // Update can work without size sometimes, but is less reliable; keep strict for now.
    logLine(log, "[OTA] Missing/invalid Content-Length");
    http.end();
    return Result::NoContentLength;
  }

  logLine(log, String("[OTA] Content-Length=") + len);

  if (!Update.begin((size_t)len))
  {
    logLine(log, String("[OTA] Update.begin failed err=") + Update.getError());
    http.end();
    return Result::UpdateBeginFailed;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buff[1024];

  while (http.connected() && written < (size_t)len)
  {
    size_t avail = stream->available();
    if (avail)
    {
      int toRead = (int)min(avail, sizeof(buff));
      int r = stream->readBytes(buff, toRead);
      if (r <= 0)
      {
        logLine(log, "[OTA] Stream read error");
        Update.abort();
        http.end();
        return Result::StreamError;
      }

      size_t w = Update.write(buff, (size_t)r);
      if (w != (size_t)r)
      {
        logLine(log, String("[OTA] Update.write failed err=") + Update.getError());
        Update.abort();
        http.end();
        return Result::UpdateWriteFailed;
      }

      written += w;
    }
    delay(1);
  }

  if (written != (size_t)len)
  {
    logLine(log, String("[OTA] Incomplete download written=") + written + " expected=" + len);
    Update.abort();
    http.end();
    return Result::StreamError;
  }

  if (!Update.end(true))
  {
    logLine(log, String("[OTA] Update.end failed err=") + Update.getError());
    http.end();
    return Result::UpdateEndFailed;
  }

  http.end();

  logLine(log, "[OTA] Success -> rebooting");
  delay(250);
  ESP.restart();

  return Result::Ok; // unreachable (restart), but ok
}