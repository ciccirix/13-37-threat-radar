#pragma once
#include "wifi_beacon_manager.h"
#include "ble_scan_manager.h"

// Time-sliced radio scheduler for the unified Scanner (AIR mode).
//
// On this ESP32-S3 build WiFi-promiscuous and BLE-scan CANNOT run at the same
// time — doing so hard-crashes/reboots the watch (radio coexistence; it's the
// same root cause behind the INT_WDT reboots). So a scanner that wants to catch
// BOTH worlds must alternate them. This scheduler holds WiFi up for a slice,
// tears it fully down, holds BLE up for a slice, and repeats forever — never
// both radios at once.
//
// The caller supplies one WiFi-beacon callback and one BLE-scan callback; each
// fires only during its own slice. Detectors classify from those callbacks and
// don't touch the radio managers directly.

void scan_radio_start(wifi_beacon_cb_t wifi_cb, ble_scan_cb_t ble_cb);
void scan_radio_stop();

// Pump from the main loop; switches the active slice when its time is up.
void scan_radio_tick();

// True while the WiFi slice is active, false during the BLE slice. For a status
// indicator ("scanning WiFi…" / "scanning BLE…").
bool scan_radio_wifi_phase();
bool scan_radio_running();
