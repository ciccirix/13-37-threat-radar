#pragma once
#include <stddef.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// WiFi credentials store — so you never fight the tiny on-screen keyboard.
//
// Credentials live in NVS (survive reboots). They can be SEEDED from a plain
// text file on the microSD — write it on a PC with a real keyboard, drop it in,
// and the watch reads + remembers it:
//
//   /wifi.txt   (on the SD card)
//   ------------------------------------
//   MyNetworkName
//   my-wifi-password
//
// (First non-comment line = SSID, second = password. "ssid: x" / "pass: y"
// key lines also work. Lines starting with # are ignored.)
//
// Once remembered, the WiFi screen connects with a single tap (no password
// prompt) and the watch auto-connects on boot.
// ---------------------------------------------------------------------------

// Read the stored credentials (NVS, seeding from /wifi.txt if NVS is empty).
// Returns false if we have none. pass may come back empty for open networks.
bool wifi_creds_get(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);

// Persist credentials to NVS (called after a successful manual connect).
void wifi_creds_save(const char *ssid, const char *pass);

// If we have a stored password for exactly this SSID, copy it out (true).
// Lets the WiFi screen skip the keyboard for a known network.
bool wifi_creds_pass_for(const char *ssid, char *pass, size_t pass_sz);

// Import /wifi.txt from the SD into NVS. Returns true if a network was read.
bool wifi_creds_seed_from_sd();

// Non-blocking: if we have creds and WiFi isn't up, start connecting (STA +
// auto-reconnect). Returns true if a connect was attempted. Safe to call at boot.
// No-op unless auto-connect is enabled (see below).
bool wifi_creds_autoconnect();

// Auto-connect preference (NVS). OFF by default: an active STA association
// fights the promiscuous channel-hopping the Scanner/Wardriver/etc. need, so the
// user opts in via a switch on the WiFi screen when they want WiFi at boot.
bool wifi_creds_autoconnect_enabled();
void wifi_creds_set_autoconnect(bool on);
