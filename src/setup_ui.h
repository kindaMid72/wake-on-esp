#pragma once
#include <Arduino.h>

// Set by the BOOT button (held 3 s, released) or the `portal` serial command.
extern volatile bool portalRequested;

void uiInit();
// Polls the BOOT button and the serial command line. Call from every long wait.
void uiPoll();
// delay() that keeps uiPoll() running.
void uiDelay(uint32_t ms);
// Serves the setup page on the access point (and on the station IP if connected) for up to `ms`
// (extended while a client is connected to the AP). With exitOnWifi it also retries the saved
// networks while nobody is connected to the AP and returns true, setting *connectedNet, once one
// connects. Saving a config reboots the device.
bool runPortal(uint32_t ms, bool exitOnWifi, int *connectedNet);
