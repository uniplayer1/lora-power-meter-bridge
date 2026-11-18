#pragma once
// WiFi lifecycle and the captive-portal DNS.
//
// Two different behaviours, one config:
//   * the basement node always offers its own AP, so the owner can walk up to
//     the meter with a phone and see what is going on;
//   * the gateway offers an AP only until it has been provisioned, then it
//     joins the flat's network and the page lives on its IP (and via mDNS).
// If a configured network cannot be joined, the AP comes back: a wrong password
// must never lock the owner out of the device.

#include <Arduino.h>

#include "app_config.h"

void netBegin();
void netLoop();
void netReconfigure();  // after a settings change
void netStopAccessPoint();
// Applies a new AP name/password: the access point is torn down and raised
// again, because the SSID cannot be changed while it is running.
void netApplyApSettings();

bool netStaUp();
bool netApUp();
bool netInSetupMode();  // AP up because the device is unprovisioned or STA failed
bool netPortalActive();
String netIp();
int netStaRssi();
// The address of the device's own access point, which is the way in whenever the
// configured network is not reachable.
String netApIp();
uint32_t netApClients();
const char* netModeName();
const char* netStateName();
String netHostName();
