#if !defined(DEVICE_ROLE_PROBE) && !defined(DEVICE_ROLE_GATEWAY)
#error "You must define DEVICE_ROLE_PROBE or DEVICE_ROLE_GATEWAY in build_flags"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

#include "../include/PreferenceService.h"
#include "../include/SetupService.h"
#ifdef DEVICE_ROLE_PROBE
#include "../include/ProbeRunService.h"
#else
#include "../include/MqttService.h"
#include "../include/RunService.h"
#endif

static const char *NVS_NS = "fluxspool";

PreferenceService prefSvc(NVS_NS);
WebServer httpServer(80);

SetupService::Config setupCfg;
SetupService setupSvc(prefSvc, httpServer, setupCfg);

#ifdef DEVICE_ROLE_PROBE

ProbeRunService::Config probeCfg;
ProbeRunService runSvc(prefSvc, probeCfg);

static bool g_runtimeStarted = false;

void setup()
{
  Serial.begin(115200);
  delay(200);

  prefSvc.begin(false);

  if (setupSvc.isSetupComplete())
  {
    Serial.println("[BOOT] Setup complete -> starting probe runtime");
    runSvc.begin();
    g_runtimeStarted = true;
  }
  else
  {
    Serial.println("[BOOT] Setup required -> starting setup service");
    setupSvc.begin();
  }
}

void loop()
{
  if (setupSvc.isSetupComplete())
  {
    if (!g_runtimeStarted)
    {
      runSvc.begin();
      g_runtimeStarted = true;
    }
    runSvc.loop();
  }
  else
  {
    setupSvc.loop();
  }

  delay(5);
}
#else

RunService::Config runCfg;
MqttService mqttSvc(runCfg.mqttBase, 8883);
RunService runSvc(prefSvc, mqttSvc, runCfg);

static bool g_runtimeStarted = false;

void setup()
{
  Serial.begin(115200);
  delay(200);

  prefSvc.begin(false);

  // Decide mode
  if (setupSvc.isSetupComplete())
  {
    Serial.println("[BOOT] Setup complete -> starting runtime");
    runSvc.begin();
    g_runtimeStarted = true;
  }
  else
  {
    Serial.println("[BOOT] Setup required -> starting setup service");
    setupSvc.begin();
  }
}

void loop()
{
  if (setupSvc.isSetupComplete())
  {
    if (!g_runtimeStarted)
    {
      // Setup just completed (or provisioned + reboot not used)
      runSvc.begin();
      g_runtimeStarted = true;
    }
    runSvc.loop();
  }
  else
  {
    setupSvc.loop();
  }

  delay(5);
}
#endif