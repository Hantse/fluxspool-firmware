#include "StandaloneRunService.h"
#include "NetUtils.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#ifndef FW_VERSION
#define FW_VERSION "0.0.0"
#endif

StandaloneRunService *StandaloneRunService::_self = nullptr;

StandaloneRunService::StandaloneRunService(PreferenceService &prefs, MqttService &mqtt, const Config &cfg)
    : _prefs(prefs), _mqtt(mqtt), _cfg(cfg), _ota(_prefs)
{
  _self = this;
}

void StandaloneRunService::begin()
{
  _running = true;
  _registerConfirmed = false;

  ensureWifiAndTime();

  if (!ensureValidToken())
  {
    Serial.println("[STANDALONE] Token refresh failed -> reboot in 30s");
    delay(30000);
    ESP.restart();
    return;
  }

  mqttBeginIfNeeded();
  mqttSubscribeAll();

  publishRegister();
}

void StandaloneRunService::loop()
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
      Serial.println("[STANDALONE] Periodic refresh failed -> reboot in 30s");
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
      Serial.println("Attempting MQTT reconnect...");
      _mqtt.connect(devKey.c_str(), devKey.c_str(), access.c_str());
    }
  }

  _mqtt.loop();

  // Register retry until confirmed
  if (!_registerConfirmed)
  {
    if (nowMs - _lastRegisterMs > _cfg.registerRetryMs)
      publishRegister();
  }

  // Periodic status/telemetry once confirmed
  if (_mqtt.connected() && _registerConfirmed)
  {
    publishStatusIfDue();
    publishTelemetryIfDue();
  }
}

void StandaloneRunService::ensureWifiAndTime()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    if (!wifiConnectSTA())
    {
      Serial.println("[STANDALONE] WiFi connect FAILED -> reboot in 30s");
      delay(30000);
      ESP.restart();
    }
  }

  if (!ensureTimeSynced())
  {
    Serial.println("[STANDALONE] NTP sync failed -> reboot in 30s");
    delay(30000);
    ESP.restart();
  }
  else
  {
    Serial.println("[STANDALONE] Time synced");
  }
}

bool StandaloneRunService::wifiConnectSTA(uint32_t timeoutMs)
{
  return netutils::wifiConnectSTA(_prefs, timeoutMs);
}

bool StandaloneRunService::ensureTimeSynced(uint32_t timeoutMs)
{
  return netutils::ensureTimeSynced(timeoutMs);
}

bool StandaloneRunService::tokenValidSoon() const
{
  uint64_t exp = _prefs.getAccessExpUnix();
  if (exp == 0)
    return false;
  uint64_t now = netutils::nowUnix();
  if (!netutils::timeIsValid(now))
    return true;
  return (now + (uint64_t)_cfg.tokenSkewSec) < exp;
}

bool StandaloneRunService::ensureValidToken()
{
  if (tokenValidSoon())
    return true;
  Serial.println("[STANDALONE] Token expiring/invalid -> refresh...");
  return authRefresh();
}

bool StandaloneRunService::httpPostJson(const String &url, const String &body, String &outResp, int &outCode)
{
  HTTPClient http;
  WiFiClientSecure client;

  String ca = _prefs.loadCaCertPem();
  if (ca.length() == 0)
  {
    Serial.println("[STANDALONE] No CA cert in NVS -> aborting HTTPS request");
    return false;
  }
  client.setCACert(ca.c_str());

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  outCode = http.POST(body);
  if (outCode > 0)
    outResp = http.getString();
  http.end();
  return outCode > 0;
}

bool StandaloneRunService::authRefresh()
{
  String refresh = _prefs.getRefreshToken();
  if (refresh.length() == 0)
  {
    Serial.println("[STANDALONE] Refresh token missing");
    return false;
  }

  String url = String(_cfg.apiBase) + "/api/device/refreshtoken";
  String devKey = deviceKey();
  JsonDocument bodyDoc;
  bodyDoc["refreshToken"] = refresh;
  bodyDoc["deviceId"] = devKey;
  String body;
  serializeJson(bodyDoc, body);

  String resp;
  int code = 0;
  if (!httpPostJson(url, body, resp, code))
  {
    Serial.println("[STANDALONE] Refresh HTTP call failed (transport)");
    return false;
  }

  if (code < 200 || code >= 300)
  {
    Serial.printf("[STANDALONE] Refresh non-2xx HTTP %d\n", code);
    Serial.println(resp);
    return false;
  }

  return netutils::storeTokenResponse(_prefs, resp);
}

String StandaloneRunService::deviceKey() const
{
  return _prefs.getDeviceKey();
}

String StandaloneRunService::topicOf(const char *suffix) const
{
  return String("device/") + deviceKey() + "/" + suffix;
}

void StandaloneRunService::publishCommandResult(const JsonDocument &doc)
{
  String out;
  serializeJson(doc, out);
  const String t = topicOf("command/result");
  _mqtt.publish(t.c_str(), out.c_str());
}

void StandaloneRunService::mqttBeginIfNeeded()
{
  if (_mqttStarted)
    return;

  String ca = _prefs.loadCaCertPem();
  const char *caPem = ca.length() > 0 ? ca.c_str() : nullptr;
  _mqtt.begin(caPem, 30, 15, 2048);
  _mqttStarted = true;

  String access = _prefs.getAccessToken();
  String devKey = deviceKey();

  Serial.print("MQTT connect -> ");
  Serial.print(_cfg.mqttBase);
  Serial.print(" clientId=");
  Serial.println(devKey);

  _mqtt.connect(devKey.c_str(), devKey.c_str(), access.c_str());
}

void StandaloneRunService::mqttSubscribeAll()
{
  const String tConfirm = topicOf("register/confirm");
  bool ok = _mqtt.subscribe(tConfirm.c_str(), 1, &StandaloneRunService::onRegisterConfirmStatic);
  Serial.print("Subscribe confirm ");
  Serial.print(tConfirm);
  Serial.print(" -> ");
  Serial.println(ok ? "OK" : "FAIL");
}

void StandaloneRunService::publishRegister()
{
  if (!_mqtt.connected())
  {
    _lastRegisterMs = millis();
    return;
  }

  JsonDocument doc;
  doc["chipId"] = String((uint32_t)(ESP.getEfuseMac() >> 32), HEX) + String((uint32_t)ESP.getEfuseMac(), HEX);
  doc["firmwareVersion"] = FW_VERSION;
  doc["macAddress"] = WiFi.macAddress();
  doc["wifiSsid"] = WiFi.SSID();

  String payload;
  serializeJson(doc, payload);

  const String t = topicOf("register");
  Serial.print("Publish register -> ");
  Serial.println(t);
  _mqtt.publish(t.c_str(), payload.c_str());

  _lastRegisterMs = millis();
}

void StandaloneRunService::publishStatusIfDue()
{
  uint32_t nowMs = millis();
  if (nowMs - _lastStatusMs < _cfg.statusEveryMs)
    return;
  _lastStatusMs = nowMs;

  JsonDocument doc;
  doc["wifi"] = (WiFi.status() == WL_CONNECTED);
  doc["rssi"] = WiFi.RSSI();
  doc["heap"] = ESP.getFreeHeap();

  String payload;
  serializeJson(doc, payload);

  const String t = topicOf("status");
  _mqtt.publish(t.c_str(), payload.c_str());
}

void StandaloneRunService::publishTelemetryIfDue()
{
  uint32_t nowMs = millis();
  if (nowMs - _lastTelemetryMs < _cfg.telemetryEveryMs)
    return;
  _lastTelemetryMs = nowMs;

  JsonDocument doc;
  doc["alive"] = true;
  // TODO: populate with last-known weight / tag when sensor integration is done

  String payload;
  serializeJson(doc, payload);

  const String t = topicOf("telemetry");
  _mqtt.publish(t.c_str(), payload.c_str());
}

// -------------------- MQTT static bridges --------------------
void StandaloneRunService::onRegisterConfirmStatic(char *topic, byte *payload, unsigned int length)
{
  if (_self)
    _self->onRegisterConfirm(topic, payload, length);
}
void StandaloneRunService::onCommandStatic(char *topic, byte *payload, unsigned int length)
{
  if (_self)
    _self->onCommand(topic, payload, length);
}

// -------------------- MQTT handlers --------------------
void StandaloneRunService::onRegisterConfirm(char *topic, byte *payload, unsigned int length)
{
  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++)
    body += (char)payload[i];

  Serial.print("MQTT IN [register/confirm] ");
  Serial.println(body);

  JsonDocument doc;
  if (deserializeJson(doc, body))
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
  Serial.println("[REGISTER] confirmed");

  _mqtt.unsubscribe(topicOf("register/confirm").c_str());

  const String tCmd = topicOf("command");
  bool ok = _mqtt.subscribe(tCmd.c_str(), 1, &StandaloneRunService::onCommandStatic);
  Serial.print("Subscribe command ");
  Serial.print(tCmd);
  Serial.print(" -> ");
  Serial.println(ok ? "OK" : "FAIL");

  _lastStatusMs = 0;
  _lastTelemetryMs = 0;
}

void StandaloneRunService::onCommand(char *topic, byte *payload, unsigned int length)
{
  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++)
    body += (char)payload[i];

  Serial.print("MQTT IN [command] ");
  Serial.println(body);

  JsonDocument doc;
  if (deserializeJson(doc, body))
    return;

  const String cmd = doc["command"].is<const char *>() ? String(doc["command"].as<const char *>()) : String("");
  const String correlationId = doc["correlationId"].is<const char *>() ? String(doc["correlationId"].as<const char *>()) : String("");

  // ---- OTA ----
  if (cmd == "Ota" || cmd == "OTA")
  {
    const String url = doc["url"].is<const char *>()   ? String(doc["url"].as<const char *>())
                       : doc["Url"].is<const char *>() ? String(doc["Url"].as<const char *>())
                                                       : String("");
    handleOta(correlationId, url);
    return;
  }

  // ---- ACK immediately for sensor commands ----
  {
    JsonDocument ack;
    ack["correlationId"] = correlationId;
    ack["ok"] = true;
    String out;
    serializeJson(ack, out);
    _mqtt.publish(topicOf("command/ack").c_str(), out.c_str());
  }

  if (cmd == "Scan")
  {
    handleScan(correlationId);
  }
  else if (cmd == "Weight")
  {
    handleWeight(correlationId);
  }
  else if (cmd == "Color")
  {
    handleColor(correlationId);
  }
  else if (cmd == "WriteRfid")
  {
    const String uid = doc["uid"].is<const char *>() ? String(doc["uid"].as<const char *>()) : String("");
    const String data = doc["data"].is<const char *>() ? String(doc["data"].as<const char *>()) : String("");
    handleWriteRfid(correlationId, uid, data);
  }
  else
  {
    JsonDocument res;
    res["correlationId"] = correlationId;
    res["ok"] = false;
    res["error"] = "unknown_command";
    publishCommandResult(res);
  }
}

// -------------------- Command handlers --------------------

void StandaloneRunService::handleScan(const String &correlationId)
{
  // TODO: read RFID tag from your reader (e.g. MFRC522 / PN532)
  // Example stub: always returns "not found"
  // Replace with actual hardware read:
  //   String uid = rfid.readUid();
  //   bool ok = uid.length() > 0;

  bool ok = false;
  String uid = "";

  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = ok;
  if (ok)
    res["uid"] = uid;
  else
    res["error"] = "no_tag";

  publishCommandResult(res);
}

void StandaloneRunService::handleWeight(const String &correlationId)
{
  // TODO: read weight from your scale (e.g. HX711)
  // Example stub:
  //   float weight_g = scale.getUnits();
  //   bool ok = true;

  bool ok = false;
  float weight_g = 0.0f;

  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = ok;
  if (ok)
    res["weight_g"] = weight_g;
  else
    res["error"] = "sensor_unavailable";

  publishCommandResult(res);
}

void StandaloneRunService::handleColor(const String &correlationId)
{
  // TODO: read RGB from your color sensor (e.g. TCS34725)
  // Example stub:
  //   uint16_t r, g, b, c;
  //   colorSensor.getRawData(&r, &g, &b, &c);
  //   bool ok = true;

  bool ok = false;
  uint16_t r = 0, g = 0, b = 0;

  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = ok;
  if (ok)
  {
    res["r"] = r;
    res["g"] = g;
    res["b"] = b;
  }
  else
  {
    res["error"] = "sensor_unavailable";
  }

  publishCommandResult(res);
}

void StandaloneRunService::handleWriteRfid(const String &correlationId, const String &uid, const String &data)
{
  if (uid.length() == 0 || data.length() == 0)
  {
    JsonDocument res;
    res["correlationId"] = correlationId;
    res["ok"] = false;
    res["error"] = "bad_args";
    publishCommandResult(res);
    return;
  }

  // TODO: write data to the RFID tag matching uid
  // Example stub:
  //   bool ok = rfid.writeTag(uid, data);

  bool ok = false;

  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = ok;
  if (!ok)
    res["error"] = "write_failed";

  publishCommandResult(res);
}

void StandaloneRunService::handleOta(const String &correlationId, const String &url)
{
  {
    JsonDocument ack;
    ack["correlationId"] = correlationId;
    ack["ok"] = (url.length() > 0);
    ack["status"] = "running";
    if (url.length() == 0)
      ack["error"] = "missing_url";
    String out;
    serializeJson(ack, out);
    _mqtt.publish(topicOf("command/ack").c_str(), out.c_str());
  }

  if (url.length() == 0)
    return;

  auto r = _ota.runGateway(url, nullptr); // gateway path: WiFi already up, no ESPNOW to tear down

  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = (r == OtaService::Result::Ok);
  res["status"] = "failed";
  res["errorCode"] = (int)r;
  String out;
  serializeJson(res, out);
  _mqtt.publish(topicOf("command/result").c_str(), out.c_str());
}
