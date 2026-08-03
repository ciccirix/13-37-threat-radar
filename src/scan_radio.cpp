#include "scan_radio.h"
#include <Arduino.h>

// Slice lengths. Long enough that each radio is worth bringing up: the WiFi
// manager hops channels every 200 ms (so ~12 channels per WiFi slice) and BLE
// advertisers repeat every ~100 ms-1 s, so a couple of seconds each catches the
// bulk of what's around without thrashing the controllers.
#define SLICE_WIFI_MS 2600
#define SLICE_BLE_MS  2400

static wifi_beacon_cb_t s_wifi_cb  = nullptr;
static ble_scan_cb_t    s_ble_cb   = nullptr;
static bool             s_running  = false;
static bool             s_wifi     = true;
static uint32_t         s_phase_ms = 0;

// The ONLY rule that matters: bring one radio fully DOWN before the other comes
// UP. Both add/remove calls are synchronous on the main task, so sequencing them
// here guarantees the two stacks never overlap.
static void enter_wifi_phase()
{
    if (s_ble_cb)  ble_scan_remove(s_ble_cb);      // BLE down first
    if (s_wifi_cb) wifi_beacon_add(s_wifi_cb);     // …then WiFi up
    s_wifi = true;
    s_phase_ms = millis();
}

static void enter_ble_phase()
{
    if (s_wifi_cb) wifi_beacon_remove(s_wifi_cb);  // WiFi down first
    if (s_ble_cb)  ble_scan_add(s_ble_cb);         // …then BLE up
    s_wifi = false;
    s_phase_ms = millis();
}

void scan_radio_start(wifi_beacon_cb_t wifi_cb, ble_scan_cb_t ble_cb)
{
    if (s_running) scan_radio_stop();
    s_wifi_cb = wifi_cb;
    s_ble_cb  = ble_cb;
    s_running = true;
    enter_wifi_phase();     // start on the WiFi slice
}

void scan_radio_stop()
{
    if (!s_running) return;
    if (s_wifi) { if (s_wifi_cb) wifi_beacon_remove(s_wifi_cb); }
    else        { if (s_ble_cb)  ble_scan_remove(s_ble_cb); }
    s_running = false;
    s_wifi_cb = nullptr;
    s_ble_cb  = nullptr;
}

void scan_radio_tick()
{
    if (!s_running) return;
    uint32_t now = millis();
    if (s_wifi) {
        if (now - s_phase_ms >= SLICE_WIFI_MS) enter_ble_phase();
    } else {
        if (now - s_phase_ms >= SLICE_BLE_MS) enter_wifi_phase();
    }
}

bool scan_radio_wifi_phase() { return s_wifi; }
bool scan_radio_running()    { return s_running; }
