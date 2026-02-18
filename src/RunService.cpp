#include "RunService.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <inttypes.h>
#include <ArduinoJson.h>

RunService *RunService::_self = nullptr;

static uint64_t nowUnix()
{
  time_t now = 0;
  time(&now);
  return (uint64_t)now;
}

static bool parseAndStoreTokens(PreferenceService &prefs, const String &respJson)
{
  JsonDocument doc;
  auto err = deserializeJson(doc, respJson);
  if (err)
  {
    Serial.print("[RUN] JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonVariant root = doc;
  if (doc["data"].is<JsonObject>())
    root = doc["data"];

  const char *access = root["accessToken"];
  const char *refresh = root["refreshToken"];

  long expiresIn = 0;
  if (root["expiresIn"].is<long>())
    expiresIn = root["expiresIn"].as<long>();
  else if (root["expiresIn"].is<int>())
    expiresIn = (long)root["expiresIn"].as<int>();
  else if (root["expiresIn"].is<const char *>())
    expiresIn = atol(root["expiresIn"].as<const char *>());

  if (!access || !refresh || expiresIn <= 0)
  {
    Serial.println("[RUN] Token payload invalid");
    return false;
  }

  const uint64_t exp = nowUnix() + (uint64_t)expiresIn;

  return prefs.updateAuthTokens(String(access), String(refresh), exp);
}

RunService::RunService(PreferenceService &prefs, MqttService &mqtt, const Config &cfg)
    : _prefs(prefs), _mqtt(mqtt), _cfg(cfg)
{
  _self = this;
}

void RunService::begin()
{
  _running = true;
  _registerConfirmed = false;

  // init espnow (won't break anything if it fails)
  if (_esp.begin())
  {
    Serial.println("[ESPNOW] ready");
    loadTopologyFromNvs();
  }
  else
  {
    Serial.println("[ESPNOW] init failed");
  }

  ensureWifiAndTime();

  if (!ensureValidToken())
  {
    Serial.println("[RUN] Token refresh failed -> reboot in 30s");
    delay(30000);
    ESP.restart();
    return;
  }

  mqttBeginIfNeeded();
  mqttSubscribeAll();

  publishRegister();
}

void RunService::loop()
{
  if (!_running)
    return;

  uint32_t nowMs = millis();

  // Token check/refresh cadence
  if (nowMs - _lastTokenCheckMs > _cfg.tokenCheckEveryMs)
  {
    _lastTokenCheckMs = nowMs;
    if (!ensureValidToken())
    {
      Serial.println("[RUN] Periodic refresh failed -> reboot in 30s");
      delay(30000);
      ESP.restart();
    }
  }

  // MQTT reconnect
  if (!_mqtt.connected())
  {
    if (nowMs - _lastMqttAttemptMs > 2000)
    {
      _lastMqttAttemptMs = nowMs;
      String access = _prefs.getAccessToken();
      String devKey = deviceKey();

      Serial.print("Attempting MQTT reconnect...\n");
      Serial.print("MQTT connect -> ");
      Serial.print(devKey);
      Serial.print("\n");

      _mqtt.connect(devKey.c_str(), devKey.c_str(), access.c_str());
    }
  }

  _mqtt.loop();

  // ESPNOW task loop (timeouts / queue)
  _esp.loop();

  // Register retry until confirmed
  if (!_registerConfirmed)
  {
    if (nowMs - _lastRegisterMs > _cfg.registerRetryMs)
    {
      publishRegister();
    }
  }

  // Periodic status/telemetry once confirmed
  if (_mqtt.connected() && _registerConfirmed)
  {
    publishStatusIfDue();
    publishTelemetryIfDue();
  }
}

void RunService::ensureWifiAndTime()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    if (!wifiConnectSTA())
    {
      Serial.println("[RUN] WiFi connect FAILED -> reboot in 30s");
      delay(30000);
      ESP.restart();
    }
  }

  if (!ensureTimeSynced())
  {
    Serial.println("[RUN] NTP sync failed -> reboot in 30s");
    delay(30000);
    ESP.restart();
  }
}

bool RunService::wifiConnectSTA(uint32_t timeoutMs)
{
  auto w = _prefs.loadWifi();
  if (w.ssid.length() == 0)
    return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(w.ssid.c_str(), w.password.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs)
  {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("[RUN] WiFi OK. IP=");
    Serial.println(WiFi.localIP());
    return true;
  }
  return false;
}

bool RunService::ensureTimeSynced(uint32_t timeoutMs)
{
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs)
  {
    time_t now;
    time(&now);
    if (now > 1700000000)
      return true;
    delay(250);
  }
  return false;
}

bool RunService::tokenValidSoon() const
{
  uint64_t exp = _prefs.getAccessExpUnix();
  uint64_t now = nowUnix();
  if (exp == 0 || now < 1700000000ULL)
    return false;
  return (now + (uint64_t)_cfg.tokenSkewSec) < exp;
}

bool RunService::ensureValidToken()
{
  if (tokenValidSoon())
    return true;
  Serial.println("[RUN] Token expiring/invalid -> refresh...");
  return authRefresh();
}

bool RunService::httpPostJson(const String &url, const String &body, String &outResp, int &outCode)
{
  HTTPClient http;
  WiFiClientSecure client;

  String ca = _prefs.loadCaCertPem();
  if (ca.length() > 0)
    client.setCACert(ca.c_str());
  else
    client.setInsecure();

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  outCode = http.POST(body);
  if (outCode > 0)
    outResp = http.getString();
  http.end();
  return outCode > 0;
}

bool RunService::authRefresh()
{
  String refresh = _prefs.getRefreshToken();
  if (refresh.length() == 0)
  {
    Serial.println("[RUN] Refresh token missing (auth_rt empty)");
    return false;
  }

  String url = String(_cfg.apiBase) + "/api/device/refreshtoken";
  String devKey = deviceKey();
  String body = "{\"refreshToken\":\"" + refresh + "\",\"deviceId\":\"" + devKey + "\"}";

  Serial.print("[RUN] Refresh token len=");
  Serial.print(refresh.length());
  Serial.print(" head=");
  Serial.print(refresh.substring(0, min(8, (int)refresh.length())));
  Serial.print("... devKey=");
  Serial.println(devKey);

  String resp;
  int code = 0;
  if (!httpPostJson(url, body, resp, code))
  {
    Serial.println("[RUN] Refresh HTTP call failed (transport)");
    return false;
  }

  Serial.print("[RUN] Refresh(POST) HTTP code: ");
  Serial.println(code);
  if (code < 200 || code >= 300)
  {
    Serial.println("[RUN] Refresh non-2xx response:");
    Serial.println(resp);
    return false;
  }

  bool ok = parseAndStoreTokens(_prefs, resp);
  if (!ok)
  {
    Serial.println("[RUN] parseAndStoreTokens failed. Response was:");
    Serial.println(resp);
  }
  else
  {
    uint64_t exp = _prefs.getAccessExpUnix();
    Serial.print("[RUN] Token stored. expUnix=");
    Serial.println((unsigned long long)exp);
  }
  return ok;
}

String RunService::deviceKey() const
{
  // V13: device identity key is stored as auth_dkey
  return _prefs.getDeviceKey();
}

String RunService::topicOf(const char *suffix) const
{
  return String("device/") + deviceKey() + "/" + suffix;
}

void RunService::mqttBeginIfNeeded()
{
  if (_mqttStarted)
    return;

  String ca = _prefs.loadCaCertPem();
  const char *caPem = ca.length() > 0 ? ca.c_str() : nullptr;

  // keep V13-ish defaults
  _mqtt.begin(caPem, 30, 15, 2048);
  _mqttStarted = true;

  // connect now
  String access = _prefs.getAccessToken();
  String devKey = deviceKey();

  Serial.print("MQTT connect -> ");
  Serial.print(_cfg.apiBase);
  Serial.print(" clientId/username=");
  Serial.print(devKey);
  Serial.print(" accessLen=");
  Serial.println(access.length());

  _mqtt.connect(devKey.c_str(), devKey.c_str(), access.c_str());
}

void RunService::mqttSubscribeAll()
{
  // register confirm always, command + topology only after confirmation
  const String tConfirm = topicOf("register/confirm");
  bool ok = _mqtt.subscribe(tConfirm.c_str(), 1, &RunService::onRegisterConfirmStatic);
  Serial.print("Subscribe confirm ");
  Serial.print(tConfirm);
  Serial.print(" -> ");
  Serial.println(ok ? "OK" : "FAIL");
}

void RunService::publishRegister()
{
  if (!_mqtt.connected())
  {
    Serial.println("[REGISTER] false -> keep retry");
    _lastRegisterMs = millis();
    return;
  }

  DynamicJsonDocument doc(512);
  doc["chipId"] = String((uint32_t)(ESP.getEfuseMac() >> 32), HEX) + String((uint32_t)ESP.getEfuseMac(), HEX);
  doc["firmwareVersion"] = _cfg.apiBase ? "0.15.0" : "0.15.0"; // keep your existing value if you patch later
  doc["macAddress"] = WiFi.macAddress();
  doc["wifiSsid"] = WiFi.SSID();

  String payload;
  serializeJson(doc, payload);

  const String t = topicOf("register");
  Serial.print("Publish register -> ");
  Serial.print(t);
  Serial.print(" payload=");
  Serial.println(payload);
  bool ok = _mqtt.publish(t.c_str(), payload.c_str());
  Serial.print("Publish result: ");
  Serial.println(ok ? "OK" : "FAIL");

  _lastRegisterMs = millis();
}

void RunService::publishStatusIfDue()
{
  uint32_t nowMs = millis();
  if (nowMs - _lastStatusMs < _cfg.statusEveryMs)
    return;
  _lastStatusMs = nowMs;

  DynamicJsonDocument doc(256);
  doc["wifi"] = (WiFi.status() == WL_CONNECTED);
  doc["rssi"] = WiFi.RSSI();
  doc["heap"] = ESP.getFreeHeap();

  String payload;
  serializeJson(doc, payload);

  const String t = topicOf("status");
  Serial.print("[STATUS] publish -> ");
  Serial.println(t);
  bool ok = _mqtt.publish(t.c_str(), payload.c_str());
  Serial.print("PUB -> ");
  Serial.print(t);
  Serial.print(" ok=");
  Serial.println(ok ? "true" : "false");
}

void RunService::publishTelemetryIfDue()
{
  uint32_t nowMs = millis();
  if (nowMs - _lastTelemetryMs < _cfg.telemetryEveryMs)
    return;
  _lastTelemetryMs = nowMs;

  // Keep current behavior: publish a heartbeat telemetry (real probe data comes via ESPNOW requests)
  DynamicJsonDocument doc(128);
  doc["alive"] = true;

  String payload;
  serializeJson(doc, payload);

  const String t = topicOf("telemetry");
  bool ok = _mqtt.publish(t.c_str(), payload.c_str());
  Serial.print("PUB -> ");
  Serial.print(t);
  Serial.print(" ok=");
  Serial.println(ok ? "true" : "false");
}

// -------------------- MQTT Static bridges --------------------
void RunService::onRegisterConfirmStatic(char *topic, byte *payload, unsigned int length)
{
  if (_self)
    _self->onRegisterConfirm(topic, payload, length);
}
void RunService::onCommandStatic(char *topic, byte *payload, unsigned int length)
{
  if (_self)
    _self->onCommand(topic, payload, length);
}
void RunService::onTopologyResultStatic(char *topic, byte *payload, unsigned int length)
{
  if (_self)
    _self->onTopologyResult(topic, payload, length);
}

// -------------------- MQTT handlers --------------------
void RunService::onRegisterConfirm(char *topic, byte *payload, unsigned int length)
{
  String t(topic);
  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++)
    body += (char)payload[i];

  Serial.print("MQTT IN [");
  Serial.print(t);
  Serial.print("] ");
  Serial.println(body);

  JsonDocument doc;
  auto err = deserializeJson(doc, body);
  if (err)
    return;

  bool isReg = false;
  if (doc["IsRegister"].is<bool>())
    isReg = doc["IsRegister"].as<bool>();
  else if (doc["isRegister"].is<bool>())
    isReg = doc["isRegister"].as<bool>();
  else if (doc["ok"].is<bool>())
    isReg = doc["ok"].as<bool>();

  if (!isReg)
  {
    Serial.println("[REGISTER] confirm received but false");
    return;
  }

  _registerConfirmed = true;
  Serial.println("[REGISTER] confirmed ✅");

  // unsubscribe confirm to stop noise
  bool uok = _mqtt.unsubscribe(topicOf("register/confirm").c_str());
  Serial.print("Unsubscribe confirm ");
  Serial.print(topicOf("register/confirm"));
  Serial.print(" -> ");
  Serial.println(uok ? "OK" : "FAIL");

  // subscribe command + topology/result
  const String tCmd = topicOf("command");
  const String tTopo = topicOf("topology/result");

  bool ok1 = _mqtt.subscribe(tCmd.c_str(), 1, &RunService::onCommandStatic);
  bool ok2 = _mqtt.subscribe(tTopo.c_str(), 1, &RunService::onTopologyResultStatic);

  Serial.print("Subscribe command ");
  Serial.print(tCmd);
  Serial.print(" -> ");
  Serial.println(ok1 ? "OK" : "FAIL");
  Serial.print("Subscribe topology ");
  Serial.print(tTopo);
  Serial.print(" -> ");
  Serial.println(ok2 ? "OK" : "FAIL");

  // allow immediate status after confirm
  _lastStatusMs = 0;
  _lastTelemetryMs = 0;
}

void RunService::onTopologyResult(char *topic, byte *payload, unsigned int length)
{
  String t(topic);
  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++)
    body += (char)payload[i];

  Serial.print("MQTT IN [");
  Serial.print(t);
  Serial.print("] ");
  Serial.println(body);

  // persist raw JSON
  _prefs.saveTopologyJson(body);

  JsonDocument doc;
  auto err = deserializeJson(doc, body);
  if (err)
    return;

  JsonArray probes = doc["Probes"].as<JsonArray>();
  if (probes.isNull())
    probes = doc["probes"].as<JsonArray>();
  if (probes.isNull())
    return;

  uint32_t added = 0;
  for (JsonVariant v : probes)
  {
    String macStr = v["MacAddress"].is<const char *>() ? String(v["MacAddress"].as<const char *>()) : String(v["macAddress"].as<const char *>());
    String lmkHex = v["Lmk"].is<const char *>() ? String(v["Lmk"].as<const char *>()) : String(v["lmk"].as<const char *>());
    String dkey = v["DeviceKey"].is<const char *>() ? String(v["DeviceKey"].as<const char *>()) : String(v["deviceKey"].as<const char *>());

    uint8_t mac[6];
    if (!EspNowService::parseMac(macStr, mac))
      continue;

    EspNowService::Peer p;
    memcpy(p.mac, mac, 6);
    p.deviceKey = dkey;

    if (lmkHex.length() == 32 && EspNowService::hexTo16(lmkHex, p.lmk))
    {
      p.hasLmk = true;
    }

    _esp.upsertPeer(p);
    added++;
  }

  Serial.print("[TOPOLOGY] stored. peers updated: ");
  Serial.println(added);
}

void RunService::onCommand(char *topic, byte *payload, unsigned int length)
{
  String t(topic);
  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++)
    body += (char)payload[i];

  Serial.print("MQTT IN [");
  Serial.print(t);
  Serial.print("] ");
  Serial.println(body);

  JsonDocument doc;
  auto err = deserializeJson(doc, body);
  if (err)
    return;

  const String cmd = doc["command"].is<const char *>() ? String(doc["command"].as<const char *>()) : String("");
  const String correlationId = doc["correlationId"].is<const char *>() ? String(doc["correlationId"].as<const char *>()) : String("");

  if (cmd != "TelemetryDevice")
  {
    // keep other commands as-is (ignored here)
    return;
  }

  const String macStr = doc["macAddress"].is<const char *>() ? String(doc["macAddress"].as<const char *>()) : String("");

  // ACK immediately
  {
    StaticJsonDocument<256> ack;
    ack["correlationId"] = correlationId;
    ack["ok"] = true;
    String out;
    serializeJson(ack, out);

    const String tAck = topicOf("command/ack");
    _mqtt.publish(tAck.c_str(), out.c_str());
  }

  uint8_t mac[6];
  if (correlationId.length() != 32 || !EspNowService::parseMac(macStr, mac))
  {
    StaticJsonDocument<256> res;
    res["correlationId"] = correlationId;
    res["macAddress"] = macStr;
    res["ok"] = false;
    res["error"] = "bad_args";
    String out;
    serializeJson(res, out);

    const String tRes = topicOf("command/result");
    _mqtt.publish(tRes.c_str(), out.c_str());
    return;
  }

  bool queued = _esp.requestTelemetryByMac(
      mac,
      correlationId,
      [this, correlationId, macStr](const EspNowService::TelemetryResponse &r)
      {
        StaticJsonDocument<512> res;
        res["correlationId"] = correlationId;
        res["macAddress"] = macStr;
        res["ok"] = r.ok;

        if (r.ok)
        {
          res["uid"] = r.uid;
          res["weight"] = r.weight;
          res["variance"] = r.variance;
          res["tagAtMs"] = r.tagAtMs;
          res["weightAtMs"] = r.weightAtMs;
        }
        else
        {
          res["error"] = "timeout";
        }

        String out;
        serializeJson(res, out);

        const String tRes = topicOf("command/result");
        _mqtt.publish(tRes.c_str(), out.c_str());
      },
      _cfg.espnowTimeoutMs,
      _cfg.espnowRetries);

  if (!queued)
  {
    StaticJsonDocument<256> res;
    res["correlationId"] = correlationId;
    res["macAddress"] = macStr;
    res["ok"] = false;
    res["error"] = "queue_full";
    String out;
    serializeJson(res, out);

    const String tRes = topicOf("command/result");
    _mqtt.publish(tRes.c_str(), out.c_str());
  }
}

void RunService::loadTopologyFromNvs()
{
  String json = _prefs.loadTopologyJson();
  if (json.length() == 0)
    return;

  JsonDocument doc;
  auto err = deserializeJson(doc, json);
  if (err)
    return;

  JsonArray probes = doc["Probes"].as<JsonArray>();
  if (probes.isNull())
    probes = doc["probes"].as<JsonArray>();
  if (probes.isNull())
    return;

  uint32_t added = 0;
  for (JsonVariant v : probes)
  {
    String macStr = v["MacAddress"].is<const char *>() ? String(v["MacAddress"].as<const char *>()) : String(v["macAddress"].as<const char *>());
    String lmkHex = v["Lmk"].is<const char *>() ? String(v["Lmk"].as<const char *>()) : String(v["lmk"].as<const char *>());
    String dkey = v["DeviceKey"].is<const char *>() ? String(v["DeviceKey"].as<const char *>()) : String(v["deviceKey"].as<const char *>());

    uint8_t mac[6];
    if (!EspNowService::parseMac(macStr, mac))
      continue;

    EspNowService::Peer p;
    memcpy(p.mac, mac, 6);
    p.deviceKey = dkey;
    if (lmkHex.length() == 32 && EspNowService::hexTo16(lmkHex, p.lmk))
      p.hasLmk = true;

    _esp.upsertPeer(p);
    added++;
  }

  Serial.print("[ESPNOW] peers loaded from NVS: ");
  Serial.println(added);
}
