#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "threat_radar.h"   // TrCategory — the classified kinds we surface

// ---------------------------------------------------------------------------
// Scan Engine — the brain behind the unified Scanner screen.
//
// It drives scan_radio (time-sliced WiFi promiscuous <-> BLE scan, never both
// at once) with one WiFi and one BLE callback. Each callback funnels the raw
// hit through EVERY detector (Evil-Twin / Flock / AirTag / Flipper / Skimmer)
// exactly the way the wardriver does, so the same SD logging and Threat-Radar
// correlation fire — but here the results are also folded into one live store
// the dashboard reads: unique-device counts per radio and a ranked list of the
// devices a detector flagged.
//
// Threading: the two callbacks run on the radio task and only push a compact
// hit onto a FreeRTOS queue — no store mutation there. scan_engine_tick(),
// called from the main/LVGL task, drains that queue into the store. The store
// is therefore single-task (main only) and needs no lock.
// ---------------------------------------------------------------------------

// A device is "plain" (just counted) unless a detector claimed it.
#define SCAN_CAT_NONE 0xFF

enum ScanKind { SCAN_KIND_WIFI = 0, SCAN_KIND_BLE = 1 };

// One row in the live store / list.
struct ScanDev {
    uint8_t  mac[6];
    int8_t   rssi;        // most recent
    int8_t   best_rssi;   // strongest (closest) ever seen
    uint8_t  kind;        // ScanKind
    uint8_t  category;    // TrCategory or SCAN_CAT_NONE
    uint16_t hits;        // times re-seen
    uint32_t last_ms;     // millis() of last sighting
    char     name[24];    // SSID / BLE local name ("" if none)
};

// Aggregate snapshot for the dashboard's big counters.
struct ScanStats {
    uint16_t wifi;                    // unique WiFi APs seen (active window)
    uint16_t ble;                     // unique BLE devices seen (active window)
    uint16_t threats;                 // active devices a detector flagged
    uint16_t per_cat[TR_CAT_COUNT];   // active count per detector category
    uint32_t total_seen;             // every sighting folded in since start
};

// Bring the radios up (via scan_radio) and clear the store. Idempotent.
void scan_engine_start();
void scan_engine_stop();
bool scan_engine_running();

// Main-task pump: drains the hit queue into the store and ages out stale
// devices. Call every UI refresh while the screen is up.
void scan_engine_tick();

// Wipe the store + counters (keeps scanning). For the CLEAR button.
void scan_engine_reset();

// Read API (main task only).
void scan_engine_stats(ScanStats *out);
// Fills up to `max` flagged devices, most-relevant first (active desc, then
// category priority, then strongest RSSI). Returns the count written.
int  scan_engine_get_devices(ScanDev *out, int max);

// True while the WiFi slice is active (for the phase indicator).
bool scan_engine_wifi_phase();
