#pragma once

#include <Arduino.h>
#include <time.h>
#include "PreferenceService.h"

namespace netutils
{

// Returns current unix timestamp, or 0 if not yet synced.
inline uint64_t nowUnix()
{
  time_t now = 0;
  time(&now);
  return (uint64_t)now;
}

// Returns true if wall-clock time appears valid (> 2023-11-01).
inline bool timeIsValid(uint64_t t) { return t > 1700000000ULL; }

// Connect to WiFi using credentials stored in PreferenceService.
bool wifiConnectSTA(PreferenceService &prefs, uint32_t timeoutMs = 15000);

// Sync time via NTP. Skips the configTime call if already synced.
bool ensureTimeSynced(uint32_t timeoutMs = 8000);

} // namespace netutils
