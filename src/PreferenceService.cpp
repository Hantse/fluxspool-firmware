#include "PreferenceService.h"

PreferenceService::PreferenceService(const char *nvsNamespace)
    : _ns(nvsNamespace) {}

bool PreferenceService::begin(bool readOnly)
{
  if (_started)
    return true;
  _started = _prefs.begin(_ns, readOnly);
  return _started;
}

void PreferenceService::end()
{
  if (!_started)
    return;
  _prefs.end();
  _started = false;
}

bool PreferenceService::clearAll()
{
  if (!_started)
    return false;
  return _prefs.clear();
}

// ---------------- Generic helpers ----------------

String PreferenceService::getString(const char *key, const String &def) const
{
  if (!_started)
    return def;

  // IMPORTANT: évite le spam NOT_FOUND
  if (!_prefs.isKey(key))
    return def;

  return _prefs.getString(key, def);
}

bool PreferenceService::setString(const char *key, const String &value)
{
  if (!_started)
    return false;
  return _prefs.putString(key, value) > 0 || value.length() == 0;
}

bool PreferenceService::setStringChecked(const char *key, const String &value)
{
  if (!_started)
    return false;

  bool ok = setString(key, value);
  if (!ok && value.length() > 0)
    return false;

  // Verify round-trip (best effort)
  String back = getString(key, "");
  return back == value;
}

uint32_t PreferenceService::getUInt(const char *key, uint32_t def) const
{
  if (!_started)
    return def;
  return _prefs.getUInt(key, def);
}

bool PreferenceService::setUInt(const char *key, uint32_t value)
{
  if (!_started)
    return false;
  _prefs.putUInt(key, value);
  return true;
}

int32_t PreferenceService::getInt(const char *key, int32_t def) const
{
  if (!_started)
    return def;
  return _prefs.getInt(key, def);
}

bool PreferenceService::setInt(const char *key, int32_t value)
{
  if (!_started)
    return false;
  _prefs.putInt(key, value);
  return true;
}

bool PreferenceService::getBool(const char *key, bool def) const
{
  if (!_started)
    return def;
  return _prefs.getBool(key, def);
}

bool PreferenceService::setBool(const char *key, bool value)
{
  if (!_started)
    return false;
  _prefs.putBool(key, value);
  return true;
}

uint64_t PreferenceService::getU64(const char *key, uint64_t def) const
{
  if (!_started)
    return def;
  return _prefs.getULong64(key, def);
}

bool PreferenceService::setU64(const char *key, uint64_t value)
{
  if (!_started)
    return false;
  _prefs.putULong64(key, value);
  return true;
}

size_t PreferenceService::getBytes(const char *key, void *outBuf, size_t maxLen) const
{
  if (!_started)
    return 0;
  return _prefs.getBytes(key, outBuf, maxLen);
}

bool PreferenceService::setBytes(const char *key, const void *buf, size_t len)
{
  if (!_started)
    return false;
  return _prefs.putBytes(key, buf, len) == len;
}

bool PreferenceService::removeKey(const char *key)
{
  if (!_started)
    return false;
  return _prefs.remove(key);
}

// ---------------- Boot flags ----------------

PreferenceService::BootFlags PreferenceService::getBootFlags() const
{
  BootFlags f;
  f.setupDone = getSetupDone();
  f.hasWifi = hasWifi();
  f.hasMqtt = hasMqtt();
  f.hasAuth = hasAuth();
  f.hasCert = hasCaCert();
  return f;
}

bool PreferenceService::getSetupDone() const
{
  return getBool(K_SETUP_DONE, false);
}

bool PreferenceService::setSetupDone(bool v)
{
  return setBool(K_SETUP_DONE, v);
}

// ---------------- WiFi ----------------

bool PreferenceService::hasWifi() const
{
  return getString(K_WIFI_SSID, "").length() > 0;
}

PreferenceService::WifiConfig PreferenceService::loadWifi() const
{
  WifiConfig c;
  c.ssid = getString(K_WIFI_SSID, "");
  c.password = getString(K_WIFI_PASS, "");
  return c;
}

bool PreferenceService::saveWifi(const WifiConfig &cfg)
{
  bool ok = true;
  ok &= setString(K_WIFI_SSID, cfg.ssid);
  ok &= setString(K_WIFI_PASS, cfg.password);
  return ok;
}

bool PreferenceService::clearWifi()
{
  bool ok = false;
  ok |= removeKey(K_WIFI_SSID);
  ok |= removeKey(K_WIFI_PASS);
  return ok;
}

// ---------------- MQTT ----------------

bool PreferenceService::hasMqtt() const
{
  String host = getString(K_MQTT_HOST, "");
  uint32_t port = getUInt(K_MQTT_PORT, 0);
  return host.length() > 0 && port > 0;
}

PreferenceService::MqttConfig PreferenceService::loadMqtt() const
{
  MqttConfig c;
  c.host = getString(K_MQTT_HOST, "");
  c.port = (uint16_t)getUInt(K_MQTT_PORT, 8883);
  c.username = getString(K_MQTT_USER, "");
  c.password = getString(K_MQTT_PASS, "");
  c.clientId = getString(K_MQTT_CID, "");
  return c;
}

bool PreferenceService::saveMqtt(const MqttConfig &cfg)
{
  bool ok = true;
  ok &= setString(K_MQTT_HOST, cfg.host);
  ok &= setUInt(K_MQTT_PORT, cfg.port);
  ok &= setString(K_MQTT_USER, cfg.username);
  ok &= setString(K_MQTT_PASS, cfg.password);
  ok &= setString(K_MQTT_CID, cfg.clientId);
  return ok;
}

bool PreferenceService::clearMqtt()
{
  bool ok = false;
  ok |= removeKey(K_MQTT_HOST);
  ok |= removeKey(K_MQTT_PORT);
  ok |= removeKey(K_MQTT_USER);
  ok |= removeKey(K_MQTT_PASS);
  ok |= removeKey(K_MQTT_CID);
  return ok;
}

// ---------------- Auth ----------------

bool PreferenceService::hasAuth() const
{
  return getString(K_AUTH_DKEY, "").length() > 0 || getString(K_AUTH_AT, "").length() > 0;
}

PreferenceService::AuthConfig PreferenceService::loadAuth() const
{
  AuthConfig a;
  a.deviceKey = getString(K_AUTH_DKEY, "");
  a.accessToken = getString(K_AUTH_AT, "");
  a.refreshToken = getString(K_AUTH_RT, "");
  a.accessExpUnix = getU64(K_AUTH_AT_EXP, 0);
  return a;
}

bool PreferenceService::saveAuth(const AuthConfig &cfg)
{
  bool ok = true;
  ok &= setString(K_AUTH_DKEY, cfg.deviceKey);
  ok &= setString(K_AUTH_AT, cfg.accessToken);
  ok &= setString(K_AUTH_RT, cfg.refreshToken);
  ok &= setU64(K_AUTH_AT_EXP, cfg.accessExpUnix);
  return ok;
}

bool PreferenceService::clearAuth()
{
  bool ok = false;
  ok |= removeKey(K_AUTH_DKEY);
  ok |= removeKey(K_AUTH_AT);
  ok |= removeKey(K_AUTH_RT);
  ok |= removeKey(K_AUTH_AT_EXP);
  return ok;
}

// ---------------- Provisioning codes (Setup) ----------------

bool PreferenceService::hasProvisioningCodes() const
{
  if (!_started)
    return false;
  return _prefs.isKey(K_PROV_CODE1) && _prefs.isKey(K_PROV_CODE2);
}

PreferenceService::ProvisioningCodes PreferenceService::loadProvisioningCodes() const
{
  ProvisioningCodes c;
  c.code1 = _prefs.getString(K_PROV_CODE1, "");
  c.code2 = _prefs.getString(K_PROV_CODE2, "");
  return c;
}

bool PreferenceService::saveProvisioningCodes(const ProvisioningCodes &c)
{
  bool ok = true;
  ok &= setString(K_PROV_CODE1, c.code1);
  ok &= setString(K_PROV_CODE2, c.code2);
  return ok;
}

bool PreferenceService::clearProvisioningCodes()
{
  bool ok = false;
  ok |= removeKey(K_PROV_CODE1);
  ok |= removeKey(K_PROV_CODE2);
  return ok;
}

// ---------------- Convenience typed getters ----------------

String PreferenceService::getDeviceKey() const
{
  return getString(K_AUTH_DKEY, "");
}

bool PreferenceService::setDeviceKey(const String &v)
{
  return setString(K_AUTH_DKEY, v);
}

String PreferenceService::getAccessToken() const
{
  return getString(K_AUTH_AT, "");
}

String PreferenceService::getRefreshToken() const
{
  return getString(K_AUTH_RT, "");
}

uint64_t PreferenceService::getAccessExpUnix() const
{
  return getU64(K_AUTH_AT_EXP, 0);
}

bool PreferenceService::updateAuthTokens(const String &accessToken, const String &refreshToken, uint64_t accessExpUnix)
{
  AuthConfig a = loadAuth();
  a.accessToken = accessToken;
  a.refreshToken = refreshToken;
  a.accessExpUnix = accessExpUnix;
  return saveAuth(a);
}

bool PreferenceService::updateAuthTokensChecked(const String &accessToken, const String &refreshToken, uint64_t accessExpUnix)
{
  if (!_started)
    return false;

  // write tokens with round-trip verification (more robust for critical secrets)
  bool ok = true;
  ok &= setStringChecked(K_AUTH_AT, accessToken);
  ok &= setStringChecked(K_AUTH_RT, refreshToken);
  ok &= setU64(K_AUTH_AT_EXP, accessExpUnix);

  if (!ok)
    return false;
  return getString(K_AUTH_AT, "").length() > 0 && getString(K_AUTH_RT, "").length() > 0;
}
// ---------------- CA PEM ----------------

bool PreferenceService::hasCaCert() const
{
  return getString(K_CA_PEM, "").length() > 0;
}

String PreferenceService::loadCaCertPem() const
{
  return getString(K_CA_PEM, "");
}

bool PreferenceService::saveCaCertPem(const String &pem)
{
  return setString(K_CA_PEM, pem);
}

bool PreferenceService::clearCaCertPem()
{
  return removeKey(K_CA_PEM);
}

// ---------------- Topology JSON ----------------

String PreferenceService::loadTopologyJson() const
{
  return getString(K_TOPOLOGY_JSON, "");
}

bool PreferenceService::saveTopologyJson(const String &json)
{
  return setString(K_TOPOLOGY_JSON, json);
}

bool PreferenceService::clearTopologyJson()
{
  return removeKey(K_TOPOLOGY_JSON);
}

// Probe
bool PreferenceService::hasProbeNowConfig() const
{
  return getString(K_PNOW_GWMAC, "").length() > 0 && getString(K_PNOW_LMK, "").length() > 0;
}

PreferenceService::ProbeNowConfig PreferenceService::loadProbeNowConfig() const
{
  ProbeNowConfig c;
  c.gatewayMac = getString(K_PNOW_GWMAC, "");
  c.lmk = getString(K_PNOW_LMK, "");
  c.gatewayHmac = getString(K_PNOW_GWHMAC, "");
  return c;
}

bool PreferenceService::saveProbeNowConfig(const ProbeNowConfig &cfg)
{
  bool ok = true;
  ok &= setString(K_PNOW_GWMAC, cfg.gatewayMac);
  ok &= setString(K_PNOW_LMK, cfg.lmk);
  ok &= setString(K_PNOW_GWHMAC, cfg.gatewayHmac);
  return ok;
}

bool PreferenceService::clearProbeNowConfig()
{
  bool ok = false;
  ok |= removeKey(K_PNOW_GWMAC);
  ok |= removeKey(K_PNOW_LMK);
  ok |= removeKey(K_PNOW_GWHMAC);
  return ok;
}

// ---------------- Debug ----------------

String PreferenceService::maskSecret(const String &s, int keep)
{
  if (s.length() == 0)
    return "";
  if (s.length() <= keep)
    return String("***");
  return String("***") + s.substring(s.length() - keep);
}

void PreferenceService::dumpToSerial(bool includeSecrets) const
{
  if (!_started)
  {
    Serial.println("[PREF] not started");
    return;
  }

  Serial.println("[PREF] ---- dump ----");
  Serial.printf("[PREF] setupDone=%s\n", getSetupDone() ? "true" : "false");

  // WiFi
  String ssid = getString(K_WIFI_SSID, "");
  String pass = getString(K_WIFI_PASS, "");
  Serial.printf("[PREF] wifi.ssid=%s\n", ssid.c_str());
  Serial.printf("[PREF] wifi.pass=%s\n", includeSecrets ? pass.c_str() : maskSecret(pass).c_str());

  // MQTT
  String host = getString(K_MQTT_HOST, "");
  uint32_t port = getUInt(K_MQTT_PORT, 0);
  String mu = getString(K_MQTT_USER, "");
  String mp = getString(K_MQTT_PASS, "");
  String cid = getString(K_MQTT_CID, "");

  Serial.printf("[PREF] mqtt.host=%s\n", host.c_str());
  Serial.printf("[PREF] mqtt.port=%u\n", (unsigned)port);
  Serial.printf("[PREF] mqtt.user=%s\n", mu.c_str());
  Serial.printf("[PREF] mqtt.pass=%s\n", includeSecrets ? mp.c_str() : maskSecret(mp).c_str());
  Serial.printf("[PREF] mqtt.clientId=%s\n", cid.c_str());

  // Auth
  String dk = getString(K_AUTH_DKEY, "");
  String at = getString(K_AUTH_AT, "");
  String rt = getString(K_AUTH_RT, "");
  uint64_t exp = getU64(K_AUTH_AT_EXP, 0);
  Serial.printf("[PREF] auth.deviceKey=%s\n", includeSecrets ? dk.c_str() : maskSecret(dk).c_str());
  Serial.printf("[PREF] auth.accessToken=%s\n", includeSecrets ? at.c_str() : maskSecret(at).c_str());
  Serial.printf("[PREF] auth.refreshToken=%s\n", includeSecrets ? rt.c_str() : maskSecret(rt).c_str());
  Serial.printf("[PREF] auth.accessExp=%llu\n", (unsigned long long)exp);

  // CA
  String ca = getString(K_CA_PEM, "");
  Serial.printf("[PREF] ca.pem.len=%u\n", (unsigned)ca.length());

  // Topology
  String topo = getString(K_TOPOLOGY_JSON, "");
  Serial.printf("[PREF] topology_json.len=%u\n", (unsigned)topo.length());

  Serial.println("[PREF] --------------");
}
