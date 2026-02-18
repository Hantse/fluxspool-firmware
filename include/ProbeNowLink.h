#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

// Minimal ESPNOW link for Probe -> Gateway (single peer)
class ProbeNowLink
{
public:
  struct PeerConfig
  {
    uint8_t mac[6]{};
    uint8_t lmk[16]{};
    bool hasLmk = false;
  };

  using RxHandler = void (*)(const uint8_t *mac, const uint8_t *data, int len);

  bool begin(const PeerConfig &peer, RxHandler onRx);
  void end();
  bool isReady() const { return _ready; }

  bool send(const uint8_t *data, size_t len);

  static bool parseMac(const String &s, uint8_t out[6]);
  // accept 32 hex chars or base64(16 bytes)
  static bool decodeKey16(const String &s, uint8_t out[16]);

private:
  bool _ready = false;
  PeerConfig _peer{};
  RxHandler _rx = nullptr;

  static void recvStatic(const uint8_t *mac, const uint8_t *data, int len);
  static ProbeNowLink *_self;
};
