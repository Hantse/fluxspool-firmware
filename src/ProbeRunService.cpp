#include "ProbeRunService.h"
#include "PnowProtocol.h"
#include "NetUtils.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#ifndef FW_VERSION
#define FW_VERSION "0.0.0"
#endif

ProbeRunService *ProbeRunService::_self = nullptr;

static String macNoSep()
{
  String m = WiFi.macAddress();
  m.replace(":", "");
  m.toLowerCase();
  return m;
}

ProbeRunService::ProbeRunService(PreferenceService &prefs, const Config &cfg)
    : _prefs(prefs), _cfg(cfg), _ota(_prefs)
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
    if (millis() - _lastHeartbeatMs > 5000)
    {
      _lastHeartbeatMs = millis();
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
  return netutils::wifiConnectSTA(_prefs, timeoutMs);
}

bool ProbeRunService::ensureTimeSynced(uint32_t timeoutMs)
{
  return netutils::ensureTimeSynced(timeoutMs);
}

bool ProbeRunService::tokenValidSoon() const
{
  uint64_t exp = _prefs.getAccessExpUnix();
  if (exp == 0)
    return false;
  uint64_t now = netutils::nowUnix();
  if (!netutils::timeIsValid(now))
    return true; // time not synced yet — trust stored exp, don't refresh blindly
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

  String devKey = deviceKey();
  if (devKey.length() == 0)
  {
    Serial.println("[PROBE] deviceKey missing");
    return false;
  }

  String url = String(_cfg.apiBase) + "/api/device/refreshtoken";
  JsonDocument bodyDoc;
  bodyDoc["refreshToken"] = refresh;
  bodyDoc["deviceId"] = devKey;
  String body;
  serializeJson(bodyDoc, body);

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

  const uint64_t exp = netutils::nowUnix() + (uint64_t)expiresIn;

  return _prefs.updateAuthTokens(String(access), String(refresh2), exp);
}

bool ProbeRunService::registerProbe()
{
  String access = _prefs.getAccessToken();
  String devKey = deviceKey();
  if (access.length() == 0 || devKey.length() == 0)
    return false;

  String url = String(_cfg.apiBase) + "/api/device/register/probe";

  // build request
  JsonDocument doc;
  doc["probeId"] = probeId();
  doc["deviceId"] = devKey;
  doc["mac"] = WiFi.macAddress();
  doc["chipId"] = String((uint32_t)ESP.getEfuseMac(), HEX);
  doc["firmwareVersion"] = firmwareVersion();
  doc["wifiSsid"] = _prefs.loadWifi().ssid;
  doc["model"] = model();

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  String ca = _prefs.loadCaCertPem();
  if (ca.length() == 0)
    return false;
  client.setCACert(ca.c_str());

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
  String ca = _prefs.loadCaCertPem();
  if (ca.length() == 0)
  {
    Serial.println("[PROBE] No CA stored");
    return false;
  }
  client.setCACert(ca.c_str());
  client.setTimeout(15000);

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

  memcpy(_gatewayMac, peer.mac, 6);
  _gatewayMacCached = true;

  // disconnect WiFi but keep STA mode
  WiFi.disconnect(true, true);
  delay(100);

  return _link.begin(peer, &ProbeRunService::onRxStatic);
}

String ProbeRunService::probeId() const
{
  return String("probe-") + macNoSep();
}

String ProbeRunService::deviceKey() const
{
  return _prefs.getDeviceKey();
}

String ProbeRunService::model() const
{
  return "FluxSpool-Probe";
}

String ProbeRunService::firmwareVersion() const
{
  return FW_VERSION;
}

void ProbeRunService::onRxStatic(const uint8_t *mac, const uint8_t *data, int len)
{
  if (_self)
    _self->onRx(mac, data, len);
}

static bool macEquals6(const uint8_t *a, const uint8_t *b)
{
  return memcmp(a, b, 6) == 0;
}

void ProbeRunService::sendAck(uint32_t seq, bool ok, uint8_t err, uint32_t arg)
{
  uint8_t buf[sizeof(pnow::Header) + sizeof(pnow::AckPayload)];
  pnow::Header h{};
  h.v = pnow::PN_VERSION;
  h.type = pnow::RSP_ACK;
  h.len = sizeof(pnow::AckPayload);
  h.seq = seq;
  h.ts = 0;

  pnow::AckPayload p{};
  p.ok = ok ? 1 : 0;
  p.err = err;
  p.arg = arg;

  memcpy(buf, &h, sizeof(h));
  memcpy(buf + sizeof(h), &p, sizeof(p));

  // write CRC
  pnow::Header *ph = (pnow::Header *)buf;
  ph->crc32 = pnow::compute_crc(*ph, buf + sizeof(pnow::Header));

  _link.send(buf, sizeof(buf));
}

void ProbeRunService::onRx(const uint8_t *mac, const uint8_t *data, int len)
{
  // ---- 0) Filter: accept only gateway MAC (cached at ESPNOW init) ----
  if (!_gatewayMacCached)
    return;
  if (!macEquals6(mac, _gatewayMac))
  {
    return;
  }

  // ---- 1) Validate header + CRC ----
  pnow::Header h{};
  const uint8_t *payload = nullptr;

  // pnow::validate_basic returns false for version/len/crc errors,
  // but we want to ACK error (if possible).
  bool okBasic = pnow::validate_basic(data, len, h, payload);
  if (!okBasic)
  {
    // best-effort parse header to respond; if even header isn't there, drop
    if (len >= (int)sizeof(pnow::Header))
    {
      memcpy(&h, data, sizeof(pnow::Header));
      uint8_t err = pnow::ERR_BAD_CRC;
      if (h.v != pnow::PN_VERSION)
        err = pnow::ERR_BAD_VERSION;
      else if (h.len > pnow::PN_MAX_PAYLOAD)
        err = pnow::ERR_BAD_LEN;
      sendAck(h.seq, false, err, 0);
    }
    return;
  }

  // ---- 2) Anti-replay (seq must increase) ----
  if (h.seq <= _lastSeqSeen)
  {
    sendAck(h.seq, false, pnow::ERR_REPLAY, _lastSeqSeen);
    return;
  }

  // ---- 3) Rate limit (except STATUS) ----
  uint32_t nowMs = millis();
  if (h.type != pnow::CMD_STATUS)
  {
    if ((uint32_t)(nowMs - _lastCmdAtMs) < 200)
    {
      sendAck(h.seq, false, pnow::ERR_RATE_LIMIT, 0);
      return;
    }
  }
  _lastCmdAtMs = nowMs;
  _lastSeqSeen = h.seq;

  // ---- 4) Dispatch ----
  switch ((pnow::MsgType)h.type)
  {
  case pnow::CMD_STATUS:
  {
    sendAck(h.seq, true, pnow::ERR_OK, 0);
    Serial.printf("[PNOW] STATUS seq=%lu\n", (unsigned long)h.seq);
    break;
  }

  case pnow::CMD_REBOOT:
  {
    sendAck(h.seq, true, pnow::ERR_OK, 0);
    Serial.println("[PNOW] REBOOT");
    delay(200);
    ESP.restart();
    break;
  }

  case pnow::CMD_RESET:
  {
    if (h.len < sizeof(pnow::ResetPayload))
    {
      sendAck(h.seq, false, pnow::ERR_BAD_LEN, 0);
      break;
    }

    pnow::ResetPayload rp{};
    memcpy(&rp, payload, sizeof(rp));

    uint32_t t = millis();
    if (!_resetArmed || t > _resetArmedUntilMs || _resetNonce != rp.nonce)
    {
      // Arm
      _resetArmed = true;
      _resetNonce = rp.nonce;
      _resetArmedUntilMs = t + 8000; // 8s window
      sendAck(h.seq, true, pnow::ERR_OK, rp.nonce);
      Serial.printf("[PNOW] RESET armed nonce=%lu\n", (unsigned long)rp.nonce);
      break;
    }

    // Confirm (same nonce within window)
    sendAck(h.seq, true, pnow::ERR_OK, rp.nonce);
    Serial.println("[PNOW] RESET confirmed -> clear prefs + reboot");

    _prefs.clearAll(); // <-- implement / or call your typed "factoryReset"
    delay(200);
    ESP.restart();
    break;
  }

  case pnow::CMD_TARE:
  {
    sendAck(h.seq, true, pnow::ERR_OK, 0);
    Serial.println("[PNOW] TARE");

    // TODO: call your tare routine here
    // e.g. _scale.tare();

    break;
  }

  case pnow::CMD_TELEMETRY:
  {
    sendAck(h.seq, true, pnow::ERR_OK, 0);
    Serial.println("[PNOW] TELEMETRY requested");

    // TODO: read sensors (weight + RFID) and send a RSP_TELEMETRY packet
    // (If payload might be big, keep it <= PN_MAX_PAYLOAD and/or chunk.)

    break;
  }

  case pnow::CMD_WRITE:
  {
    sendAck(h.seq, true, pnow::ERR_OK, 0);
    Serial.println("[PNOW] WRITE");

    // TODO: implement write routine here

    break;
  }

  case pnow::CMD_OTA:
  {
    Serial.println("[PNOW] OTA");
    // ACK immediately: "command in progress"
    sendAck(h.seq, true, pnow::ERR_OK, 0);

    // payload = URL as bytes (not necessarily null-terminated)
    if (h.len == 0 || h.len > pnow::PN_MAX_PAYLOAD)
    {
      Serial.println("[PNOW] OTA missing url payload");
      break;
    }

    String url;
    url.reserve(h.len + 1);
    for (uint16_t i = 0; i < h.len; i++)
      url += (char)payload[i];

    Serial.println("[PNOW] OTA start");
    handleOtaCommand(url);

    break;
  }

  default:
    sendAck(h.seq, false, pnow::ERR_NOT_SUPPORTED, 0);
    break;
  }
}

void ProbeRunService::handleOtaCommand(const String &url)
{
  Serial.print("[PNOW][OTA] url=");
  Serial.println(url);

  // Stop ESPNOW link before WiFi connect/HTTP
  _link.end();
  delay(50);

  auto r = _ota.runProbe(url, nullptr);

  if (r == OtaService::Result::Ok)
  {
    // reboot already triggered
    return;
  }

  Serial.print("[PNOW][OTA] failed code=");
  Serial.println((int)r);

  // Re-init ESPNOW link from cached config (ensureEspNow handles WiFi disconnect internally)
  ensureEspNow();
}