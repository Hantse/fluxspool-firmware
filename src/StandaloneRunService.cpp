#include "StandaloneRunService.h"
#include "NetUtils.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <SPI.h>
#include <time.h>
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>
#ifndef FW_VERSION
#define FW_VERSION "0.0.0"
#endif

StandaloneRunService *StandaloneRunService::_self = nullptr;

static const char *resolvedFirmwareVersion()
{
  const char *version = FW_VERSION;
  return version[0] == '\0' ? "0.0.0" : version;
}

static bool isSoftRebootCommand(const String &cmd)
{
  return cmd == "reboot" || cmd == "Reboot" || cmd == "reset" || cmd == "Reset";
}

static bool readFloatField(JsonDocument &doc, const char *key, float &out)
{
  JsonVariant value = doc[key];
  if (value.isNull())
    return false;

  if (value.is<float>() || value.is<double>() || value.is<int>() || value.is<long>())
  {
    out = value.as<float>();
    return true;
  }

  if (value.is<const char *>())
  {
    String s = String(value.as<const char *>());
    s.trim();
    if (s.length() == 0)
      return false;
    out = s.toFloat();
    return true;
  }

  return false;
}

static uint8_t readSamplesField(JsonDocument &doc, uint8_t fallback)
{
  int samples = fallback;
  JsonVariant value = doc["samples"];
  if (value.is<int>())
    samples = value.as<int>();
  else if (value.is<const char *>())
    samples = String(value.as<const char *>()).toInt();

  if (samples < 1)
    samples = 1;
  if (samples > 50)
    samples = 50;
  return (uint8_t)samples;
}

StandaloneRunService::StandaloneRunService(PreferenceService &prefs, MqttService &mqtt, const Config &cfg)
    : _prefs(prefs), _mqtt(mqtt), _cfg(cfg), _ota(_prefs)
{
  _self = this;
}

void StandaloneRunService::begin()
{
  _running = true;
  _registerConfirmed = false;
  loadScaleCalibration();

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
  pollRfidCache();

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
  const bool ok = _mqtt.publish(t.c_str(), out.c_str());

  Serial.print("[COMMAND] publish result -> ");
  Serial.print(t);
  Serial.print(" len=");
  Serial.print(out.length());
  Serial.print(" ");
  Serial.println(ok ? "OK" : "FAIL");

  if (!ok)
  {
    Serial.print("[COMMAND] result payload=");
    Serial.println(out);
  }
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
  doc["firmwareVersion"] = resolvedFirmwareVersion();
  doc["macAddress"] = WiFi.macAddress();
  doc["wifiSsid"] = WiFi.SSID();
  doc["scaleTared"] = _scaleTared;
  doc["scaleCalibrated"] = _scaleCalibrated;

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
  doc["firmwareVersion"] = resolvedFirmwareVersion();
  doc["scaleTared"] = _scaleTared;
  doc["scaleCalibrated"] = _scaleCalibrated;

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
  if (_hasLastWeight)
  {
    doc["weight_g"] = _lastWeightG;
    doc["weightRaw"] = _lastWeightRaw;
    doc["weightAtMs"] = _lastWeightAtMs;
  }
  if (_lastUidTelemetryAllowed && _lastUid.length() > 0)
  {
    doc["uid"] = _lastUid;
    doc["uidAtMs"] = _lastUidAtMs;
    if (_lastRfidData.length() > 0)
    {
      doc["rfidData"] = _lastRfidData;
      doc["rfidDataAtMs"] = _lastRfidDataAtMs;
    }
  }
  if (_hasLastColor)
  {
    JsonObject color = doc["color"].to<JsonObject>();
    color["sensor"] = _lastColor.sensor;
    color["r"] = _lastColor.r;
    color["g"] = _lastColor.g;
    color["b"] = _lastColor.b;
    color["c"] = _lastColor.c;
    color["atMs"] = _lastColorAtMs;
  }

  String payload;
  serializeJson(doc, payload);

  const String t = topicOf("telemetry");
  _mqtt.publish(t.c_str(), payload.c_str());
}

// -------------------- Hardware helpers --------------------

void StandaloneRunService::loadScaleCalibration()
{
  _scaleTared = _prefs.hasScaleTare();
  const bool hasPersistedCalibration = _prefs.hasScaleCalibration();
  PreferenceService::ScaleCalibration calibration =
      _prefs.loadScaleCalibration(_cfg.hx711Scale, _cfg.hx711Offset);

  _cfg.hx711Scale = calibration.scale;
  _cfg.hx711Offset = calibration.offset;
  _scaleCalibrated = hasPersistedCalibration && calibration.calibrated && fabs(_cfg.hx711Scale) >= 0.0001f;

  if (_scaleCalibrated)
  {
    Serial.printf("[SCALE] calibration loaded scale=%.6f offset=%ld\n", _cfg.hx711Scale, _cfg.hx711Offset);
  }
  else
  {
    Serial.println("[SCALE] not calibrated yet");
  }
}

void StandaloneRunService::applyScaleCalibration()
{
  if (!_scaleStarted)
    return;

  _scale.set_scale(_cfg.hx711Scale);
  _scale.set_offset(_cfg.hx711Offset);
}

void StandaloneRunService::ensureI2cStarted()
{
  if (_i2cStarted)
    return;

  Wire.begin(fluxspool::hw::I2C_SDA_PIN, fluxspool::hw::I2C_SCL_PIN);
  Wire.setClock(400000);
  _i2cStarted = true;
}

bool StandaloneRunService::ensureScaleStarted()
{
  if (_scaleStarted)
    return true;

  _scale.begin(fluxspool::hw::HX711_DOUT_PIN, fluxspool::hw::HX711_SCK_PIN, _cfg.hx711Gain);
  _scaleStarted = true;
  applyScaleCalibration();
  return true;
}

bool StandaloneRunService::ensureAs7341Started()
{
  if (_as7341Started)
    return true;

  ensureI2cStarted();
  if (!_as7341.begin(AS7341_I2CADDR_DEFAULT, &Wire))
    return false;

  _as7341.setATIME(100);
  _as7341.setASTEP(999);
  _as7341.setGain(AS7341_GAIN_256X);
  _as7341Started = true;
  return true;
}

bool StandaloneRunService::ensureGy33Started()
{
  if (_gy33Started)
    return true;

  ensureI2cStarted();
  _gy33.begin(fluxspool::hw::I2C_SDA_PIN, fluxspool::hw::I2C_SCL_PIN);
  _gy33Started = true;
  return true;
}

bool StandaloneRunService::ensureTcs34725Started()
{
  if (_tcs34725Started)
    return true;

  ensureI2cStarted();
  if (!_tcs34725.begin(TCS34725_ADDRESS, &Wire))
    return false;

  _tcs34725Started = true;
  return true;
}

bool StandaloneRunService::ensureRfidStarted()
{
  if (_rfidStarted)
    return true;

  SPI.begin(fluxspool::hw::MFRC522_SCK_PIN,
            fluxspool::hw::MFRC522_MISO_PIN,
            fluxspool::hw::MFRC522_MOSI_PIN,
            fluxspool::hw::MFRC522_SS_PIN);
  pinMode(fluxspool::hw::MFRC522_SS_PIN, OUTPUT);
  digitalWrite(fluxspool::hw::MFRC522_SS_PIN, HIGH);

  _rfid.PCD_Init(fluxspool::hw::MFRC522_SS_PIN, fluxspool::hw::MFRC522_RST_PIN);
  _rfid.PCD_AntennaOn();
  _rfidStarted = true;
  return true;
}

bool StandaloneRunService::readWeight(float &weightG, long &raw, String &error, uint8_t samples)
{
  if (!readScaleRaw(raw, error, samples))
    return false;

  if (!_scaleCalibrated)
  {
    error = "scale_not_calibrated";
    return false;
  }

  if (fabs(_cfg.hx711Scale) < 0.0001f)
  {
    error = "scale_not_calibrated";
    return false;
  }

  weightG = ((float)(raw - _cfg.hx711Offset)) / _cfg.hx711Scale;

  _lastWeightG = weightG;
  _lastWeightRaw = raw;
  _lastWeightAtMs = millis();
  _hasLastWeight = true;
  return true;
}

bool StandaloneRunService::readScaleRaw(long &raw, String &error, uint8_t samples)
{
  if (!ensureScaleStarted())
  {
    error = "sensor_unavailable";
    return false;
  }

  if (!_scale.wait_ready_timeout(_cfg.hx711ReadyTimeoutMs, 10))
  {
    error = "sensor_timeout";
    return false;
  }

  const uint8_t sampleCount = samples == 0 ? _cfg.hx711Samples : samples;
  raw = _scale.read_average(sampleCount);
  return true;
}

bool StandaloneRunService::saveScaleCalibration(float scale, long offset, bool calibrated)
{
  _cfg.hx711Scale = scale;
  _cfg.hx711Offset = offset;
  _scaleTared = true;
  _scaleCalibrated = calibrated && fabs(scale) >= 0.0001f;
  applyScaleCalibration();

  PreferenceService::ScaleCalibration calibration;
  calibration.scale = _cfg.hx711Scale;
  calibration.offset = _cfg.hx711Offset;
  calibration.calibrated = _scaleCalibrated;
  return _prefs.saveScaleCalibration(calibration);
}

bool StandaloneRunService::readWeightWithRetries(float &weightG, long &raw, String &error, uint8_t &attempts, uint8_t samples)
{
  const uint8_t maxAttempts = _cfg.sensorReadRetries == 0 ? 1 : _cfg.sensorReadRetries;
  attempts = 0;

  for (uint8_t i = 0; i < maxAttempts; i++)
  {
    attempts = i + 1;
    error = "";
    Serial.print("[SCALE] weight attempt=");
    Serial.print(attempts);
    Serial.print("/");
    Serial.print(maxAttempts);
    Serial.print(" samples=");
    Serial.println(samples);

    if (readWeight(weightG, raw, error, samples))
    {
      Serial.print("[SCALE] weight OK raw=");
      Serial.print(raw);
      Serial.print(" offset=");
      Serial.print(_cfg.hx711Offset);
      Serial.print(" scale=");
      Serial.print(_cfg.hx711Scale);
      Serial.print(" weight_g=");
      Serial.println(weightG);
      return true;
    }

    Serial.print("[SCALE] weight FAIL raw=");
    Serial.print(raw);
    Serial.print(" calibrated=");
    Serial.print(_scaleCalibrated ? "true" : "false");
    Serial.print(" error=");
    Serial.println(error);

    if (i + 1 < maxAttempts)
    {
      delay(_cfg.sensorRetryDelayMs);
      yield();
    }
  }

  if (error.length() == 0)
    error = "sensor_unavailable";
  return false;
}

bool StandaloneRunService::readAs7341Color(ColorReading &reading)
{
  if (!ensureAs7341Started())
    return false;

  if (!_as7341.readAllChannels())
    return false;

  reading = {};
  reading.sensor = "as7341";
  reading.f1 = _as7341.getChannel(AS7341_CHANNEL_415nm_F1);
  reading.f2 = _as7341.getChannel(AS7341_CHANNEL_445nm_F2);
  reading.f3 = _as7341.getChannel(AS7341_CHANNEL_480nm_F3);
  reading.f4 = _as7341.getChannel(AS7341_CHANNEL_515nm_F4);
  reading.f5 = _as7341.getChannel(AS7341_CHANNEL_555nm_F5);
  reading.f6 = _as7341.getChannel(AS7341_CHANNEL_590nm_F6);
  reading.f7 = _as7341.getChannel(AS7341_CHANNEL_630nm_F7);
  reading.f8 = _as7341.getChannel(AS7341_CHANNEL_680nm_F8);
  reading.c = _as7341.getChannel(AS7341_CHANNEL_CLEAR);
  reading.nir = _as7341.getChannel(AS7341_CHANNEL_NIR);

  reading.r = reading.f7;
  reading.g = reading.f5;
  reading.b = reading.f3;
  return true;
}

bool StandaloneRunService::readGy33Color(ColorReading &reading)
{
  if (!ensureGy33Started())
    return false;

  if (!_gy33.update())
    return false;

  const GY33_Raw raw = _gy33.getRaw();
  const GY33_Processed processed = _gy33.getProcessed();
  const GY33_LCC lcc = _gy33.getLCC();

  reading = {};
  reading.sensor = "gy33";
  reading.r = raw.r;
  reading.g = raw.g;
  reading.b = raw.b;
  reading.c = raw.c;
  reading.lux = lcc.lux;
  reading.colorTemp = lcc.colourTemp;
  reading.colorIndex = lcc.colourIndex;
  reading.colorName = _gy33.colour();
  reading.f1 = processed.r;
  reading.f2 = processed.g;
  reading.f3 = processed.b;
  return true;
}

bool StandaloneRunService::readTcs34725Color(ColorReading &reading)
{
  if (!ensureTcs34725Started())
    return false;

  reading = {};
  reading.sensor = "tcs34725";
  _tcs34725.getRawData(&reading.r, &reading.g, &reading.b, &reading.c);
  reading.lux = _tcs34725.calculateLux(reading.r, reading.g, reading.b);
  reading.colorTemp = _tcs34725.calculateColorTemperature_dn40(reading.r, reading.g, reading.b, reading.c);
  return true;
}

bool StandaloneRunService::readColor(ColorReading &reading, String &error)
{
  if (readAs7341Color(reading) || readGy33Color(reading) || readTcs34725Color(reading))
  {
    _lastColor = reading;
    _lastColorAtMs = millis();
    _hasLastColor = true;
    return true;
  }

  error = "sensor_unavailable";
  return false;
}

bool StandaloneRunService::readColorWithRetries(ColorReading &reading, String &error, uint8_t &attempts)
{
  const uint8_t maxAttempts = _cfg.sensorReadRetries == 0 ? 1 : _cfg.sensorReadRetries;
  attempts = 0;

  for (uint8_t i = 0; i < maxAttempts; i++)
  {
    attempts = i + 1;
    error = "";
    if (readColor(reading, error))
      return true;

    if (i + 1 < maxAttempts)
    {
      delay(_cfg.sensorRetryDelayMs);
      yield();
    }
  }

  if (error.length() == 0)
    error = "sensor_unavailable";
  return false;
}

void StandaloneRunService::addColorJson(JsonObject target, const ColorReading &color) const
{
  target["sensor"] = color.sensor;
  target["r"] = color.r;
  target["g"] = color.g;
  target["b"] = color.b;
  target["c"] = color.c;
  if (strcmp(color.sensor, "as7341") == 0)
  {
    target["f1_415"] = color.f1;
    target["f2_445"] = color.f2;
    target["f3_480"] = color.f3;
    target["f4_515"] = color.f4;
    target["f5_555"] = color.f5;
    target["f6_590"] = color.f6;
    target["f7_630"] = color.f7;
    target["f8_680"] = color.f8;
    target["nir"] = color.nir;
  }
  else if (strcmp(color.sensor, "gy33") == 0)
  {
    target["processedR"] = color.f1;
    target["processedG"] = color.f2;
    target["processedB"] = color.f3;
    target["lux"] = color.lux;
    target["colorTemp"] = color.colorTemp;
    target["colorIndex"] = color.colorIndex;
    target["colorName"] = color.colorName;
  }
  else
  {
    target["lux"] = color.lux;
    target["colorTemp"] = color.colorTemp;
  }
}

String StandaloneRunService::currentRfidUid() const
{
  String out;
  for (byte i = 0; i < _rfid.uid.size; i++)
  {
    if (_rfid.uid.uidByte[i] < 0x10)
      out += '0';
    out += String(_rfid.uid.uidByte[i], HEX);
  }
  out.toUpperCase();
  return out;
}

String StandaloneRunService::normalizeUid(const String &uid) const
{
  String out;
  out.reserve(uid.length());
  for (uint16_t i = 0; i < uid.length(); i++)
  {
    const char c = uid.charAt(i);
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
      out += c;
  }
  out.toUpperCase();
  return out;
}

String StandaloneRunService::rfidStatusName(MFRC522::StatusCode status) const
{
  return String(MFRC522::GetStatusCodeName(status));
}

uint8_t StandaloneRunService::rfidTrailerBlock(uint8_t blockAddr) const
{
  return blockAddr + (3 - (blockAddr % 4));
}

void StandaloneRunService::rememberRfidUid(const String &uid, bool allowTelemetry)
{
  if (uid.length() == 0)
    return;

  if (_lastUid != uid)
  {
    Serial.print("[RFID] cached UID ");
    Serial.println(uid);
  }

  _lastUid = uid;
  _lastUidAtMs = millis();
  _lastUidTelemetryAllowed = allowTelemetry;
}

void StandaloneRunService::rememberRfidData(const String &data)
{
  _lastRfidData = data;
  _lastRfidDataAtMs = millis();
}

bool StandaloneRunService::cachedRfidUid(String &uid, uint32_t &ageMs) const
{
  if (_lastUid.length() == 0)
    return false;

  ageMs = (uint32_t)(millis() - _lastUidAtMs);
  if (_cfg.rfidCacheTtlMs > 0 && ageMs > _cfg.rfidCacheTtlMs)
    return false;

  uid = _lastUid;
  return true;
}

bool StandaloneRunService::readSelectedRfidData(String &data, String &error, uint8_t blockAddr)
{
  data = "";

  if ((blockAddr % 4) == 3)
  {
    error = "trailer_block_forbidden";
    return false;
  }

  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < MFRC522::MF_KEY_SIZE; i++)
    key.keyByte[i] = 0xFF;

  const uint8_t trailerBlock = rfidTrailerBlock(blockAddr);
  MFRC522::StatusCode status = _rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailerBlock, &key, &(_rfid.uid));
  if (status != MFRC522::STATUS_OK)
  {
    error = "auth_failed:" + rfidStatusName(status);
    _rfid.PCD_StopCrypto1();
    return false;
  }

  byte buffer[18] = {};
  byte size = sizeof(buffer);
  status = _rfid.MIFARE_Read(blockAddr, buffer, &size);
  _rfid.PCD_StopCrypto1();

  if (status != MFRC522::STATUS_OK)
  {
    error = "read_failed:" + rfidStatusName(status);
    return false;
  }

  for (byte i = 0; i < 16; i++)
  {
    if (buffer[i] == 0)
      break;
    data += (char)buffer[i];
  }

  rememberRfidData(data);
  return data.length() > 0;
}

void StandaloneRunService::publishRfidDetected(const String &uid, const String &data, const String &dataError)
{
  if (!_rfidDetectMode || !_mqtt.connected())
    return;

  const uint32_t nowMs = millis();
  const String signature = uid + "|" + data;
  if (signature == _lastRfidDetectedSignature &&
      (uint32_t)(nowMs - _lastRfidDetectedPublishMs) < _cfg.rfidDetectedPublishCooldownMs)
  {
    return;
  }

  JsonDocument doc;
  doc["event"] = "detected";
  doc["uid"] = uid;
  doc["uidAtMs"] = _lastUidAtMs;
  if (data.length() > 0)
  {
    doc["rfidData"] = data;
    doc["rfidDataAtMs"] = _lastRfidDataAtMs;
    doc["dataBlock"] = _cfg.rfidDataBlock;
  }
  else if (dataError.length() > 0)
  {
    doc["dataError"] = dataError;
  }

  String out;
  serializeJson(doc, out);
  const String t = topicOf("rfid");
  const bool ok = _mqtt.publish(t.c_str(), out.c_str());

  Serial.print("[RFID] publish detected -> ");
  Serial.print(t);
  Serial.print(" len=");
  Serial.print(out.length());
  Serial.print(" ");
  Serial.println(ok ? "OK" : "FAIL");

  if (!ok)
  {
    Serial.print("[RFID] detected payload=");
    Serial.println(out);
  }

  _lastRfidDetectedSignature = signature;
  _lastRfidDetectedPublishMs = nowMs;
}

void StandaloneRunService::pollRfidCache()
{
  if (_cfg.rfidPassivePollEveryMs == 0)
    return;

  const uint32_t nowMs = millis();
  if ((uint32_t)(nowMs - _lastRfidPassivePollMs) < _cfg.rfidPassivePollEveryMs)
    return;
  _lastRfidPassivePollMs = nowMs;

  if (!ensureRfidStarted())
    return;

  if (_rfid.PICC_IsNewCardPresent() && _rfid.PICC_ReadCardSerial())
  {
    const String uid = currentRfidUid();
    String data;
    String dataError;
    readSelectedRfidData(data, dataError, _cfg.rfidDataBlock);
    rememberRfidUid(uid, false);
    publishRfidDetected(uid, data, dataError);
    _rfid.PICC_HaltA();
  }
}

bool StandaloneRunService::selectRfidCard(String &uid, String &error, uint32_t timeoutMs)
{
  if (!ensureRfidStarted())
  {
    error = "sensor_unavailable";
    return false;
  }

  const uint32_t startMs = millis();
  do
  {
    if (_rfid.PICC_IsNewCardPresent() && _rfid.PICC_ReadCardSerial())
    {
      uid = currentRfidUid();
      String data;
      String dataError;
      readSelectedRfidData(data, dataError, _cfg.rfidDataBlock);
      rememberRfidUid(uid, true);
      return true;
    }

    delay(20);
    yield();
  } while ((uint32_t)(millis() - startMs) < timeoutMs);

  error = "no_tag";
  return false;
}

bool StandaloneRunService::readRfidUid(String &uid, String &error, uint32_t timeoutMs)
{
  if (!selectRfidCard(uid, error, timeoutMs))
    return false;

  _rfid.PICC_HaltA();
  return true;
}

bool StandaloneRunService::readRfidUidWithRetries(String &uid, String &error, uint8_t &attempts, bool &fromCache, uint32_t &cacheAgeMs)
{
  const uint8_t maxAttempts = _cfg.sensorReadRetries == 0 ? 1 : _cfg.sensorReadRetries;
  attempts = 0;
  fromCache = false;
  cacheAgeMs = 0;

  for (uint8_t i = 0; i < maxAttempts; i++)
  {
    attempts = i + 1;
    error = "";
    if (readRfidUid(uid, error, _cfg.rfidScanTimeoutMs))
      return true;

    if (i + 1 < maxAttempts)
    {
      delay(_cfg.sensorRetryDelayMs);
      yield();
    }
  }

  if (cachedRfidUid(uid, cacheAgeMs))
  {
    fromCache = true;
    error = "";
    return true;
  }

  if (error.length() == 0)
    error = "no_tag";
  return false;
}

bool StandaloneRunService::writeRfidBlock(const String &expectedUid, const String &data, uint8_t blockAddr, String &actualUid, String &error)
{
  const String expected = normalizeUid(expectedUid);
  if (expected.length() == 0 || data.length() == 0)
  {
    error = "bad_args";
    return false;
  }

  if (data.length() > 16)
  {
    error = "data_too_long";
    return false;
  }

  if ((blockAddr % 4) == 3)
  {
    error = "trailer_block_forbidden";
    return false;
  }

  if (!selectRfidCard(actualUid, error, _cfg.rfidScanTimeoutMs))
    return false;

  if (normalizeUid(actualUid) != expected)
  {
    error = "uid_mismatch";
    _rfid.PICC_HaltA();
    return false;
  }

  const MFRC522::PICC_Type piccType = _rfid.PICC_GetType(_rfid.uid.sak);
  if (piccType != MFRC522::PICC_TYPE_MIFARE_MINI &&
      piccType != MFRC522::PICC_TYPE_MIFARE_1K &&
      piccType != MFRC522::PICC_TYPE_MIFARE_4K)
  {
    error = "unsupported_tag";
    _rfid.PICC_HaltA();
    return false;
  }

  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < MFRC522::MF_KEY_SIZE; i++)
    key.keyByte[i] = 0xFF;

  const uint8_t trailerBlock = rfidTrailerBlock(blockAddr);
  MFRC522::StatusCode status = _rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailerBlock, &key, &(_rfid.uid));
  if (status != MFRC522::STATUS_OK)
  {
    error = "auth_failed:" + rfidStatusName(status);
    _rfid.PICC_HaltA();
    _rfid.PCD_StopCrypto1();
    return false;
  }

  byte buffer[16] = {};
  for (uint16_t i = 0; i < data.length(); i++)
    buffer[i] = (byte)data.charAt(i);

  status = _rfid.MIFARE_Write(blockAddr, buffer, sizeof(buffer));
  if (status != MFRC522::STATUS_OK)
  {
    error = "write_failed:" + rfidStatusName(status);
    _rfid.PICC_HaltA();
    _rfid.PCD_StopCrypto1();
    return false;
  }

  byte verify[18] = {};
  byte verifySize = sizeof(verify);
  status = _rfid.MIFARE_Read(blockAddr, verify, &verifySize);
  if (status != MFRC522::STATUS_OK)
  {
    error = "verify_failed:" + rfidStatusName(status);
    _rfid.PICC_HaltA();
    _rfid.PCD_StopCrypto1();
    return false;
  }

  for (byte i = 0; i < sizeof(buffer); i++)
  {
    if (verify[i] != buffer[i])
    {
      error = "verify_mismatch";
      _rfid.PICC_HaltA();
      _rfid.PCD_StopCrypto1();
      return false;
    }
  }

  _rfid.PICC_HaltA();
  _rfid.PCD_StopCrypto1();
  rememberRfidData(data);
  return true;
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
    handleWeight(correlationId, readSamplesField(doc, _cfg.hx711Samples));
  }
  else if (cmd == "ScaleTare" || cmd == "Tare")
  {
    handleScaleTare(correlationId, readSamplesField(doc, _cfg.hx711Samples));
  }
  else if (cmd == "ScaleCalibrate")
  {
    float referenceWeightG = 0.0f;
    if (!readFloatField(doc, "referenceWeightG", referenceWeightG) &&
        !readFloatField(doc, "weightG", referenceWeightG) &&
        !readFloatField(doc, "weight_g", referenceWeightG) &&
        !readFloatField(doc, "knownWeightG", referenceWeightG) &&
        !readFloatField(doc, "grams", referenceWeightG))
    {
      referenceWeightG = 0.0f;
    }
    handleScaleCalibrate(correlationId, referenceWeightG, readSamplesField(doc, _cfg.hx711Samples));
  }
  else if (cmd == "ScaleStatus")
  {
    handleScaleStatus(correlationId);
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
  else if (cmd == "RfidDetect")
  {
    const bool enabled = doc["enabled"].is<bool>() ? doc["enabled"].as<bool>() : true;
    handleRfidDetectMode(correlationId, enabled);
  }
  else if (cmd == "RfidDetectStart")
  {
    handleRfidDetectMode(correlationId, true);
  }
  else if (cmd == "RfidDetectStop")
  {
    handleRfidDetectMode(correlationId, false);
  }
  else if (isSoftRebootCommand(cmd))
  {
    JsonDocument res;
    res["correlationId"] = correlationId;
    res["ok"] = true;
    res["status"] = "rebooting";
    publishCommandResult(res);

    Serial.println("[COMMAND] Soft reboot requested");
    _mqtt.loop();
    delay(250);
    Serial.flush();
    ESP.restart();
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
  String uid;
  String rfidError;
  uint8_t rfidAttempts = 0;
  bool rfidFromCache = false;
  uint32_t rfidCacheAgeMs = 0;
  const bool rfidOk = readRfidUidWithRetries(uid, rfidError, rfidAttempts, rfidFromCache, rfidCacheAgeMs);

  float weightG = 0.0f;
  long weightRaw = 0;
  String weightError;
  uint8_t weightAttempts = 0;
  const bool weightOk = readWeightWithRetries(weightG, weightRaw, weightError, weightAttempts, _cfg.hx711Samples);

  ColorReading color;
  String colorError;
  uint8_t colorAttempts = 0;
  const bool colorOk = readColorWithRetries(color, colorError, colorAttempts);

  const bool ok = rfidOk && weightOk && colorOk;

  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = ok;
  if (!ok)
    res["partial"] = rfidOk || weightOk || colorOk;

  JsonObject attempts = res["attempts"].to<JsonObject>();
  attempts["rfid"] = rfidAttempts;
  attempts["weight"] = weightAttempts;
  attempts["color"] = colorAttempts;

  if (rfidOk)
  {
    res["uid"] = uid;
    res["uidAtMs"] = _lastUidAtMs;
    res["uidSource"] = rfidFromCache ? "cache" : "reader";
    if (_lastRfidData.length() > 0)
    {
      res["rfidData"] = _lastRfidData;
      res["rfidDataAtMs"] = _lastRfidDataAtMs;
      res["dataBlock"] = _cfg.rfidDataBlock;
    }
    if (rfidFromCache)
      res["uidAgeMs"] = rfidCacheAgeMs;
  }

  if (weightOk)
  {
    res["weight_g"] = weightG;
    res["weightRaw"] = weightRaw;
    res["weightAtMs"] = _lastWeightAtMs;
  }

  if (colorOk)
  {
    JsonObject colorObj = res["color"].to<JsonObject>();
    addColorJson(colorObj, color);
    colorObj["colorAtMs"] = _lastColorAtMs;
  }

  if (!ok)
  {
    JsonArray missing = res["missing"].to<JsonArray>();
    JsonObject errors = res["errors"].to<JsonObject>();
    if (!rfidOk)
    {
      missing.add("rfid");
      errors["rfid"] = rfidError;
    }
    if (!weightOk)
    {
      missing.add("weight");
      errors["weight"] = weightError;
    }
    if (!colorOk)
    {
      missing.add("color");
      errors["color"] = colorError;
    }
  }

  publishCommandResult(res);
}

void StandaloneRunService::handleWeight(const String &correlationId, uint8_t samples)
{
  Serial.print("[SCALE] weight requested samples=");
  Serial.println(samples);

  String error;
  float weight_g = 0.0f;
  long raw = 0;
  uint8_t attempts = 0;
  const bool ok = readWeightWithRetries(weight_g, raw, error, attempts, samples);

  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = ok;
  res["attempts"] = attempts;
  if (ok)
  {
    res["weight_g"] = weight_g;
    res["weightRaw"] = raw;
    res["weightAtMs"] = _lastWeightAtMs;
  }
  else
  {
    res["error"] = error;
  }

  Serial.print("[SCALE] weight result ");
  Serial.print(ok ? "OK" : "FAIL");
  Serial.print(" attempts=");
  Serial.print(attempts);
  Serial.print(" raw=");
  Serial.print(raw);
  if (ok)
  {
    Serial.print(" weight_g=");
    Serial.print(weight_g);
  }
  else
  {
    Serial.print(" error=");
    Serial.print(error);
  }
  Serial.println();

  publishCommandResult(res);
}

void StandaloneRunService::handleScaleTare(const String &correlationId, uint8_t samples)
{
  Serial.print("[SCALE] tare requested samples=");
  Serial.println(samples);

  String error;
  long raw = 0;
  const long previousOffset = _cfg.hx711Offset;
  const bool rawOk = readScaleRaw(raw, error, samples);

  bool saved = false;
  if (rawOk)
  {
    saved = saveScaleCalibration(_cfg.hx711Scale, raw, _scaleCalibrated);
    if (!saved)
      error = "nvs_save_failed";
  }

  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = rawOk && saved;
  res["offset"] = raw;
  res["scale"] = _cfg.hx711Scale;
  res["tared"] = rawOk && saved;
  res["calibrated"] = _scaleCalibrated;
  res["samples"] = samples;
  if (!(rawOk && saved))
    res["error"] = error;

  Serial.print("[SCALE] tare ");
  Serial.print(rawOk && saved ? "OK" : "FAIL");
  Serial.print(" oldOffset=");
  Serial.print(previousOffset);
  Serial.print(" raw=");
  Serial.print(raw);
  Serial.print(" newOffset=");
  Serial.print(_cfg.hx711Offset);
  Serial.print(" scale=");
  Serial.print(_cfg.hx711Scale);
  Serial.print(" calibrated=");
  Serial.print(_scaleCalibrated ? "true" : "false");
  Serial.print(" tared=");
  Serial.print(_scaleTared ? "true" : "false");
  Serial.print(" saved=");
  Serial.print(saved ? "true" : "false");
  if (!(rawOk && saved))
  {
    Serial.print(" error=");
    Serial.print(error);
  }
  Serial.println();

  publishCommandResult(res);
}

void StandaloneRunService::handleScaleCalibrate(const String &correlationId, float referenceWeightG, uint8_t samples)
{
  JsonDocument res;
  res["correlationId"] = correlationId;

  if (referenceWeightG <= 0.0f)
  {
    res["ok"] = false;
    res["error"] = "bad_reference_weight";
    publishCommandResult(res);
    return;
  }

  String error;
  long raw = 0;
  if (!readScaleRaw(raw, error, samples))
  {
    res["ok"] = false;
    res["error"] = error;
    publishCommandResult(res);
    return;
  }

  const long delta = raw - _cfg.hx711Offset;
  if (labs(delta) < 100)
  {
    res["ok"] = false;
    res["error"] = "reference_weight_not_detected";
    res["raw"] = raw;
    res["offset"] = _cfg.hx711Offset;
    res["delta"] = delta;
    publishCommandResult(res);
    return;
  }

  const float scale = ((float)delta) / referenceWeightG;
  const bool saved = saveScaleCalibration(scale, _cfg.hx711Offset, true);

  res["ok"] = saved;
  res["referenceWeightG"] = referenceWeightG;
  res["raw"] = raw;
  res["offset"] = _cfg.hx711Offset;
  res["delta"] = delta;
  res["scale"] = _cfg.hx711Scale;
  res["tared"] = _scaleTared;
  res["calibrated"] = _scaleCalibrated;
  res["samples"] = samples;
  if (!saved)
    res["error"] = "nvs_save_failed";

  publishCommandResult(res);
}

void StandaloneRunService::handleScaleStatus(const String &correlationId)
{
  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = true;
  res["scale"] = _cfg.hx711Scale;
  res["offset"] = _cfg.hx711Offset;
  res["tared"] = _scaleTared;
  res["calibrated"] = _scaleCalibrated;
  res["samples"] = _cfg.hx711Samples;
  publishCommandResult(res);
}

void StandaloneRunService::handleColor(const String &correlationId)
{
  String error;
  ColorReading color;
  uint8_t attempts = 0;
  const bool ok = readColorWithRetries(color, error, attempts);

  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = ok;
  res["attempts"] = attempts;
  if (ok)
  {
    JsonObject root = res.as<JsonObject>();
    addColorJson(root, color);
    res["colorAtMs"] = _lastColorAtMs;
  }
  else
  {
    res["error"] = error;
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

  String actualUid;
  String error;
  const bool ok = writeRfidBlock(uid, data, _cfg.rfidDataBlock, actualUid, error);

  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = ok;
  if (ok)
  {
    res["uid"] = actualUid;
    res["block"] = _cfg.rfidDataBlock;
    res["bytesWritten"] = data.length();
  }
  else
  {
    res["error"] = error;
    if (actualUid.length() > 0)
      res["uid"] = actualUid;
  }

  publishCommandResult(res);
}

void StandaloneRunService::handleRfidDetectMode(const String &correlationId, bool enabled)
{
  _rfidDetectMode = enabled;
  if (!enabled)
  {
    _lastRfidDetectedSignature = "";
    _lastRfidDetectedPublishMs = 0;
  }

  JsonDocument res;
  res["correlationId"] = correlationId;
  res["ok"] = true;
  res["rfidDetectMode"] = _rfidDetectMode;
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
  publishCommandResult(res);
}
