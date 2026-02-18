#include "ProbeNowLink.h"
#include <mbedtls/base64.h>

ProbeNowLink *ProbeNowLink::_self = nullptr;

static bool isHexChar(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int hexVal(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + (c - 'A');
  return 0;
}

bool ProbeNowLink::parseMac(const String &sIn, uint8_t out[6])
{
  String s = sIn;
  s.replace(":", "");
  s.replace("-", "");
  if (s.length() != 12)
    return false;
  for (int i = 0; i < 12; i++)
    if (!isHexChar(s[i]))
      return false;
  for (int i = 0; i < 6; i++)
    out[i] = (hexVal(s[i * 2]) << 4) | hexVal(s[i * 2 + 1]);
  return true;
}

bool ProbeNowLink::decodeKey16(const String &sIn, uint8_t out[16])
{
  String s = sIn;
  s.trim();
  if (s.length() == 32)
  {
    for (int i = 0; i < 32; i++)
      if (!isHexChar(s[i]))
        return false;
    for (int i = 0; i < 16; i++)
      out[i] = (hexVal(s[i * 2]) << 4) | hexVal(s[i * 2 + 1]);
    return true;
  }
  size_t olen = 0;
  int rc = mbedtls_base64_decode(out, 16, &olen, (const unsigned char *)s.c_str(), s.length());
  return (rc == 0 && olen == 16);
}

bool ProbeNowLink::begin(const PeerConfig &peer, RxHandler onRx)
{
  if (_ready)
    return true;
  _peer = peer;
  _rx = onRx;

  // ESPNOW requires STA or AP_STA
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_OFF)
    WiFi.mode(WIFI_STA);
  else if (mode == WIFI_AP)
    WiFi.mode(WIFI_AP_STA);
  else if (mode != WIFI_STA && mode != WIFI_AP_STA)
    WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK)
  {
    Serial.println("[PNOW] esp_now_init failed");
    return false;
  }

  esp_now_register_recv_cb(&ProbeNowLink::recvStatic);
  _self = this;

  esp_now_peer_info_t pi{};
  memcpy(pi.peer_addr, _peer.mac, 6);
  pi.channel = 0;
  pi.ifidx = WIFI_IF_STA;
  if (_peer.hasLmk)
  {
    pi.encrypt = true;
    memcpy(pi.lmk, _peer.lmk, 16);
  }
  else
  {
    pi.encrypt = false;
  }

  esp_now_del_peer(pi.peer_addr);
  if (esp_now_add_peer(&pi) != ESP_OK)
  {
    Serial.println("[PNOW] add peer failed");
    esp_now_deinit();
    _self = nullptr;
    return false;
  }

  _ready = true;
  Serial.println("[PNOW] ready");
  return true;
}

void ProbeNowLink::end()
{
  if (!_ready)
    return;
  esp_now_deinit();
  _ready = false;
  _self = nullptr;
}

bool ProbeNowLink::send(const uint8_t *data, size_t len)
{
  if (!_ready)
    return false;
  return esp_now_send(_peer.mac, data, len) == ESP_OK;
}

void ProbeNowLink::recvStatic(const uint8_t *mac, const uint8_t *data, int len)
{
  if (!_self || !_self->_rx)
    return;
  _self->_rx(mac, data, len);
}
