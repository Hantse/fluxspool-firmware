#include "NetUtils.h"

#include <WiFi.h>
#include <ArduinoJson.h>

namespace netutils
{

bool wifiConnectSTA(PreferenceService &prefs, uint32_t timeoutMs)
{
  auto w = prefs.loadWifi();
  if (w.ssid.length() == 0)
    return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(w.ssid.c_str(), w.password.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs)
    delay(200);

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("[WiFi] Connected IP=");
    Serial.println(WiFi.localIP());
    return true;
  }
  return false;
}

bool ensureTimeSynced(uint32_t timeoutMs)
{
  time_t now;
  time(&now);
  if (now > 1700000000)
    return true;

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs)
  {
    time(&now);
    if (now > 1700000000)
      return true;
    delay(250);
  }
  return false;
}

bool storeTokenResponse(PreferenceService &prefs, const String &respJson, bool isProvisioning)
{
  JsonDocument doc;
  if (deserializeJson(doc, respJson))
  {
    Serial.println("[TOKEN] JSON parse error");
    return false;
  }

  JsonVariant root = doc;
  if (doc["data"].is<JsonObject>())
    root = doc["data"];

  const char *access = root["accessToken"];
  const char *refresh = root["refreshToken"];

  long expiresIn = 0;
  if (root["expiresIn"].is<long>())
    expiresIn = root["expiresIn"].as<long>();
  else if (root["expiresIn"].is<int>())
    expiresIn = (long)root["expiresIn"].as<int>();
  else if (root["expiresIn"].is<const char *>())
    expiresIn = atol(root["expiresIn"].as<const char *>());

  if (!access || !refresh || expiresIn <= 0)
  {
    Serial.println("[TOKEN] Payload invalid (missing access/refresh/expiresIn)");
    return false;
  }

  const uint64_t exp = nowUnix() + (uint64_t)expiresIn;

  if (isProvisioning)
  {
    const char *devId = root["deviceId"];
    if (devId && *devId)
      prefs.setDeviceKey(String(devId));
    return prefs.updateAuthTokensChecked(String(access), String(refresh), exp);
  }

  return prefs.updateAuthTokens(String(access), String(refresh), exp);
}

} // namespace netutils
