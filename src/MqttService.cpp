#include "MqttService.h"

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
    Serial.println("[MQTT] ERROR: No CA configured");
    return; // ou fail dur
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
