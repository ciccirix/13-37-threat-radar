#pragma once

// Unified Scanner screen — one place that lights up WiFi + BLE (time-sliced),
// runs every detector, and shows live counters + a ranked list of flagged
// devices. Opening it starts the scan engine; leaving it (swipe up) stops it.
void scan_screen_show();
void scan_screen_create();
bool scan_screen_is_active();
