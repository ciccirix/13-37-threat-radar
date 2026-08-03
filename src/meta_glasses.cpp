#include "meta_glasses.h"
#include "ble_scan_manager.h"
#include <Arduino.h>
#include <string.h>

// ─── identifier tables (ported from ESP32-Marauder C5, filters via NullPxl) ──
// Extend META_IDS with more smart-glasses maker IDs — but use REAL Bluetooth
// SIG company IDs / assigned 16-bit UUIDs, never guesses (a wrong id = false
// positives). Oakley Meta needs nothing extra: Oakley is Luxottica (0x0D53).
static const uint16_t META_IDS[] = {
    0xFD5F,   // Meta
    0xFEB7,   // Meta
    0xFEB8,   // Meta
    0x01AB,   // Meta / Facebook
    0x058E,   // Meta
    0x0D53,   // Luxottica (Ray-Ban Meta, Oakley Meta)
};
// If any of these show up in the advert it's a phone/other, NOT glasses — skip.
static const uint16_t BLOCKED_IDS[] = {
    0xFD5A, 0xFD69,   // Samsung
    0x004C,           // Apple
    0x0006,           // Microsoft
    0xFEF3,           // common phone service
};

static bool is_meta(uint16_t id)
{
    for (unsigned i = 0; i < sizeof(META_IDS) / sizeof(META_IDS[0]); i++)
        if (META_IDS[i] == id) return true;
    return false;
}
static bool is_blocked(uint16_t id)
{
    for (unsigned i = 0; i < sizeof(BLOCKED_IDS) / sizeof(BLOCKED_IDS[0]); i++)
        if (BLOCKED_IDS[i] == id) return true;
    return false;
}

// Which identifier "wins" when an advert carries several (Ray-Ban glasses send
// both the Luxottica id and a Meta id). Luxottica (0x0D53) → rayban photo;
// Quest ids → quest; anything else → generic meta. Higher = preferred.
static int meta_prio(uint16_t id)
{
    if (id == 0x0D53) return 3;                     // Luxottica → Ray-Ban / Oakley
    if (id == 0x058E || id == 0xFD5F) return 2;     // Quest
    return 1;                                       // generic Meta
}

// ─── live store ──────────────────────────────────────────────────────────────
#define META_MAX 32
static MetaHit s_hits[META_MAX];
static int     s_count = 0;
static bool    s_running = false;

static void fold(const uint8_t *mac, int8_t rssi, uint16_t id, uint32_t now)
{
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_hits[i].mac, mac, 6) == 0) {
            s_hits[i].rssi = rssi;
            s_hits[i].id   = id;
            s_hits[i].last_ms = now;
            if (s_hits[i].hits < 0xFFFF) s_hits[i].hits++;
            return;
        }
    }
    if (s_count < META_MAX) {
        MetaHit *h = &s_hits[s_count++];
        memcpy(h->mac, mac, 6);
        h->rssi = rssi; h->id = id; h->hits = 1; h->last_ms = now;
    }
}

// ─── classifier ──────────────────────────────────────────────────────────────
bool meta_glasses_check(const uint8_t *mac6, int8_t rssi,
                        const uint8_t *adv, int adv_len)
{
    bool     blocked = false, match = false;
    uint16_t matched = 0;
    int      best    = 0;

    for (int pos = 0; pos + 1 < adv_len; ) {
        uint8_t len = adv[pos];
        if (len == 0) break;
        if (pos + 1 + len > adv_len) break;
        uint8_t        type = adv[pos + 1];
        const uint8_t *d    = adv + pos + 2;
        int            dlen = (int)len - 1;

        if (type == 0xFF && dlen >= 2) {                 // manufacturer company id
            uint16_t id = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
            if (is_blocked(id)) blocked = true;
            else if (is_meta(id)) { match = true; if (meta_prio(id) > best) { best = meta_prio(id); matched = id; } }
        } else if ((type == 0x02 || type == 0x03) && dlen >= 2) {  // 16-bit svc UUIDs
            for (int k = 0; k + 1 < dlen; k += 2) {
                uint16_t id = (uint16_t)d[k] | ((uint16_t)d[k + 1] << 8);
                if (is_blocked(id)) blocked = true;
                else if (is_meta(id)) { match = true; if (meta_prio(id) > best) { best = meta_prio(id); matched = id; } }
            }
        } else if (type == 0x16 && dlen >= 2) {          // service data (16-bit uuid)
            uint16_t id = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
            if (is_blocked(id)) blocked = true;
            else if (is_meta(id)) { match = true; if (meta_prio(id) > best) { best = meta_prio(id); matched = id; } }
        }
        pos += 1 + len;
    }

    if (blocked || !match) return false;      // a blocked id wins → not glasses
    fold(mac6, rssi, matched, millis());
    return true;
}

// ─── BLE consumer (standalone tile) ──────────────────────────────────────────
static void on_scan_result(esp_ble_gap_cb_param_t *param)
{
    auto &res = param->scan_rst;
    int total = (int)res.adv_data_len + (int)res.scan_rsp_len;
    meta_glasses_check(res.bda, (int8_t)res.rssi, res.ble_adv, total);
}

bool meta_glasses_start()
{
    if (s_running) return true;
    meta_glasses_reset();
    if (!ble_scan_add(on_scan_result)) return false;
    s_running = true;
    return true;
}

void meta_glasses_stop()
{
    if (!s_running) return;
    ble_scan_remove(on_scan_result);
    s_running = false;
}

bool meta_glasses_is_running() { return s_running; }

// ─── read API ────────────────────────────────────────────────────────────────
int meta_glasses_count() { return s_count; }

int meta_glasses_get(MetaHit *out, int max)
{
    // Snapshot first (the BLE task may be folding into s_hits concurrently), then
    // sort the copy — never mutate the store from the reader.
    MetaHit tmp[META_MAX];
    int c = s_count; if (c > META_MAX) c = META_MAX;
    for (int i = 0; i < c; i++) tmp[i] = s_hits[i];
    for (int i = 0; i < c; i++)
        for (int j = i + 1; j < c; j++)
            if (tmp[j].rssi > tmp[i].rssi) { MetaHit t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
    int n = c < max ? c : max;
    for (int i = 0; i < n; i++) out[i] = tmp[i];
    return n;
}

void meta_glasses_reset() { s_count = 0; }
