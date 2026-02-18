#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// MqttService: owns WiFiClientSecure + PubSubClient and manages ALL MQTT concerns:
// - TLS (CA)
// - connect/reconnect with simple backoff
// - publish/subscribe/unsubscribe
// - EXACT topic routing (no suffix match)
// - auto re-subscribe after reconnect
//
// Note: PubSubClient doesn't support userdata in callbacks, so this service assumes
// a single instance (singleton bridge) which matches FluxSpool firmware usage.

class MqttService
{
public:
  using RawHandler = void (*)(char *topic, byte *payload, unsigned int length);

  MqttService(const char *host, uint16_t port);

  // Configure TLS + client settings (call once in setup)
  void begin(const char *caPem,
             uint16_t keepAliveSec,
             uint16_t socketTimeoutSec,
             uint16_t bufferSize);

  // Connect using clientId/username/password
  bool connect(const char *clientId, const char *username, const char *password);

  bool connected();
  int state();

  // Must be called frequently (or from a task) to keep connection alive
  void loop();

  void disconnect();

  // Publishing
  bool publish(const char *topic, const char *payload, bool retained = false);
  bool publish(const char *topic, const uint8_t *payload, size_t length, bool retained = false);

  bool publish(const String &topic, const String &payload, bool retained = false)
  {
    return publish(topic.c_str(), payload.c_str(), retained);
  }

  // Subscriptions (EXACT routing)
  // - subscribe(topic,qos) will subscribe without a handler (will go to default handler if set)
  // - subscribe(topic,qos,handler) registers an EXACT handler for that topic
  bool subscribe(const char *topic, uint8_t qos = 1);
  bool subscribe(const char *topic, uint8_t qos, RawHandler handler);
  bool unsubscribe(const char *topic);

  bool subscribe(const String &topic, uint8_t qos = 1) { return subscribe(topic.c_str(), qos); }
  bool subscribe(const String &topic, uint8_t qos, RawHandler handler) { return subscribe(topic.c_str(), qos, handler); }
  bool unsubscribe(const String &topic) { return unsubscribe(topic.c_str()); }

  // Default handler receives messages for topics without a specific handler
  void setDefaultHandler(RawHandler handler);

  // Clears routing table (does not unsubscribe)
  void clearHandlers();

private:
  struct SubEntry
  {
    bool used = false;
    String topic;
    uint8_t qos = 1;
    RawHandler handler = nullptr;
  };

  bool _subscribeWire(const char *topic, uint8_t qos);
  void _resubscribeAll();

  static void _staticCallback(char *topic, byte *payload, unsigned int length);
  void _onMessage(char *topic, byte *payload, unsigned int length);

private:
  const char *_host;
  uint16_t _port;

  WiFiClientSecure _net;
  PubSubClient _mqtt;

  RawHandler _defaultHandler = nullptr;

  static MqttService *_self; // singleton bridge for PubSubClient callback

  static const uint8_t MAX_SUBS = 16;
  SubEntry _subs[MAX_SUBS];

  // reconnect backoff managed by caller in current firmware; we just help resubscribe on connect
  bool _wasConnected = false;
};
