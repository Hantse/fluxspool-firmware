#include "EspNowService.h"

EspNowService* EspNowService::_self = nullptr;

#pragma pack(push, 1)
struct TelemetryReq {
  uint8_t type; // 1
  uint8_t corr[16];
};

struct TelemetryResp {
  uint8_t type; // 2
  uint8_t corr[16];
  uint8_t ok;
  int32_t weight;
  uint16_t variance;
  uint32_t tagAtMs;
  uint32_t weightAtMs;
  char uid[16];
};
#pragma pack(pop)

EspNowService::EspNowService() {}

bool EspNowService::begin() {
  // ESPNOW needs STA or AP_STA (never WIFI_OFF)
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_OFF) WiFi.mode(WIFI_STA);
  else if (mode == WIFI_AP) WiFi.mode(WIFI_AP_STA);

  if (esp_now_init() != ESP_OK) return false;

  _self = this;
  esp_now_register_recv_cb(&EspNowService::recvStatic);
  return true;
}

void EspNowService::loop() {
  if (_pending.active && (int32_t)(millis() - _pending.deadlineMs) > 0) {
    if (_pending.retriesLeft > 0) {
      _pending.retriesLeft--;
      sendReq(_pending.mac);
      _pending.deadlineMs = millis() + _pending.timeoutMs;
    } else {
      failPending();
    }
  }

  if (!_pending.active) processQueue();
}

void EspNowService::upsertPeer(const Peer& p) {
  for (uint8_t i = 0; i < _peerCount; i++) {
    if (memcmp(_peers[i].mac, p.mac, 6) == 0) {
      _peers[i] = p;
      return;
    }
  }
  if (_peerCount < MAX_PEERS) {
    _peers[_peerCount++] = p;
  }
}

bool EspNowService::requestTelemetryByMac(const uint8_t mac[6],
                                         const String& correlationIdHex,
                                         TelemetryCallback cb,
                                         uint32_t timeoutMs,
                                         uint8_t retries) {
  for (uint8_t i = 0; i < MAX_QUEUE; i++) {
    if (!_queue[i].used) {
      _queue[i].used = true;
      memcpy(_queue[i].mac, mac, 6);
      if (!hexTo16(correlationIdHex, _queue[i].corr)) {
        _queue[i].used = false;
        return false;
      }
      _queue[i].cb = cb;
      _queue[i].timeoutMs = timeoutMs;
      _queue[i].retries = retries;
      return true;
    }
  }
  return false;
}

void EspNowService::processQueue() {
  for (uint8_t i = 0; i < MAX_QUEUE; i++) {
    if (_queue[i].used) {
      memcpy(_pending.mac, _queue[i].mac, 6);
      memcpy(_pending.corr, _queue[i].corr, 16);
      _pending.cb = _queue[i].cb;
      _pending.timeoutMs = _queue[i].timeoutMs;
      _pending.retriesLeft = _queue[i].retries;
      _pending.deadlineMs = millis() + _pending.timeoutMs;
      _pending.active = true;

      _queue[i].used = false;

      sendReq(_pending.mac);
      return;
    }
  }
}

bool EspNowService::sendReq(const uint8_t mac[6]) {
  if (!addPeerIfNeeded(mac)) return false;

  TelemetryReq req{};
  req.type = 1;
  memcpy(req.corr, _pending.corr, 16);

  return esp_now_send(mac, (uint8_t*)&req, sizeof(req)) == ESP_OK;
}

void EspNowService::failPending() {
  if (!_pending.active) return;
  TelemetryResponse r{};
  r.ok = false;
  if (_pending.cb) _pending.cb(r);
  _pending.active = false;
}

void EspNowService::recvStatic(const uint8_t* mac, const uint8_t* data, int len) {
  if (_self) _self->onRecv(mac, data, len);
}
void EspNowService::onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (!_pending.active) return;
  if (memcmp(mac, _pending.mac, 6) != 0) return;
  if (len < (int)sizeof(TelemetryResp)) return;

  const auto* resp = (const TelemetryResp*)data;
  if (resp->type != 2) return;
  if (memcmp(resp->corr, _pending.corr, 16) != 0) return;

  TelemetryResponse r{};
  r.ok = resp->ok != 0;
  r.weight = resp->weight;
  r.variance = resp->variance;
  r.tagAtMs = resp->tagAtMs;
  r.weightAtMs = resp->weightAtMs;
  r.uid = String(resp->uid);

  if (_pending.cb) _pending.cb(r);
  _pending.active = false;
}

bool EspNowService::findPeer(const uint8_t mac[6], Peer& out) {
  for (uint8_t i = 0; i < _peerCount; i++) {
    if (memcmp(_peers[i].mac, mac, 6) == 0) {
      out = _peers[i];
      return true;
    }
  }
  return false;
}

bool EspNowService::addPeerIfNeeded(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return true;

  Peer p{};
  bool has = findPeer(mac, p);

  esp_now_peer_info_t info{};
  memcpy(info.peer_addr, mac, 6);
  info.channel = 0; // current channel
  info.encrypt = false;

  if (has && p.hasLmk) {
    info.encrypt = true;
    memcpy(info.lmk, p.lmk, 16);
  }

  return esp_now_add_peer(&info) == ESP_OK;
}

bool EspNowService::parseMac(const String& s, uint8_t out[6]) {
  int v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}

bool EspNowService::hexTo16(const String& hex, uint8_t out[16]) {
  if (hex.length() != 32) return false;
  for (int i = 0; i < 16; i++) {
    out[i] = (uint8_t)strtoul(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
  }
  return true;
}
