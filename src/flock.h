#pragma once
#include <stdint.h>

// Checks a detected device against the surveillance-vendor OUI table and the
// device-name pattern list. `name` may be NULL or empty. Returns true if the
// device matched (and was newly logged after dedup).
bool flock_check(const uint8_t *mac6, int8_t rssi, const char *name, char source);

// Pure classifier: returns the surveillance/camera vendor for a MAC or advertised
// name, or nullptr if neither the OUI table nor the name-prefix list matches.
// No dedup / logging / threat-radar side effects, so the Cameras live-scan screen
// can call it on every beacon and BLE advert.
const char *flock_classify(const uint8_t *mac6, const char *name);

int  flock_get_count();
void flock_bg_tick();
void flock_reset_count();

// Standalone tile API -- starts/stops dedicated WiFi and BLE scans so the
// detector runs without the wardriver.
bool flock_start();
void flock_stop();
bool flock_is_running();
