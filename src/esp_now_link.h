#pragma once
#include <stdint.h>
#include <stddef.h>

// ESP-NOW device-to-device link — an *out-of-mesh* transport that lets two (or
// a small group of) T-Watch Ultras find each other and exchange direct text
// messages over raw 2.4 GHz WiFi, with no AP, router or Meshtastic in between.
//
// Why ESP-NOW and not the LoRa mesh: the ask was "fast, ~100 m max". ESP-NOW is
// connectionless WiFi unicast — millisecond latency, ~250 B/packet, hardware
// AES on unicast — which is exactly the short-range/high-speed corner where
// BLE is too slow and LoRa is overkill (km range but bytes/s, plus EU duty
// cycle). See the module .cpp header for the full rationale and its limits.
//
// Radio contention: ESP-NOW claims the WiFi radio in STA mode on a fixed
// channel. While the link is active it is mutually exclusive with the WiFi
// scan/attack tools (deauther, evil-twin, wardriver, analyzer) that reconfigure
// the same radio. It coexists with the BLE detectors (Threat Radar / phone
// link) via the S3's radio time-slicing, at a small throughput cost.

#define ENOW_MAX_PEERS       16
#define ENOW_MAX_MESSAGES    24
#define ENOW_MAX_TEXT_LEN    200
#define ENOW_NAME_LEN        16   // long-ish display name, NUL-terminated
#define ENOW_BROADCAST_ID    0xFFFFFFFFu

// A peer heard advertising itself via a presence beacon (broadcast).
// Note: the legacy esp_now_register_recv_cb() signature this SDK (arduino-esp32
// 2.0.x / IDF 4.4) exposes does not carry per-frame RSSI — only src MAC, data,
// len. So peers/messages are surfaced by last-heard age rather than signal
// strength; a promiscuous-mode RSSI hook would be extra plumbing this V1
// doesn't need.
struct EnowPeer {
    uint8_t  mac[6];
    uint32_t id;                 // 32-bit identity derived from the low MAC bytes
    char     name[ENOW_NAME_LEN];
    uint32_t last_heard_ms;      // millis() of the last frame from this peer
    bool     encrypted;          // registered as an AES-encrypted ESP-NOW peer
};

// One entry in the shared TX+RX log ring (newest first).
struct EnowMessage {
    uint32_t peer_id;            // sender id (RX) or destination id (TX)
    char     name[ENOW_NAME_LEN];// sender/destination display name
    char     text[ENOW_MAX_TEXT_LEN];
    char     time_str[6];        // "HH:MM\0"
    bool     outgoing;           // true = we sent it, false = received
    bool     encrypted;          // frame carried over an encrypted peer link
};

// ---- lifecycle ------------------------------------------------------------

// Claim the WiFi radio (STA mode, fixed channel), start ESP-NOW and begin
// beaconing our presence. Returns false if the radio couldn't be brought up.
// Snapshots the previous WiFi mode and restores it on set_active(false).
bool esp_now_link_set_active(bool on);
bool esp_now_link_is_active();

// Pump: drains frames received in the WiFi-task callback into the peer table +
// message ring, emits a periodic presence beacon, and ages out stale peers.
// Called from the main loop's background tick. Cheap when idle.
void esp_now_link_bg_tick();

// ---- identity -------------------------------------------------------------

uint32_t    esp_now_link_self_id();       // derived from this watch's STA MAC
const char *esp_now_link_self_name();
void        esp_now_link_set_name(const char *name);   // not persisted (V1)
uint8_t     esp_now_link_channel();

// ---- peers ----------------------------------------------------------------

int             esp_now_link_peer_count();
const EnowPeer *esp_now_link_peer(int idx);   // 0 = most recently heard
const EnowPeer *esp_now_link_peer_by_id(uint32_t id);

// ---- send -----------------------------------------------------------------

// Queue a text frame to `dest_id`. ENOW_BROADCAST_ID sends to everyone on the
// channel (unencrypted); any other id sends an encrypted unicast DM to that
// peer. Returns false if the link is off, the text is empty/too long, or the
// destination id is unknown. The sent text is also appended to the message
// ring so it shows in the conversation view.
bool esp_now_link_send_text_to(const char *text, uint32_t dest_id);

// ---- inbox ----------------------------------------------------------------

int                esp_now_link_msg_count();
const EnowMessage *esp_now_link_msg(int idx);   // 0 = newest
int                esp_now_link_unread();
void               esp_now_link_mark_read();
void               esp_now_link_clear_messages();

// ---- stats ----------------------------------------------------------------

int  esp_now_link_rx_count();   // total valid frames received since boot
int  esp_now_link_tx_count();   // total frames we queued for transmit
