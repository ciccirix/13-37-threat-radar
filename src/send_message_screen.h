#pragma once
#include <lvgl.h>
#include <stdint.h>

void send_message_screen_create();

// Show the send screen in broadcast mode (sends to the public channel).
// Used by the normal swipe-chain entry from the Nodes screen.
void send_message_screen_show();

// Show the send screen pre-targeted at a specific node. The title bar
// switches to "DM -> [SHRT]" and the preset cell sends as a DM rather
// than a broadcast. Used by the Nodes screen's per-row tap handler.
void send_message_screen_show_to(uint32_t dest_node);

// ESP-NOW variant: reuse the same compose UI but route the send over the
// out-of-mesh ESP-NOW link to `peer_id` (ENOW_BROADCAST_ID = broadcast).
// The title switches to "ENOW -> [name]" and back-swipe returns to the
// ESP-NOW screen instead of the Meshtastic swipe chain. Used by the
// ESP-NOW screen's per-peer tap handler.
void send_message_screen_show_espnow_to(uint32_t peer_id);

bool send_message_screen_is_active();
