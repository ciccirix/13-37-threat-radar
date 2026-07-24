#pragma once
#include <stdint.h>
#include <stdbool.h>

// Dedicated 802.11 deauthentication transmitter. Targets a single AP by BSSID +
// channel and floods broadcast deauth/disassoc frames at its clients from a
// background task. Intended for testing the resilience of networks you own or
// are authorised to assess.
//
// On IDF 5.x the SDK rejects injected management frames unless the weak symbol
// ieee80211_raw_frame_sanity_check() is overridden — deauther.cpp does that.

// Begin flooding `bssid` on `channel`. Puts WiFi into STA+promiscuous, fixes the
// channel, and spawns the TX task. Returns false if already running.
bool deauther_start(const uint8_t bssid[6], uint8_t channel);

// Stop the flood and return the radio to its pre-attack state.
void deauther_stop();

bool     deauther_is_running();
uint32_t deauther_frames_sent();
