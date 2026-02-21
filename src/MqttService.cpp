#include "MqttService.h"

static const char *LE_CA = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

MqttService *MqttService::_self = nullptr;

MqttService::MqttService(const char *host, uint16_t port)
    : _host(host), _port(port), _net(), _mqtt(_net) {}

void MqttService::begin(const char *caPem,
                        uint16_t keepAliveSec,
                        uint16_t socketTimeoutSec,
                        uint16_t bufferSize)
{
  _self = this;

  if (caPem && strlen(caPem) > 0)
  {
    _net.setCACert(caPem);
  }
  else
  {
    _net.setCACert(LE_CA);
    Serial.println("[MQTT] No CA provided, using default LE root");
  }

  _net.setTimeout(socketTimeoutSec);

  _mqtt.setServer(_host, _port);
  _mqtt.setKeepAlive(keepAliveSec);
  _mqtt.setBufferSize(bufferSize);
  _mqtt.setCallback(MqttService::_staticCallback);
}

bool MqttService::connect(const char *clientId, const char *username, const char *password)
{
  if (!clientId || strlen(clientId) == 0)
    return false;

  bool ok = false;
  if (username && strlen(username) > 0)
  {
    ok = _mqtt.connect(clientId, username, password);
  }
  else
  {
    ok = _mqtt.connect(clientId);
  }

  // When we (re)connect, ensure we have the exact subscriptions back.
  if (ok)
  {
    _resubscribeAll();
  }

  return ok;
}

bool MqttService::connected()
{
  return _mqtt.connected();
}
int MqttService::state()
{
  return _mqtt.state();
}

void MqttService::loop()
{
  bool nowConnected = _mqtt.connected();

  // If we were disconnected and became connected (rare if connect() called elsewhere), resubscribe.
  if (nowConnected && !_wasConnected)
  {
    _resubscribeAll();
  }

  _wasConnected = nowConnected;

  if (nowConnected)
  {
    _mqtt.loop();
  }
}

void MqttService::disconnect()
{
  _mqtt.disconnect();
  _wasConnected = false;
}

bool MqttService::publish(const char *topic, const char *payload, bool retained)
{
  if (!_mqtt.connected())
    return false;
  return _mqtt.publish(topic, payload, retained);
}

bool MqttService::publish(const char *topic, const uint8_t *payload, size_t length, bool retained)
{
  if (!_mqtt.connected())
    return false;
  return _mqtt.publish(topic, payload, length, retained);
}

bool MqttService::_subscribeWire(const char *topic, uint8_t qos)
{
  if (!_mqtt.connected())
    return false;
  return _mqtt.subscribe(topic, qos);
}

bool MqttService::subscribe(const char *topic, uint8_t qos)
{
  return subscribe(topic, qos, nullptr);
}

bool MqttService::subscribe(const char *topic, uint8_t qos, RawHandler handler)
{
  if (!topic || strlen(topic) == 0)
    return false;

  // update existing
  for (auto &e : _subs)
  {
    if (e.used && e.topic == topic)
    {
      e.qos = qos;
      e.handler = handler;
      // subscribe on wire if connected (otherwise will resubscribe on connect)
      if (_mqtt.connected())
        return _subscribeWire(topic, qos);
      return true;
    }
  }

  // add new
  for (auto &e : _subs)
  {
    if (!e.used)
    {
      e.used = true;
      e.topic = topic;
      e.qos = qos;
      e.handler = handler;
      if (_mqtt.connected())
        return _subscribeWire(topic, qos);
      return true;
    }
  }

  return false; // table full
}

bool MqttService::unsubscribe(const char *topic)
{
  if (!topic || strlen(topic) == 0)
    return false;

  // remove from table
  for (auto &e : _subs)
  {
    if (e.used && e.topic == topic)
    {
      e.used = false;
      e.topic = "";
      e.qos = 1;
      e.handler = nullptr;
      break;
    }
  }

  if (!_mqtt.connected())
    return true; // will be gone on next connect anyway
  return _mqtt.unsubscribe(topic);
}

void MqttService::setDefaultHandler(RawHandler handler)
{
  _defaultHandler = handler;
}

void MqttService::clearHandlers()
{
  for (auto &e : _subs)
  {
    e.used = false;
    e.topic = "";
    e.qos = 1;
    e.handler = nullptr;
  }
}

void MqttService::_resubscribeAll()
{
  if (!_mqtt.connected())
    return;

  for (auto &e : _subs)
  {
    if (e.used && e.topic.length() > 0)
    {
      _mqtt.subscribe(e.topic.c_str(), e.qos);
    }
  }
}

void MqttService::_staticCallback(char *topic, byte *payload, unsigned int length)
{
  if (_self)
    _self->_onMessage(topic, payload, length);
}

void MqttService::_onMessage(char *topic, byte *payload, unsigned int length)
{
  // Exact match first
  String t(topic);

  for (auto &e : _subs)
  {
    if (e.used && e.topic == t)
    {
      if (e.handler)
      {
        e.handler(topic, payload, length);
      }
      else if (_defaultHandler)
      {
        _defaultHandler(topic, payload, length);
      }
      return;
    }
  }

  if (_defaultHandler)
  {
    _defaultHandler(topic, payload, length);
  }
}
