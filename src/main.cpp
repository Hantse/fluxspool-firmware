#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

#include "../include/PreferenceService.h"
#include "../include/MqttService.h"
#include "../include/SetupService.h"
#ifdef DEVICE_ROLE_PROBE
#include "ProbeRunService.h"
#else
#include "../include/RunService.h"
#endif

static const char* NVS_NS = "fluxspool";

PreferenceService prefSvc(NVS_NS);
WebServer httpServer(80);

// MQTT service (host/port injected by RunService config)
MqttService mqttSvc("mqtt.fluxspool.app", 8883);

SetupService::Config setupCfg;
SetupService setupSvc(prefSvc, httpServer, setupCfg);

RunService::Config runCfg;
RunService runSvc(prefSvc, mqttSvc, runCfg);

static bool g_runtimeStarted = false;

void setup() {
  Serial.begin(115200);
  delay(200);

  prefSvc.begin(false);

  // Decide mode
  if (setupSvc.isSetupComplete()) {
    Serial.println("[BOOT] Setup complete -> starting runtime");
    runSvc.begin();
    g_runtimeStarted = true;
  } else {
    Serial.println("[BOOT] Setup required -> starting setup service");
    setupSvc.begin();
  }
}

void loop() {
  if (setupSvc.isSetupComplete()) {
    if (!g_runtimeStarted) {
      // Setup just completed (or provisioned + reboot not used)
      runSvc.begin();
      g_runtimeStarted = true;
    }
    runSvc.loop();
  } else {
    setupSvc.loop();
  }

  delay(5);
}
