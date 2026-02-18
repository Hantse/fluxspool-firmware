#include "ProbeRunService.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>

ProbeRunService *ProbeRunService::_self = nullptr;

static uint64_t nowUnix()
{
  time_t now = 0;
  time(&now);
  return (uint64_t)now;
}

static String macNoSep()
{
  String m = WiFi.macAddress();
  m.replace(":", "");
  m.toLowerCase();
  return m;
}

ProbeRunService::ProbeRunService(PreferenceService &prefs, const Config &cfg)
    : _prefs(prefs), _cfg(cfg)
{
  _self = this;
}

void ProbeRunService::begin()
{
  _running = true;
  _espOnly = false;
  _lastTokenCheckMs = 0;
  _nextRegisterMs = 0;
  Serial.println("[PROBE] begin");

  ensureWifiAndTime();
}

void ProbeRunService::loop()
{
  if (!_running)
    return;

  // If we already switched to ESPNOW only, just run periodic work
  if (_espOnly)
  {
    // TODO: heartbeat / telemetry
    static uint32_t last = 0;
    if (millis() - last > 5000)
    {
      last = millis();
      const char *msg = "probe:heartbeat";
      _link.send((const uint8_t *)msg, strlen(msg));
    }
    delay(5);
    return;
  }

  ensureWifiAndTime();

  // token maintenance
  if ((int32_t)(millis() - _lastTokenCheckMs) > (int32_t)_cfg.tokenCheckEveryMs)
  {
    _lastTokenCheckMs = millis();
    if (!ensureValidToken())
    {
      Serial.println("[PROBE] token invalid and refresh failed");
      delay(250);
      return;
    }
  }

  // registerProbe if needed
  if (!_prefs.hasProbeNowConfig())
  {
    if ((int32_t)(millis() - _nextRegisterMs) >= 0)
    {
      if (registerProbe())
      {
        Serial.println("[PROBE] registerProbe OK");
      }
      else
      {
        Serial.println("[PROBE] registerProbe failed, will retry");
        _nextRegisterMs = millis() + _cfg.registerRetryMs;
      }
    }
    delay(20);
    return;
  }

  // we have gatewayMac + lmk, switch to ESPNOW
  if (ensureEspNow())
  {
    Serial.println("[PROBE] switched to ESPNOW-only");
    _espOnly = true;
    // hard cut WiFi
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_STA);
  }

  delay(20);
}

void ProbeRunService::ensureWifiAndTime()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    wifiConnectSTA();
  }
  // time is needed for exp checks
  ensureTimeSynced();
}

bool ProbeRunService::wifiConnectSTA(uint32_t timeoutMs)
{
  String ssid = _prefs.getString("wifi_ssid", "");
  String pass = _prefs.getString("wifi_pass", "");
  if (ssid.length() == 0)
    return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs)
  {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool ProbeRunService::ensureTimeSynced(uint32_t timeoutMs)
{
  time_t now;
  time(&now);
  if (now > 1700000000)
    return true;

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs)
  {
    time(&now);
    if (now > 1700000000)
      return true;
    delay(250);
  }
  return false;
}

bool ProbeRunService::tokenValidSoon() const
{
  uint64_t exp = _prefs.getAccessExpUnix();
  if (exp == 0)
    return false;
  uint64_t now = nowUnix();
  if (now == 0)
    return true; // if time unknown, don't block
  return (exp > (now + (uint64_t)_cfg.tokenSkewSec));
}

bool ProbeRunService::ensureValidToken()
{
  // must have access token
  if (_prefs.getAccessToken().length() == 0)
    return false;

  if (tokenValidSoon())
    return true;
  return authRefresh();
}

bool ProbeRunService::authRefresh()
{
  String refresh = _prefs.getRefreshToken();
  if (refresh.length() == 0)
  {
    Serial.println("[PROBE] refresh token missing");
    return false;
  }

  String devId = deviceId();
  if (devId.length() == 0)
  {
    Serial.println("[PROBE] deviceId missing");
    return false;
  }

  String url = String(_cfg.apiBase) + "/api/device/refreshtoken";
  String body = String("{\"refreshToken\":\"") + refresh + "\",\"deviceId\":\"" + devId + "\"}";

  String resp;
  int code = 0;
  if (!httpPostJson(url, body, resp, code))
    return false;
  if (code < 200 || code >= 300)
  {
    Serial.printf("[PROBE] refresh HTTP %d\n", code);
    Serial.println(resp);
    return false;
  }

  // parse same shape as setup provisioning response
  JsonDocument doc;
  if (deserializeJson(doc, resp))
    return false;
  JsonVariant root = doc;
  if (doc["data"].is<JsonObject>())
    root = doc["data"];

  const char *access = root["accessToken"];
  const char *refresh2 = root["refreshToken"];
  long expiresIn =
      root["expiresIn"].is<long>() ? root["expiresIn"].as<long>() : root["expiresIn"].is<int>()        ? (long)root["expiresIn"].as<int>()
                                                                : root["expiresIn"].is<const char *>() ? atol(root["expiresIn"].as<const char *>())
                                                                                                       : 0;

  if (!access || !refresh2 || expiresIn <= 0)
    return false;

  const uint64_t exp = nowUnix() + (uint64_t)expiresIn;

  // store in BOTH legacy and auth_* so future code can use either
  _prefs.setStringChecked("access", String(access));
  _prefs.setStringChecked("refresh", String(refresh2));
  _prefs.setU64("access_exp", exp);

  _prefs.setString("auth_at", String(access));
  _prefs.setString("auth_rt", String(refresh2));
  _prefs.setU64("auth_at_exp", exp);

  return true;
}

bool ProbeRunService::registerProbe()
{
  String access = _prefs.getAccessToken();
  String devId = deviceId();
  if (access.length() == 0 || devId.length() == 0)
    return false;

  String url = String(_cfg.apiBase) + "/api/device/register/probe";

  // build request
  JsonDocument doc;
  doc["probeId"] = probeId();
  doc["deviceId"] = devId;
  doc["mac"] = WiFi.macAddress();
  doc["chipId"] = String((uint32_t)ESP.getEfuseMac(), HEX);
  doc["firmwareVersion"] = firmwareVersion();
  doc["wifiSsid"] = _prefs.getString("wifi_ssid", "");
  doc["model"] = model();

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure(); // you can switch to CA cert store later

  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + access);

  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : "";
  http.end();

  Serial.printf("[PROBE] registerProbe HTTP %d\n", code);
  if (code < 200 || code >= 300)
  {
    Serial.println(resp);
    return false;
  }

  JsonDocument r;
  if (deserializeJson(r, resp))
    return false;
  JsonVariant root = r;
  if (r["data"].is<JsonObject>())
    root = r["data"];

  const char *gwMac = root["gatewayMac"];
  const char *lmk = root["lmk"];
  const char *gwHmac = root["gatewayHmac"];

  if (!gwMac || !lmk)
    return false;

  PreferenceService::ProbeNowConfig cfg;
  cfg.gatewayMac = String(gwMac);
  cfg.lmk = String(lmk);
  cfg.gatewayHmac = gwHmac ? String(gwHmac) : String("");
  return _prefs.saveProbeNowConfig(cfg);
}

bool ProbeRunService::httpPostJson(const String &url, const String &body, String &outResp, int &outCode)
{
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url))
    return false;
  http.addHeader("Content-Type", "application/json");

  outCode = http.POST(body);
  outResp = (outCode > 0) ? http.getString() : "";
  http.end();
  return outCode != 0;
}

bool ProbeRunService::ensureEspNow()
{
  auto cfg = _prefs.loadProbeNowConfig();
  if (cfg.gatewayMac.length() == 0 || cfg.lmk.length() == 0)
    return false;

  ProbeNowLink::PeerConfig peer{};
  if (!ProbeNowLink::parseMac(cfg.gatewayMac, peer.mac))
  {
    Serial.println("[PROBE] invalid gatewayMac");
    return false;
  }
  if (!ProbeNowLink::decodeKey16(cfg.lmk, peer.lmk))
  {
    Serial.println("[PROBE] invalid lmk");
    return false;
  }
  peer.hasLmk = true;

  // disconnect WiFi but keep STA mode
  WiFi.disconnect(true, true);
  delay(100);

  return _link.begin(peer, &ProbeRunService::onRxStatic);
}

String ProbeRunService::probeId() const
{
  // stable id derived from MAC
  return String("probe-") + macNoSep();
}

String ProbeRunService::deviceId() const
{
  return _prefs.getDeviceKey();
}

String ProbeRunService::model() const
{
  return "FluxSpool-Probe";
}

String ProbeRunService::firmwareVersion() const
{
  return "0.0.1";
}

void ProbeRunService::onRxStatic(const uint8_t *mac, const uint8_t *data, int len)
{
  if (_self)
    _self->onRx(mac, data, len);
}

void ProbeRunService::onRx(const uint8_t *mac, const uint8_t *data, int len)
{
  Serial.printf("[PNOW][RX] len=%d\n", len);
  // TODO: parse gateway commands (tare, telemetry request, etc.)
}
