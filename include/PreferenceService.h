#pragma once

#include <Arduino.h>
#include <Preferences.h>

// Centralizes ALL NVS storage access for the device.
// NOTE: Preferences/NVS is plaintext unless NVS encryption/flash encryption is enabled.
class PreferenceService
{
public:
  struct WifiConfig
  {
    String ssid;
    String password;
  };

  struct MqttConfig
  {
    String host;
    uint16_t port = 8883;
    String username;
    String password;
    String clientId;
  };

  struct AuthConfig
  {
    String deviceKey;
    String accessToken;
    String refreshToken;
    uint64_t accessExpUnix = 0;
  };

  struct ProvisioningCodes
  {
    String code1; // pairing
    String code2; // confirm
  };

  struct ProbeNowConfig
  {
    String gatewayMac;  // "AA:BB:CC:DD:EE:FF"
    String lmk;         // base64 OR 32 hex chars
    String gatewayHmac; // server-provided identity for gateway
  };

  struct BootFlags
  {
    bool setupDone = false;
    bool hasCert = false;
    bool hasWifi = false;
    bool hasAuth = false;
    bool hasMqtt = false;
  };

  // Provisioning session codes (Setup mode)
  bool hasProvisioningCodes() const;
  ProvisioningCodes loadProvisioningCodes() const;
  bool saveProvisioningCodes(const ProvisioningCodes &c);
  bool clearProvisioningCodes();

  // Convenience typed getters (avoid raw keys outside this class)
  String getDeviceKey() const;
  bool setDeviceKey(const String &v);
  String getAccessToken() const;
  String getRefreshToken() const;
  uint64_t getAccessExpUnix() const;

  // Update only token fields; keeps current deviceKey
  bool updateAuthTokens(const String &accessToken, const String &refreshToken, uint64_t accessExpUnix);
  bool updateAuthTokensChecked(const String &accessToken, const String &refreshToken, uint64_t accessExpUnix);

  explicit PreferenceService(const char *nvsNamespace = "fluxspool");

  bool begin(bool readOnly = false);
  void end();
  bool isReady() const { return _started; }

  bool clearAll();

  // Generic
  String getString(const char *key, const String &def = "") const;
  bool setString(const char *key, const String &value);
  // Write then read back to verify (useful for critical secrets)
  bool setStringChecked(const char *key, const String &value);

  uint32_t getUInt(const char *key, uint32_t def = 0) const;
  bool setUInt(const char *key, uint32_t value);

  int32_t getInt(const char *key, int32_t def = 0) const;
  bool setInt(const char *key, int32_t value);

  bool getBool(const char *key, bool def = false) const;
  bool setBool(const char *key, bool value);

  uint64_t getU64(const char *key, uint64_t def = 0) const;
  bool setU64(const char *key, uint64_t value);

  size_t getBytes(const char *key, void *outBuf, size_t maxLen) const;
  bool setBytes(const char *key, const void *buf, size_t len);

  bool removeKey(const char *key);

  // Boot flags
  BootFlags getBootFlags() const;
  bool getSetupDone() const;
  bool setSetupDone(bool v);

  // WiFi
  bool hasWifi() const;
  WifiConfig loadWifi() const;
  bool saveWifi(const WifiConfig &cfg);
  bool clearWifi();

  // MQTT
  bool hasMqtt() const;
  MqttConfig loadMqtt() const;
  bool saveMqtt(const MqttConfig &cfg);
  bool clearMqtt();

  // Auth
  bool hasAuth() const;
  AuthConfig loadAuth() const;
  bool saveAuth(const AuthConfig &cfg);
  bool clearAuth();

  // TLS CA (PEM)
  bool hasCaCert() const;
  String loadCaCertPem() const;
  bool saveCaCertPem(const String &pem);
  bool clearCaCertPem();

  // Topology JSON (raw)
  String loadTopologyJson() const;
  bool saveTopologyJson(const String &json);
  bool clearTopologyJson();

  // Debug
  void dumpToSerial(bool includeSecrets = false) const;

  // Probe
  bool hasProbeNowConfig() const;
  ProbeNowConfig loadProbeNowConfig() const;
  bool saveProbeNowConfig(const ProbeNowConfig &cfg);
  bool clearProbeNowConfig();

private:
  // Keys (keep short)
  static constexpr const char *K_SETUP_DONE = "setup_done";

  static constexpr const char *K_WIFI_SSID = "wifi_ssid";
  static constexpr const char *K_WIFI_PASS = "wifi_pass";

  static constexpr const char *K_MQTT_HOST = "mq_host";
  static constexpr const char *K_MQTT_PORT = "mq_port";
  static constexpr const char *K_MQTT_USER = "mq_user";
  static constexpr const char *K_MQTT_PASS = "mq_pass";
  static constexpr const char *K_MQTT_CID = "mq_cid";

  static constexpr const char *K_AUTH_DKEY = "auth_dkey";
  static constexpr const char *K_AUTH_AT = "auth_at";
  static constexpr const char *K_AUTH_RT = "auth_rt";
  static constexpr const char *K_AUTH_AT_EXP = "auth_at_exp";

  static constexpr const char *K_CA_PEM = "ca_pem";
  static constexpr const char *K_TOPOLOGY_JSON = "topology_json";

  // Setup/provisioning session codes
  static constexpr const char *K_PROV_CODE1 = "pairing";
  static constexpr const char *K_PROV_CODE2 = "confirm";

  // Probe
  static constexpr const char *K_PNOW_GWMAC = "pnow_gwmac";
  static constexpr const char *K_PNOW_LMK = "pnow_lmk";
  static constexpr const char *K_PNOW_GWHMAC = "pnow_gwhmac";

private:
  const char *_ns;
  mutable Preferences _prefs;
  bool _started = false;

  static String maskSecret(const String &s, int keep = 4);
};
