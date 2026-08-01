#include "esp_now_link.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// ===========================================================================
// ESP-NOW out-of-mesh link — implementation.
//
// Design in one screen:
//
//   * Transport. WiFi in STA mode on a FIXED channel (ENOW_CHANNEL). We never
//     associate to an AP, so the channel stays put — the classic ESP-NOW trap
//     (a STA association drags the channel to the AP's) does not apply here.
//     Power-save is disabled so an idle listener never misses a frame.
//
//   * Discovery. There is no ESP-NOW node discovery, so we roll our own: every
//     ENOW_BEACON_MS each watch broadcasts a tiny presence beacon carrying its
//     id + display name. Peers populate a table (mirrors the Meshtastic Nodes
//     screen concept). A peer not heard for ENOW_PEER_TTL_MS is aged out.
//
//   * Addressing. Unicast is by 6-byte MAC. We surface a stable 32-bit id
//     (low four MAC bytes) so the UI + send_message_screen can address a peer
//     the same way they address a Meshtastic node.
//
//   * Encryption. A shared 16-byte group key doubles as the ESP-NOW PMK and the
//     per-peer LMK, so every watch that shares the key can exchange AES unicast
//     DMs — the same "one channel PSK for the group" model Meshtastic uses.
//     Broadcast beacons are unencrypted (they carry only id + name). If the
//     hardware's encrypted-peer slots (6) fill up we fall back to an
//     unencrypted registration and flag the message so the UI never lies about
//     confidentiality. CHANGE s_group_key per group for real privacy.
//
//   * Threading. The RX callback runs in the WiFi task; it only copies the raw
//     frame into a spinlock-guarded staging ring. All parsing, table updates
//     and message-ring writes happen in bg_tick() on the main loop task, so the
//     UI reads the peer/message arrays without locking.
// ===========================================================================

// Fixed operating channel. Both ends must agree; 1 is the least-congested
// default across regions and is legal everywhere the watch ships.
static const uint8_t ENOW_CHANNEL = 1;

// Presence + housekeeping cadences.
static const uint32_t ENOW_BEACON_MS    = 4000;    // broadcast our presence
static const uint32_t ENOW_PEER_TTL_MS  = 60000;   // drop peers unheard this long

// Hardware caps (from esp_now.h): 20 total peers, 6 of them encrypted. We keep
// the broadcast peer plus a handful of encrypted unicast peers, evicting the
// least-recently-used unicast slot when full.
static const int ENOW_MAX_ENCRYPTED = 6;

// Shared group key — PMK and per-peer LMK. Replace with your own 16 random
// bytes so only your watches can read each other's DMs. Watches with different
// keys still see each other's (plaintext) beacons but cannot decrypt DMs.
static uint8_t s_group_key[16] = {
    '1','3','3','7','-','e','n','o','w','-','g','r','o','u','p','!'
};

static const uint8_t BROADCAST_MAC[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

// ---- wire format ----------------------------------------------------------

#define ENOW_MAGIC      0x1337
#define ENOW_PROTO_VER  1

enum EnowType : uint8_t {
    ENOW_T_BEACON = 1,   // presence advertisement (broadcast, unencrypted)
    ENOW_T_TEXT   = 2,   // text message (broadcast or unicast)
};

struct __attribute__((packed)) EnowFrame {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  type;
    uint32_t src_id;
    char     src_name[ENOW_NAME_LEN];
    uint16_t text_len;                  // valid bytes in text[] (TEXT only)
    char     text[ENOW_MAX_TEXT_LEN];
};

// Bytes actually sent for a frame: fixed header + only the used text bytes.
static size_t frame_len(const EnowFrame *f)
{
    return offsetof(EnowFrame, text) + f->text_len;
}

// ---- module state ---------------------------------------------------------

static bool        s_active   = false;
static wifi_mode_t s_prev_mode = WIFI_MODE_NULL;

static uint32_t s_self_id = 0;
static char     s_self_name[ENOW_NAME_LEN] = {0};
static uint8_t  s_self_mac[6] = {0};

static EnowPeer s_peers[ENOW_MAX_PEERS];
static int      s_peer_count = 0;

static EnowMessage s_msgs[ENOW_MAX_MESSAGES];
static int         s_msg_count = 0;   // valid entries; index 0 = newest
static int         s_unread    = 0;

static uint32_t s_last_beacon_ms = 0;
static int      s_rx_total = 0;
static int      s_tx_total = 0;

// Registered ESP-NOW unicast peers (excludes the broadcast peer), tracked so we
// can honour the encrypted-slot cap and evict least-recently-used entries.
struct RegPeer {
    uint8_t  mac[6];
    bool     used;
    bool     encrypted;
    uint32_t last_used_ms;
};
static RegPeer s_reg[ENOW_MAX_PEERS];
static int     s_encrypted_count = 0;

// ---- RX staging (WiFi task -> main task) ----------------------------------

struct RawFrame {
    uint8_t  mac[6];
    uint16_t len;
    uint8_t  buf[sizeof(EnowFrame)];
};
#define STAGE_SLOTS 8
static RawFrame        s_stage[STAGE_SLOTS];
static volatile int    s_stage_head = 0;   // written by WiFi task
static volatile int    s_stage_tail = 0;   // read by main task
static portMUX_TYPE    s_stage_mux = portMUX_INITIALIZER_UNLOCKED;

// ---- small helpers --------------------------------------------------------

static uint32_t id_from_mac(const uint8_t *mac)
{
    return ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
           ((uint32_t)mac[4] <<  8) |  (uint32_t)mac[5];
}

static void now_hhmm(char *out /* [6] */)
{
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    // A watch fresh off battery with no RTC sync reports 1970; still harmless.
    strftime(out, 6, "%H:%M", &tmv);
}

static void default_self_name()
{
    // "TW-AB12" from the last two MAC bytes — short, unique enough for a group.
    snprintf(s_self_name, sizeof(s_self_name), "TW-%02X%02X",
             s_self_mac[4], s_self_mac[5]);
}

// ---- peer table (main task only) ------------------------------------------

static EnowPeer *find_peer(uint32_t id)
{
    for (int i = 0; i < s_peer_count; i++)
        if (s_peers[i].id == id) return &s_peers[i];
    return nullptr;
}

// Insert/refresh a peer and move it to the front (most-recently-heard first).
static void touch_peer(const uint8_t *mac, uint32_t id, const char *name,
                       bool encrypted)
{
    int idx = -1;
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].id == id) { idx = i; break; }
    }
    EnowPeer p;
    memset(&p, 0, sizeof(p));
    memcpy(p.mac, mac, 6);
    p.id = id;
    strncpy(p.name, (name && name[0]) ? name : "?", ENOW_NAME_LEN - 1);
    p.last_heard_ms = millis();
    p.encrypted = encrypted;

    if (idx < 0) {
        if (s_peer_count < ENOW_MAX_PEERS) {
            // Shift down to make room at the front.
            for (int i = s_peer_count; i > 0; i--) s_peers[i] = s_peers[i - 1];
            s_peers[0] = p;
            s_peer_count++;
        } else {
            // Full: replace the oldest (last) entry.
            for (int i = ENOW_MAX_PEERS - 1; i > 0; i--) s_peers[i] = s_peers[i - 1];
            s_peers[0] = p;
        }
    } else {
        // Existing: keep the encrypted flag if already true, refresh the rest,
        // and float to the front.
        p.encrypted = s_peers[idx].encrypted || encrypted;
        for (int i = idx; i > 0; i--) s_peers[i] = s_peers[i - 1];
        s_peers[0] = p;
    }
}

static void age_out_peers()
{
    uint32_t now = millis();
    int w = 0;
    for (int i = 0; i < s_peer_count; i++) {
        if (now - s_peers[i].last_heard_ms <= ENOW_PEER_TTL_MS) {
            if (w != i) s_peers[w] = s_peers[i];
            w++;
        }
    }
    s_peer_count = w;
}

// ---- message ring (main task only) ----------------------------------------

static void push_message(uint32_t peer_id, const char *name, const char *text,
                         bool outgoing, bool encrypted)
{
    // Shift newest-first.
    for (int i = ENOW_MAX_MESSAGES - 1; i > 0; i--) s_msgs[i] = s_msgs[i - 1];
    EnowMessage *m = &s_msgs[0];
    memset(m, 0, sizeof(*m));
    m->peer_id = peer_id;
    strncpy(m->name, (name && name[0]) ? name : "?", ENOW_NAME_LEN - 1);
    strncpy(m->text, text ? text : "", ENOW_MAX_TEXT_LEN - 1);
    now_hhmm(m->time_str);
    m->outgoing = outgoing;
    m->encrypted = encrypted;
    if (s_msg_count < ENOW_MAX_MESSAGES) s_msg_count++;
    if (!outgoing) s_unread++;
}

// ---- ESP-NOW peer registration --------------------------------------------

static RegPeer *reg_find(const uint8_t *mac)
{
    for (int i = 0; i < ENOW_MAX_PEERS; i++)
        if (s_reg[i].used && memcmp(s_reg[i].mac, mac, 6) == 0) return &s_reg[i];
    return nullptr;
}

static RegPeer *reg_free_slot()
{
    for (int i = 0; i < ENOW_MAX_PEERS; i++)
        if (!s_reg[i].used) return &s_reg[i];
    return nullptr;
}

// Evict the least-recently-used registered unicast peer to free a slot.
static void reg_evict_lru()
{
    RegPeer *lru = nullptr;
    for (int i = 0; i < ENOW_MAX_PEERS; i++) {
        if (!s_reg[i].used) continue;
        if (!lru || s_reg[i].last_used_ms < lru->last_used_ms) lru = &s_reg[i];
    }
    if (!lru) return;
    esp_now_del_peer(lru->mac);
    if (lru->encrypted) s_encrypted_count--;
    lru->used = false;
}

// Make sure `mac` is registered with ESP-NOW so we can send to / decrypt from
// it. Prefers an encrypted registration; degrades to unencrypted when the
// encrypted slots are exhausted. Returns whether the final registration is
// encrypted.
static bool ensure_registered(const uint8_t *mac)
{
    RegPeer *r = reg_find(mac);
    if (r) { r->last_used_ms = millis(); return r->encrypted; }

    r = reg_free_slot();
    if (!r) { reg_evict_lru(); r = reg_free_slot(); }
    if (!r) return false;   // should not happen

    bool want_enc = (s_encrypted_count < ENOW_MAX_ENCRYPTED);

    esp_now_peer_info_t info;
    memset(&info, 0, sizeof(info));
    memcpy(info.peer_addr, mac, 6);
    info.channel = ENOW_CHANNEL;
    info.ifidx   = WIFI_IF_STA;
    info.encrypt = want_enc;
    if (want_enc) memcpy(info.lmk, s_group_key, 16);

    esp_err_t e = esp_now_add_peer(&info);
    if (e != ESP_OK && want_enc) {
        // Retry unencrypted so the peer is at least reachable in the clear.
        want_enc = false;
        info.encrypt = false;
        memset(info.lmk, 0, 16);
        e = esp_now_add_peer(&info);
    }
    if (e != ESP_OK) return false;

    memcpy(r->mac, mac, 6);
    r->used = true;
    r->encrypted = want_enc;
    r->last_used_ms = millis();
    if (want_enc) s_encrypted_count++;
    return want_enc;
}

// ---- RX callback (WiFi task) ----------------------------------------------

// arduino-esp32 2.0.x / IDF 4.4 uses the legacy recv-callback signature:
// (src_mac, data, len). Per-frame RSSI is not exposed here (it would need a
// promiscuous RX hook), so peers are surfaced by last-heard age instead.
static void on_recv(const uint8_t *mac, const uint8_t *data, int len)
{
    if (!mac || !data || len <= 0 || len > (int)sizeof(EnowFrame)) return;

    portENTER_CRITICAL(&s_stage_mux);
    int next = (s_stage_head + 1) % STAGE_SLOTS;
    if (next != s_stage_tail) {          // drop on overflow rather than block
        RawFrame *rf = &s_stage[s_stage_head];
        memcpy(rf->mac, mac, 6);
        rf->len  = (uint16_t)len;
        memcpy(rf->buf, data, len);
        s_stage_head = next;
    }
    portEXIT_CRITICAL(&s_stage_mux);
}

// Parse one staged frame on the main task.
static void process_frame(const RawFrame *rf)
{
    if (rf->len < offsetof(EnowFrame, text)) return;
    EnowFrame f;
    memcpy(&f, rf->buf, rf->len < sizeof(f) ? rf->len : sizeof(f));
    if (f.magic != ENOW_MAGIC || f.ver != ENOW_PROTO_VER) return;
    if (f.src_id == s_self_id) return;   // ignore our own broadcast echoes

    f.src_name[ENOW_NAME_LEN - 1] = '\0';
    s_rx_total++;

    // Any frame proves the peer exists — register it (encrypted if we can) so
    // future DMs in both directions work, and record it in the table.
    bool enc = ensure_registered(rf->mac);
    touch_peer(rf->mac, f.src_id, f.src_name, enc);

    if (f.type == ENOW_T_TEXT && f.text_len > 0) {
        int n = f.text_len;
        if (n > ENOW_MAX_TEXT_LEN - 1) n = ENOW_MAX_TEXT_LEN - 1;
        char text[ENOW_MAX_TEXT_LEN];
        memcpy(text, f.text, n);
        text[n] = '\0';
        push_message(f.src_id, f.src_name, text, /*outgoing=*/false, enc);
    }
}

static void drain_staging()
{
    for (;;) {
        RawFrame rf;
        bool have = false;
        portENTER_CRITICAL(&s_stage_mux);
        if (s_stage_tail != s_stage_head) {
            rf = s_stage[s_stage_tail];
            s_stage_tail = (s_stage_tail + 1) % STAGE_SLOTS;
            have = true;
        }
        portEXIT_CRITICAL(&s_stage_mux);
        if (!have) break;
        process_frame(&rf);
    }
}

// ---- beacon ---------------------------------------------------------------

static void send_beacon()
{
    EnowFrame f;
    memset(&f, 0, sizeof(f));
    f.magic  = ENOW_MAGIC;
    f.ver    = ENOW_PROTO_VER;
    f.type   = ENOW_T_BEACON;
    f.src_id = s_self_id;
    strncpy(f.src_name, s_self_name, ENOW_NAME_LEN - 1);
    f.text_len = 0;
    esp_now_send(BROADCAST_MAC, (const uint8_t *)&f, frame_len(&f));
}

// ===========================================================================
// public API
// ===========================================================================

bool esp_now_link_set_active(bool on)
{
    if (on == s_active) return true;

    if (on) {
        s_prev_mode = WiFi.getMode();
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false, true);   // drop any association; keep radio up
        esp_wifi_set_ps(WIFI_PS_NONE);  // reliable RX for an idle listener
        esp_wifi_set_channel(ENOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

        // Cache our identity from the STA MAC.
        esp_wifi_get_mac(WIFI_IF_STA, s_self_mac);
        s_self_id = id_from_mac(s_self_mac);
        if (!s_self_name[0]) default_self_name();

        if (esp_now_init() != ESP_OK) {
            // Roll back the radio to how we found it.
            if (s_prev_mode == WIFI_MODE_NULL) WiFi.mode(WIFI_OFF);
            else                               WiFi.mode(s_prev_mode);
            return false;
        }
        esp_now_set_pmk(s_group_key);
        esp_now_register_recv_cb(on_recv);

        // Broadcast peer for beacons + broadcast text (must be unencrypted).
        esp_now_peer_info_t bcast;
        memset(&bcast, 0, sizeof(bcast));
        memcpy(bcast.peer_addr, BROADCAST_MAC, 6);
        bcast.channel = ENOW_CHANNEL;
        bcast.ifidx   = WIFI_IF_STA;
        bcast.encrypt = false;
        esp_now_add_peer(&bcast);

        memset(s_reg, 0, sizeof(s_reg));
        s_encrypted_count = 0;
        s_last_beacon_ms  = 0;   // beacon promptly on the next tick
        s_active = true;
        send_beacon();           // announce immediately
        return true;
    }

    // Teardown.
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    memset(s_reg, 0, sizeof(s_reg));
    s_encrypted_count = 0;
    s_peer_count = 0;

    // Restore the radio exactly like the WiFi tools do, so the clock-face WiFi
    // indicator reflects reality.
    if (s_prev_mode == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_OFF);
        esp_wifi_stop();
        esp_wifi_deinit();
    } else {
        WiFi.mode(s_prev_mode);
    }
    s_prev_mode = WIFI_MODE_NULL;
    s_active = false;
    return true;
}

bool esp_now_link_is_active() { return s_active; }

void esp_now_link_bg_tick()
{
    if (!s_active) return;
    drain_staging();
    age_out_peers();
    uint32_t now = millis();
    if (now - s_last_beacon_ms >= ENOW_BEACON_MS) {
        s_last_beacon_ms = now;
        send_beacon();
    }
}

uint32_t    esp_now_link_self_id()   { return s_self_id; }
const char *esp_now_link_self_name() { return s_self_name; }
uint8_t     esp_now_link_channel()   { return ENOW_CHANNEL; }

void esp_now_link_set_name(const char *name)
{
    if (!name) return;
    strncpy(s_self_name, name, ENOW_NAME_LEN - 1);
    s_self_name[ENOW_NAME_LEN - 1] = '\0';
}

int             esp_now_link_peer_count() { return s_peer_count; }
const EnowPeer *esp_now_link_peer(int idx)
{
    if (idx < 0 || idx >= s_peer_count) return nullptr;
    return &s_peers[idx];
}
const EnowPeer *esp_now_link_peer_by_id(uint32_t id) { return find_peer(id); }

bool esp_now_link_send_text_to(const char *text, uint32_t dest_id)
{
    if (!s_active || !text || !text[0]) return false;
    size_t tlen = strlen(text);
    if (tlen > ENOW_MAX_TEXT_LEN - 1) tlen = ENOW_MAX_TEXT_LEN - 1;

    EnowFrame f;
    memset(&f, 0, sizeof(f));
    f.magic  = ENOW_MAGIC;
    f.ver    = ENOW_PROTO_VER;
    f.type   = ENOW_T_TEXT;
    f.src_id = s_self_id;
    strncpy(f.src_name, s_self_name, ENOW_NAME_LEN - 1);
    f.text_len = (uint16_t)tlen;
    memcpy(f.text, text, tlen);

    const uint8_t *dst;
    bool encrypted;
    const char *dst_name;

    if (dest_id == ENOW_BROADCAST_ID) {
        dst = BROADCAST_MAC;
        encrypted = false;
        dst_name = "Broadcast";
    } else {
        EnowPeer *p = find_peer(dest_id);
        if (!p) return false;                 // unknown destination
        dst = p->mac;
        encrypted = ensure_registered(p->mac);
        p->encrypted = encrypted;
        dst_name = p->name;
    }

    esp_err_t e = esp_now_send(dst, (const uint8_t *)&f, frame_len(&f));
    if (e != ESP_OK) return false;
    s_tx_total++;

    // Log our own message into the ring so it appears in the conversation.
    push_message(dest_id, dst_name, text, /*outgoing=*/true, encrypted);
    return true;
}

int esp_now_link_msg_count() { return s_msg_count; }
const EnowMessage *esp_now_link_msg(int idx)
{
    if (idx < 0 || idx >= s_msg_count) return nullptr;
    return &s_msgs[idx];
}
int  esp_now_link_unread()    { return s_unread; }
void esp_now_link_mark_read() { s_unread = 0; }
void esp_now_link_clear_messages() { s_msg_count = 0; s_unread = 0; }

int esp_now_link_rx_count() { return s_rx_total; }
int esp_now_link_tx_count() { return s_tx_total; }
