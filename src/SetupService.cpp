#include "SetupService.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>

// Keep this CA local to SetupService for HTTPS API provisioning.
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

// Same HTML as v9 (kept in SetupService)
static const char SETUP_PAGE_HTML[] PROGMEM = R"HTML(<!doctype html><html lang=fr><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><meta name=color-scheme content=dark><title>FluxSpool • Add Device</title><style>:root{--bg0:#05080c;--bg1:#0b1119;--text:#d7e2ee;--muted:#8aa2b8;--accent:#18e6d0;--border:rgba(24,230,208,.35);--radius:18px;--shadow:0 30px 80px rgba(0,0,0,.65)}*{box-sizing:border-box}html,body{height:100%}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial;background:radial-gradient(900px 500px at 30% 10%,rgba(24,230,208,.1),transparent 60%),linear-gradient(180deg,var(--bg0),var(--bg1));color:var(--text);display:flex;align-items:center;justify-content:center;padding:20px}.modal{width:min(520px,100%);background:linear-gradient(180deg,#0d1722,#0a121b);border-radius:18px;box-shadow:var(--shadow);border:1px solid rgba(255,255,255,.06)}.header{padding:22px}.title{font-size:18px;font-weight:700}.subtitle{font-size:13px;color:var(--muted);margin-top:4px}.divider{height:1px;background:rgba(255,255,255,.08);margin:0 22px}form{padding:20px 22px 22px;display:flex;flex-direction:column;gap:14px}label{display:block;font-size:12px;color:rgba(255,255,255,.7);margin-bottom:6px}.inputWrap{display:flex;align-items:center;gap:8px;border:2px dashed rgba(24,230,208,.35);border-radius:14px;padding:12px;background:rgba(0,0,0,.15)}.inputWrap:focus-within{border-color:rgba(24,230,208,.7);background:rgba(24,230,208,.05)}input{flex:1;background:none;border:0;outline:none;color:var(--accent);font-size:16px;font-weight:600;letter-spacing:.12em}.normal input{letter-spacing:.02em;color:#e7f2ff}.togglePwd{background:none;border:0;cursor:pointer;font-size:16px;color:rgba(255,255,255,.7);padding:4px}.actions{display:flex;gap:12px;margin-top:18px;padding-top:16px;border-top:1px solid rgba(255,255,255,.06)}.btn{flex:1;padding:12px;border-radius:14px;border:1px solid rgba(24,230,208,.35);background:rgba(24,230,208,.1);color:#eafffb;font-weight:700;cursor:pointer}.btn.secondary{border:1px solid rgba(255,255,255,.12);background:rgba(255,255,255,.04);color:#ddd}</style><body><div class=modal><div class=header><div class=title>Add New Device</div><div class=subtitle>Enter these codes on your FluxSpool device</div></div><div class=divider></div><form method=post action=/setup><div><label>Code 1</label><div class=inputWrap><input name=code1 placeholder=000000></div></div><div><label>Code 2</label><div class=inputWrap><input name=code2 placeholder=000000></div></div><div><label>WiFi</label><div class="inputWrap normal"><input name=wifi placeholder=MyWifi></div></div><div><label>WiFi Password</label><div class="inputWrap normal"><input id=wifiPassword type=password name=wifiPassword placeholder="••••••••"><button type=button class=togglePwd onclick="i=wifiPassword;i.type=i.type[0]=='p'?'text':'password'">👁</button></div></div><div class=actions><button type=reset class="btn secondary">Reset</button><button type=submit class=btn>Save</button></div></form></div></body></html>)HTML";

static uint64_t nowUnix() {
  time_t now = 0;
  time(&now);
  return (uint64_t)now;
}

SetupService::SetupService(PreferenceService& prefs, WebServer& server, const Config& cfg)
: _prefs(prefs), _server(server), _cfg(cfg) {}

static bool hasWifiCreds(PreferenceService& prefs) {
  return prefs.hasWifi();
}
static bool codesExist(PreferenceService& prefs) {
  return prefs.hasProvisioningCodes();
}
static bool tokenExists(PreferenceService& prefs) {
  return prefs.hasAuth();
}

bool SetupService::isSetupComplete() const {
  // ready for runtime: have wifi, have token, and no pending codes.
  if (!hasWifiCreds(_prefs)) return false;
  if (codesExist(_prefs)) return false;
  return tokenExists(_prefs);
}

void SetupService::begin() {
  // If portal was running from a previous mode, stop it.
  stopPortal();

  if (!hasWifiCreds(_prefs)) {
    startPortal();
    return;
  }

  // If codes exist, run provisioning as part of setup.
  if (codesExist(_prefs)) {
    Serial.println("=== SETUP: PROVISIONING ===");

    if (!wifiConnectSTA()) {
      Serial.println("[SETUP] WiFi connect FAILED -> starting portal");
      startPortal();
      return;
    }

    if (!ensureTimeSynced()) {
      Serial.println("[SETUP] NTP sync FAILED -> starting portal");
      startPortal();
      return;
    }

    if (!authProvision()) {
      Serial.println("[SETUP] Provision FAILED -> starting portal");
      startPortal();
      return;
    }

    // Clear codes and reboot to runtime
    _prefs.clearProvisioningCodes();

    Serial.println("[SETUP] Provision OK -> reboot to RUN");
    delay(300);
    ESP.restart();
    return;
  }

  // If we have wifi but no token -> portal
  if (!tokenExists(_prefs)) {
    startPortal();
    return;
  }

  // Setup complete: nothing to do.
  Serial.println("[SETUP] Setup complete.");
}

void SetupService::loop() {
  if (_portalStarted) {
    _server.handleClient();
  } else {
    // When not portalStarted, setup is running provisioning or done. Nothing to do here.
    delay(5);
  }
}

void SetupService::startPortal() {
  if (_portalStarted) return;

  WiFi.mode(WIFI_AP);
  IPAddress apIP(192,168,4,1);
  IPAddress netmask(255,255,255,0);
  WiFi.softAPConfig(apIP, apIP, netmask);
  WiFi.softAP(_cfg.apSsid);

  _server.on("/", HTTP_GET, [&]() {
    _server.send_P(200, "text/html; charset=utf-8", SETUP_PAGE_HTML);
  });

  _server.on("/setup", HTTP_POST, [&]() {
    const String code1 = _server.arg("code1");
    const String code2 = _server.arg("code2");
    const String ssid  = _server.arg("wifi");
    const String pass  = _server.arg("wifiPassword");

    if (code1.length() == 0 || code2.length() == 0 || ssid.length() == 0) {
      _server.send(400, "text/plain; charset=utf-8", "Missing fields");
      return;
    }

    PreferenceService::ProvisioningCodes pc;
    pc.code1 = code1;
    pc.code2 = code2;
    _prefs.saveProvisioningCodes(pc);

    PreferenceService::WifiConfig wc;
    wc.ssid = ssid;
    wc.password = pass;
    _prefs.saveWifi(wc);

    // reset auth (deviceKey + tokens)
    _prefs.clearAuth();

    _server.send(200, "text/plain; charset=utf-8", "Saved. Rebooting...");
    delay(300);
    ESP.restart();
  });

  _server.begin();
  _portalStarted = true;

  Serial.println("=== SETUP MODE ===");
  Serial.print("AP SSID: "); Serial.println(_cfg.apSsid);
  Serial.print("AP IP:   "); Serial.println(WiFi.softAPIP());
  Serial.println("Open http://192.168.4.1/");
}

void SetupService::stopPortal() {
  if (!_portalStarted) return;
  _server.stop();
  _portalStarted = false;
}

bool SetupService::wifiConnectSTA(uint32_t timeoutMs) {
  auto w = _prefs.loadWifi();
  if (w.ssid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(w.ssid.c_str(), w.password.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool SetupService::ensureTimeSynced(uint32_t timeoutMs) {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    time_t now; time(&now);
    if (now > 1700000000) return true;
    delay(250);
  }
  return false;
}

static bool parseAndStoreTokens(PreferenceService& prefs, const String& respJson) {
  JsonDocument doc;
  auto err = deserializeJson(doc, respJson);
  if (err) {
    Serial.print("[SETUP] JSON parse error: "); Serial.println(err.c_str());
    Serial.println(respJson);
    return false;
  }

  JsonVariant root = doc;
  if (doc["data"].is<JsonObject>()) root = doc["data"];

  const char* devId   = root["deviceId"];
  const char* access  = root["accessToken"];
  const char* refresh = root["refreshToken"];

  long expiresIn =
    root["expiresIn"].is<long>() ? root["expiresIn"].as<long>() :
    root["expiresIn"].is<int>() ? (long)root["expiresIn"].as<int>() :
    root["expiresIn"].is<const char*>() ? atol(root["expiresIn"].as<const char*>()) : 0;

  if (!access || !refresh || expiresIn <= 0) {
    Serial.println("[SETUP] Token payload invalid");
    Serial.println(respJson);
    return false;
  }

  const String accessS(access);
  const String refreshS(refresh);
  const uint64_t exp = nowUnix() + (uint64_t)expiresIn;

  if (devId && *devId) prefs.setDeviceKey(String(devId));

  return prefs.updateAuthTokensChecked(accessS, refreshS, exp);
}

bool SetupService::authProvision() {
  auto codes = _prefs.loadProvisioningCodes();
  if (codes.code1.length() == 0 || codes.code2.length() == 0) return false;

  String url = String(_cfg.apiBase) + "/api/device/provisioningsession/" + codes.code1 + "/" + codes.code2;

  WiFiClientSecure client;
  client.setCACert(LE_CA);

  HTTPClient http;
  http.begin(client, url);
  int code = http.GET();
  String resp = (code > 0) ? http.getString() : "";
  http.end();

  Serial.print("[SETUP] Provision(GET) HTTP code: "); Serial.println(code);

  if (code < 200 || code >= 300) {
    Serial.println(resp);
    return false;
  }

  return parseAndStoreTokens(_prefs, resp);
}

String SetupService::readPayloadToString(const uint8_t* payload, size_t len) {
  String body;
  body.reserve(len + 1);
  for (size_t i = 0; i < len; i++) body += (char)payload[i];
  return body;
}
