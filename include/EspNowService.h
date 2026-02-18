#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <functional>

// EspNowService
// - Maintains a peer table (from topology/result: mac + lmk + deviceKey)
// - Supports ONE in-flight request at a time (sequential polling)
// - Provides a FIFO queue for requests arriving while one is in-flight
// - Request/response matched by correlationId (16 bytes)
// - Optional LMK encryption per peer (AES-128 as per ESP-NOW)

class EspNowService
{
public:
  static constexpr uint8_t MAX_PEERS = 64;
  static constexpr uint8_t MAX_QUEUE = 8;

  struct Peer
  {
    uint8_t mac[6]{};
    uint8_t lmk[16]{};
    bool hasLmk = false;
    String deviceKey;
  };

  struct TelemetryResponse
  {
    bool ok = false;
    int32_t weight = 0;
    uint16_t variance = 0;
    uint32_t tagAtMs = 0;
    uint32_t weightAtMs = 0;
    String uid;
  };

  using TelemetryCallback = std::function<void(const TelemetryResponse &)>;

  EspNowService();

  bool begin();
  void loop();

  void upsertPeer(const Peer &p);
  uint8_t peerCount() const { return _peerCount; }

  // correlationIdHex must be 32 hex chars.
  bool requestTelemetryByMac(const uint8_t mac[6],
                             const String &correlationIdHex,
                             TelemetryCallback cb,
                             uint32_t timeoutMs = 1200,
                             uint8_t retries = 1);

  static bool parseMac(const String &s, uint8_t out[6]);
  static bool hexTo16(const String &hex, uint8_t out[16]);

private:
  struct QueueItem
  {
    bool used = false;
    uint8_t mac[6]{};
    uint8_t corr[16]{};
    TelemetryCallback cb;
    uint32_t timeoutMs = 1200;
    uint8_t retries = 1;
  };

  struct Pending
  {
    bool active = false;
    uint8_t mac[6]{};
    uint8_t corr[16]{};
    TelemetryCallback cb;
    uint32_t timeoutMs = 1200;
    uint32_t deadlineMs = 0;
    uint8_t retriesLeft = 1;
  };

  static void recvStatic(const uint8_t *mac, const uint8_t *data, int len);
  void onRecv(const uint8_t *mac, const uint8_t *data, int len);

  void processQueue();
  void failPending();
  bool sendReq(const uint8_t mac[6]);

  bool addPeerIfNeeded(const uint8_t mac[6]);
  bool findPeer(const uint8_t mac[6], Peer &out);

private:
  Peer _peers[MAX_PEERS];
  uint8_t _peerCount = 0;

  QueueItem _queue[MAX_QUEUE];
  Pending _pending;

  static EspNowService *_self;
};
