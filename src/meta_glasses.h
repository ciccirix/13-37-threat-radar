#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_gap_ble_api.h"

// ---------------------------------------------------------------------------
// Meta / smart-glasses detector — ported from the ESP32-Marauder C5 "RAYBAN"
// sniffer (filters originally from NullPxl). Meta Ray-Ban glasses randomise
// their BLE MAC, so an OUI match is unreliable; the robust signal is the BLE
// COMPANY ID (manufacturer data) / 16-bit SERVICE UUID / SERVICE DATA UUID.
// We flag an advert whose identifier is a known Meta/Luxottica one AND is not
// one of the common blocked IDs (Apple/Samsung/MS/phone), which keeps false
// positives down.
// ---------------------------------------------------------------------------

// Classify one BLE advertisement. Returns true on a Meta match (also folds it
// into the live list). Safe to call from the shared scanner too.
bool meta_glasses_check(const uint8_t *mac6, int8_t rssi,
                        const uint8_t *adv, int adv_len);

// Standalone tile: start/stop a dedicated BLE scan feeding the detector.
bool meta_glasses_start();
void meta_glasses_stop();
bool meta_glasses_is_running();

// Live list for the screen.
struct MetaHit {
    uint8_t  mac[6];
    int8_t   rssi;
    uint16_t id;        // the Meta identifier that matched
    uint16_t hits;
    uint32_t last_ms;
};
int  meta_glasses_count();                 // distinct glasses seen this session
int  meta_glasses_get(MetaHit *out, int max);
void meta_glasses_reset();
