#include "NetUtils.h"

#include <WiFi.h>

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

} // namespace netutils
