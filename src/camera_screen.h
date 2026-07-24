#pragma once
#include <lvgl.h>

// Live surveillance-camera scanner. Registers a WiFi-beacon and a BLE consumer,
// classifies every hit with flock_classify(), and shows a live list of nearby
// cameras / surveillance gear (vendor, MAC, channel, RSSI) sorted by signal.
void camera_screen_create();
void camera_screen_show();
void camera_screen_stop();          // drops the WiFi/BLE consumers (radio cleanup)
bool camera_screen_is_active();
