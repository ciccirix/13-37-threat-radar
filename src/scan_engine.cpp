#include "scan_engine.h"
#include "scan_radio.h"
#include "wifi_beacon_manager.h"
#include "ble_scan_manager.h"
#include "flock.h"
#include "airtag.h"
#include "flipper.h"
#include "skimmer.h"
#include "evil_twin.h"
#include "counter_tail.h"
#include <Arduino.h>
#include <string.h>
#include "esp_gap_ble_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ─── tunables ────────────────────────────────────────────────────────────────
#define SCAN_MAX_DEV   128     // live store capacity (unique MACs)
#define SCAN_QUEUE_LEN  64     // radio-task -> main-task hand-off depth
#define SCAN_DRAIN_MAX  48     // hits folded in per tick (bounds main-task cost)
#define SCAN_STALE_MS   60000  // a device is "active" if seen within this window

// A compact sighting pushed from the radio callbacks. No pointers — copied by
// value through the queue, so it stays valid after the callback returns.
struct ScanHit {
    uint8_t mac[6];
    int8_t  rssi;
    uint8_t kind;      // ScanKind
    uint8_t category;  // TrCategory or SCAN_CAT_NONE
    char    name[24];
};

static QueueHandle_t s_queue = nullptr;
static ScanDev       s_dev[SCAN_MAX_DEV];
static int           s_dev_n = 0;
static uint32_t      s_total = 0;
static bool          s_running = false;

// Higher = surfaced nearer the top of the list. Trackers that physically ride
// with you outrank fixed-installation flags.
static uint8_t cat_priority(uint8_t cat)
{
    switch (cat) {
        case TR_CAT_AIRTAG:   return 6;
        case TR_CAT_SKIMMER:  return 5;
        case TR_CAT_FLIPPER:  return 4;
        case TR_CAT_EVILTWIN: return 3;
        case TR_CAT_FLOCK:    return 2;
        default:              return 0;
    }
}

// ─── radio callbacks (run on the radio task — enqueue only) ──────────────────

static void push_hit(const ScanHit *h)
{
    if (s_queue) xQueueSend(s_queue, h, 0);   // drop on overflow: it's telemetry
}

static void wifi_cb(const WifiBeacon *b)
{
    ScanHit h = {};
    memcpy(h.mac, b->bssid, 6);
    h.rssi = b->rssi;
    h.kind = SCAN_KIND_WIFI;
    h.category = SCAN_CAT_NONE;

    // Same detector fan-out the wardriver runs (drives SD logs + Threat Radar).
    if (evil_twin_check(b->bssid, b->ssid, b->auth, b->rssi, b->channel))
        h.category = TR_CAT_EVILTWIN;
    if (flock_check(b->bssid, b->rssi, b->ssid, 'W'))
        h.category = TR_CAT_FLOCK;
    counter_tail_observe_wifi(b->bssid, b->rssi);

    strncpy(h.name, b->ssid, sizeof(h.name) - 1);
    push_hit(&h);
}

static void ble_cb(esp_ble_gap_cb_param_t *param)
{
    auto &res = param->scan_rst;
    ScanHit h = {};
    memcpy(h.mac, res.bda, 6);
    h.rssi = (int8_t)res.rssi;
    h.kind = SCAN_KIND_BLE;
    h.category = SCAN_CAT_NONE;

    // Pull the local name out of the advertisement (0x08 shortened / 0x09 full),
    // same walk the wardriver uses.
    const uint8_t *adv       = res.ble_adv;
    int            total_len = (int)res.adv_data_len + (int)res.scan_rsp_len;
    for (int pos = 0; pos < total_len; ) {
        uint8_t seg_len = adv[pos];
        if (seg_len == 0) break;
        if (pos + 1 + (int)seg_len > total_len) break;
        uint8_t        ad_type     = adv[pos + 1];
        const uint8_t *ad_data     = adv + pos + 2;
        int            ad_data_len  = (int)seg_len - 1;
        if ((ad_type == 0x08 || ad_type == 0x09) && ad_data_len > 0) {
            int n = ad_data_len < (int)sizeof(h.name) - 1
                        ? ad_data_len : (int)sizeof(h.name) - 1;
            if (ad_type == 0x09 || h.name[0] == '\0') {
                memcpy(h.name, ad_data, n);
                h.name[n] = '\0';
            }
        }
        pos += 1 + (int)seg_len;
    }

    if (flock_check(res.bda, h.rssi, h.name, 'L'))
        h.category = TR_CAT_FLOCK;
    if (airtag_check(res.bda, h.rssi, res.ble_addr_type, res.ble_adv, total_len))
        h.category = TR_CAT_AIRTAG;
    if (flipper_check(res.bda, h.rssi, res.ble_addr_type, res.ble_adv, total_len))
        h.category = TR_CAT_FLIPPER;
    if (skimmer_check(res.bda, h.rssi, res.ble_addr_type, res.ble_adv, total_len))
        h.category = TR_CAT_SKIMMER;
    counter_tail_observe_ble(res.bda, h.rssi);

    push_hit(&h);
}

// ─── store maintenance (main task) ───────────────────────────────────────────

static ScanDev *find_dev(const uint8_t *mac)
{
    for (int i = 0; i < s_dev_n; i++)
        if (memcmp(s_dev[i].mac, mac, 6) == 0) return &s_dev[i];
    return nullptr;
}

// Pick an entry to overwrite when the store is full: prefer the oldest plain
// (unclassified) device so a detector flag is never evicted by ambient noise.
static ScanDev *evict_victim()
{
    ScanDev *plain = nullptr, *any = nullptr;
    for (int i = 0; i < s_dev_n; i++) {
        if (!any || s_dev[i].last_ms < any->last_ms) any = &s_dev[i];
        if (s_dev[i].category == SCAN_CAT_NONE &&
            (!plain || s_dev[i].last_ms < plain->last_ms)) plain = &s_dev[i];
    }
    return plain ? plain : any;
}

static void fold_hit(const ScanHit *h, uint32_t now)
{
    s_total++;
    ScanDev *d = find_dev(h->mac);
    if (!d) {
        if (s_dev_n < SCAN_MAX_DEV) d = &s_dev[s_dev_n++];
        else                        d = evict_victim();
        if (!d) return;
        memset(d, 0, sizeof(*d));
        memcpy(d->mac, h->mac, 6);
        d->best_rssi = h->rssi;
        d->category  = SCAN_CAT_NONE;
    }
    d->kind    = h->kind;
    d->rssi    = h->rssi;
    d->last_ms = now;
    if (h->rssi > d->best_rssi) d->best_rssi = h->rssi;
    if (d->hits < 0xFFFF)       d->hits++;
    // A detector flag only ever sticks or upgrades to a higher-priority one.
    if (h->category != SCAN_CAT_NONE &&
        (d->category == SCAN_CAT_NONE ||
         cat_priority(h->category) > cat_priority(d->category)))
        d->category = h->category;
    // Keep a name once we have a good one; don't clobber it with a later blank.
    if (h->name[0] && (d->name[0] == '\0'))
        strncpy(d->name, h->name, sizeof(d->name) - 1);
}

// ─── public API ──────────────────────────────────────────────────────────────

void scan_engine_start()
{
    if (s_running) return;
    if (!s_queue) s_queue = xQueueCreate(SCAN_QUEUE_LEN, sizeof(ScanHit));
    scan_engine_reset();
    scan_radio_start(wifi_cb, ble_cb);
    s_running = true;
}

void scan_engine_stop()
{
    if (!s_running) return;
    scan_radio_stop();
    s_running = false;
    if (s_queue) xQueueReset(s_queue);
}

bool scan_engine_running() { return s_running; }

void scan_engine_reset()
{
    s_dev_n = 0;
    s_total = 0;
    if (s_queue) xQueueReset(s_queue);
}

void scan_engine_tick()
{
    if (!s_queue) return;
    uint32_t now = millis();
    ScanHit h;
    int budget = SCAN_DRAIN_MAX;
    while (budget-- > 0 && xQueueReceive(s_queue, &h, 0) == pdTRUE)
        fold_hit(&h, now);
}

void scan_engine_stats(ScanStats *out)
{
    memset(out, 0, sizeof(*out));
    out->total_seen = s_total;
    uint32_t now = millis();
    for (int i = 0; i < s_dev_n; i++) {
        if (now - s_dev[i].last_ms > SCAN_STALE_MS) continue;   // stale
        if (s_dev[i].kind == SCAN_KIND_WIFI) out->wifi++;
        else                                 out->ble++;
        if (s_dev[i].category != SCAN_CAT_NONE) {
            out->threats++;
            if (s_dev[i].category < TR_CAT_COUNT)
                out->per_cat[s_dev[i].category]++;
        }
    }
}

// Sort key: active first, then category priority, then strongest signal.
static bool dev_better(const ScanDev *a, const ScanDev *b, uint32_t now)
{
    bool aa = (now - a->last_ms) <= SCAN_STALE_MS;
    bool ba = (now - b->last_ms) <= SCAN_STALE_MS;
    if (aa != ba) return aa;
    uint8_t pa = cat_priority(a->category), pb = cat_priority(b->category);
    if (pa != pb) return pa > pb;
    return a->best_rssi > b->best_rssi;
}

int scan_engine_get_devices(ScanDev *out, int max)
{
    uint32_t now = millis();
    int n = 0;
    // Collect only flagged devices — plain ones live in the counters, not the
    // list. Simple selection sort against the output (max is small, ~16).
    for (int i = 0; i < s_dev_n; i++) {
        if (s_dev[i].category == SCAN_CAT_NONE) continue;
        if (n < max) {
            out[n++] = s_dev[i];
        } else {
            // Replace the current worst if this one is better.
            int worst = 0;
            for (int j = 1; j < n; j++)
                if (dev_better(&out[worst], &out[j], now)) worst = j;
            if (dev_better(&s_dev[i], &out[worst], now)) out[worst] = s_dev[i];
        }
    }
    // Order the selected set.
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (dev_better(&out[j], &out[i], now)) {
                ScanDev t = out[i]; out[i] = out[j]; out[j] = t;
            }
    return n;
}

bool scan_engine_wifi_phase() { return scan_radio_wifi_phase(); }
