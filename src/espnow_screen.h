#pragma once

// Tools sub-screen for the out-of-mesh ESP-NOW link: a power toggle, this
// watch's identity + channel, a scrollable list of nearby peers (tap a peer to
// compose a direct message via the shared send screen), a Broadcast row, and a
// preview of the most recent received message. Swipe up returns to Tools.

void espnow_screen_create();
void espnow_screen_show();
bool espnow_screen_is_active();
