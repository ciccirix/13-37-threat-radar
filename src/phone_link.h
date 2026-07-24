#pragma once
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Phone Link — BLE GATT bridge that mirrors the Threat Radar onto a phone.
//
// The watch advertises as "13-37" and exposes one custom service the
// companion web app (phoneapp/index.html, opened in a Web-Bluetooth browser
// such as Bluefy on iOS) subscribes to:
//
//   13370000-1337-4001-8000-746872616461  service
//   13370001-…  STATUS   read+notify, 8 bytes
//                [ver, threat_count, top_level, alert_seq, threats_seq,
//                 flags(bit0 = GPS powered), uptime_min lo, uptime_min hi]
//   13370002-…  THREATS  read, [ver, count] + count × 30-byte records
//                (mac6, category, level, waypoints, rssi, flags, pad,
//                 span_m u16, span_min u16, first_lat f32, first_lon f32,
//                 first_time "HH:MM\0") — little-endian, packed
//   13370003-…  CMD      write, 0x01 = wipe the contact store
//
// The phone polls nothing: STATUS notifies on every change, and the phone
// re-reads THREATS only when threats_seq bumps. alert_seq bumps once each
// time the worst contact crosses into LIKELY — the app's cue to raise hell.
//
// All GATT attribute updates happen from the main task via bg_tick (the
// threat-radar read API is main-task only); the BT task only sets flags.
// ---------------------------------------------------------------------------

// Bring the stack up (via ble_scan_manager's shared refcount), register the
// GATT service and start advertising. Returns false if the radio won't start.
bool phone_link_start();

// Tear the service down and release our claim on the stack. The radio stays
// up if the detectors are still scanning.
void phone_link_stop();

// User-facing state for the UI toggle / status rows.
bool phone_link_active();      // started (advertising or connected)
bool phone_link_connected();   // a phone is currently connected

// Main-task pump: serialize the threat-radar state into the GATT attributes,
// notify the phone on change, apply commands. Call from loop().
void phone_link_bg_tick();
