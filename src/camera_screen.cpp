#include "camera_screen.h"
#include "cam_audit.h"
#include "flock.h"
#include "wifi_beacon_manager.h"
#include "ble_scan_manager.h"
#include "esp_gap_ble_api.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

// Defined in tools_screen.cpp
void tools_screen_show();

// The screen has two mutually-exclusive modes because the radio can't do both
// at once: OFF-AIR uses promiscuous WiFi (which drops any association), LAN
// AUDIT needs the watch associated to a network to probe it over IP.
enum CamMode { CM_RF, CM_LAN };

// ---- OFF-AIR (passive RF) detections ---------------------------------------

struct CamHit {                 // handed from WiFi/BLE task to main task
    uint8_t     mac[6];
    int8_t      rssi;
    uint8_t     channel;        // 0 for BLE
    const char *vendor;
    char        name[33];
    char        source;         // 'W' / 'L'
};
struct CamDev {                 // coalesced view
    uint8_t     mac[6];
    int8_t      rssi;
    uint8_t     channel;
    const char *vendor;
    char        name[33];
    char        source;
    uint32_t    count;
};

#define CAM_MAX   40
#define CAM_QLEN  24

static lv_obj_t *screen;
static lv_obj_t *status_label;
static lv_obj_t *list_box;
static lv_obj_t *btn_rf,  *btn_rf_lbl;
static lv_obj_t *btn_lan, *btn_lan_lbl;

static CamMode       s_mode = CM_RF;
static bool          s_rf_on = false;

static CamDev        s_dev[CAM_MAX];
static int           s_dev_count = 0;
static QueueHandle_t s_queue = nullptr;
static bool          s_dirty  = false;

static CamFinding    s_find[CAM_MAX];
static int           s_find_count = 0;

// ---- OFF-AIR capture callbacks (WiFi / BLE task context) --------------------

static void enqueue(const uint8_t *mac, int8_t rssi, uint8_t channel,
                    const char *vendor, const char *name, char source)
{
    if (!s_queue) return;
    CamHit h = {};
    memcpy(h.mac, mac, 6);
    h.rssi = rssi; h.channel = channel; h.vendor = vendor; h.source = source;
    if (name && name[0]) { strncpy(h.name, name, 32); h.name[32] = '\0'; }
    xQueueSend(s_queue, &h, 0);
}

static void cam_beacon_cb(const WifiBeacon *b)
{
    const char *v = flock_classify(b->bssid, b->ssid);
    if (v) enqueue(b->bssid, b->rssi, b->channel, v, b->ssid, 'W');
}

// Kept for the scanner redesign (BLE will return time-sliced with WiFi); unused
// for now since OFF-AIR runs WiFi-only to avoid the coexistence crash.
static void __attribute__((unused)) cam_ble_cb(esp_ble_gap_cb_param_t *param)
{
    auto &res = param->scan_rst;
    char name[33] = {};
    const uint8_t *adv = res.ble_adv;
    int total = (int)res.adv_data_len + (int)res.scan_rsp_len;
    for (int pos = 0; pos < total; ) {
        uint8_t seg = adv[pos];
        if (seg == 0) break;
        if (pos + 1 + (int)seg > total) break;
        uint8_t type = adv[pos + 1];
        int dl = (int)seg - 1;
        if ((type == 0x08 || type == 0x09) && dl > 0 && dl <= 32) {
            if (type == 0x09 || name[0] == '\0') { memcpy(name, adv + pos + 2, dl); name[dl] = '\0'; }
        }
        pos += 1 + (int)seg;
    }
    const char *v = flock_classify(res.bda, name[0] ? name : nullptr);
    if (v) enqueue(res.bda, (int8_t)res.rssi, 0, v, name, 'L');
}

static void rf_on()
{
    if (s_rf_on) return;
    if (!s_queue) s_queue = xQueueCreate(CAM_QLEN, sizeof(CamHit));
    s_dev_count = 0;
    s_dirty = true;
    // WiFi promiscuous ONLY. Running WiFi-promiscuous + BLE-scan at the same time
    // hard-crashes/reboots this S3 build (radio coexistence) — it was the only
    // tile that started both at once. Surveillance cams (Ring/Flock/Axon…) are
    // WiFi devices, so WiFi covers the useful case; the BLE half will come back
    // time-sliced (alternate WiFi/BLE) in the scanner redesign.
    wifi_beacon_add(cam_beacon_cb);
    s_rf_on = true;
}

static void rf_off()
{
    if (!s_rf_on) return;
    wifi_beacon_remove(cam_beacon_cb);
    s_rf_on = false;
}

static void upsert(const CamHit *h)
{
    for (int i = 0; i < s_dev_count; i++) {
        if (memcmp(s_dev[i].mac, h->mac, 6) == 0) {
            s_dev[i].rssi = h->rssi;
            if (h->channel) s_dev[i].channel = h->channel;
            if (h->name[0]) { strncpy(s_dev[i].name, h->name, 32); s_dev[i].name[32] = '\0'; }
            s_dev[i].count++;
            s_dirty = true;
            return;
        }
    }
    if (s_dev_count >= CAM_MAX) return;
    CamDev &d = s_dev[s_dev_count++];
    memset(&d, 0, sizeof(d));
    memcpy(d.mac, h->mac, 6);
    d.rssi = h->rssi; d.channel = h->channel; d.vendor = h->vendor; d.source = h->source;
    strncpy(d.name, h->name, 32); d.name[32] = '\0';
    d.count = 1;
    s_dirty = true;
}

// ---- list rendering helpers -------------------------------------------------

static lv_obj_t *make_card(lv_color_t border)
{
    lv_obj_t *card = lv_obj_create(list_box);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, border, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

static void add_text(lv_obj_t *card, const char *txt, const lv_font_t *font, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(card);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, col, LV_PART_MAIN);
    lv_label_set_text(l, txt);
}

static void placeholder(const char *txt)
{
    lv_obj_t *l = lv_label_create(list_box);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_make(0x00, 0x66, 0x00), LV_PART_MAIN);
    lv_label_set_text(l, txt);
    lv_obj_add_flag(l, LV_OBJ_FLAG_FLOATING);
    lv_obj_center(l);
}

// ---- OFF-AIR list -----------------------------------------------------------

static void rf_repaint()
{
    lv_obj_clean(list_box);
    if (s_dev_count == 0) { placeholder("Scanning... no cameras yet"); return; }

    int order[CAM_MAX];
    for (int i = 0; i < s_dev_count; i++) order[i] = i;
    for (int i = 0; i < s_dev_count - 1; i++)
        for (int j = i + 1; j < s_dev_count; j++)
            if (s_dev[order[j]].rssi > s_dev[order[i]].rssi) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }

    for (int k = 0; k < s_dev_count; k++) {
        CamDev &d = s_dev[order[k]];
        lv_obj_t *card = make_card(lv_color_make(0x33, 0x33, 0x33));
        char l1[72];
        if (d.name[0]) snprintf(l1, sizeof(l1), "%s - %s", d.vendor, d.name);
        else           snprintf(l1, sizeof(l1), "%s", d.vendor);
        add_text(card, l1, &lv_font_montserrat_20, lv_color_white());
        char l2[88];
        if (d.source == 'W')
            snprintf(l2, sizeof(l2), "%02X:%02X:%02X:%02X:%02X:%02X  ch %d  %d dBm  x%u",
                     d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
                     d.channel, (int)d.rssi, (unsigned)d.count);
        else
            snprintf(l2, sizeof(l2), "%02X:%02X:%02X:%02X:%02X:%02X  BLE  %d dBm  x%u",
                     d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
                     (int)d.rssi, (unsigned)d.count);
        add_text(card, l2, &lv_font_montserrat_14, lv_color_make(0x99, 0x99, 0x99));
    }
}

// ---- LAN audit list ---------------------------------------------------------

static lv_color_t level_color(uint8_t lvl)
{
    switch (lvl) {
        case CL_DEFAULT_CREDS: return lv_color_make(0xFF, 0x22, 0x88);  // owned
        case CL_RTSP_OPEN: return lv_color_make(0xFF, 0x44, 0x44);
        case CL_RTSP_AUTH: return lv_color_make(0xFF, 0xCC, 0x00);
        case CL_HTTP:      return lv_color_make(0x44, 0xBB, 0xFF);
        default:           return lv_color_make(0xAA, 0xAA, 0xAA);
    }
}

static void lan_repaint()
{
    lv_obj_clean(list_box);
    if (s_find_count == 0) {
        placeholder(cam_audit_is_running() ? "Scanning LAN..." : "No cameras found");
        return;
    }
    int order[CAM_MAX];
    for (int i = 0; i < s_find_count; i++) order[i] = i;
    for (int i = 0; i < s_find_count - 1; i++)
        for (int j = i + 1; j < s_find_count; j++)
            if (s_find[order[j]].level > s_find[order[i]].level) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }

    for (int k = 0; k < s_find_count; k++) {
        CamFinding &f = s_find[order[k]];
        lv_color_t col = level_color(f.level);
        lv_obj_t *card = make_card(f.level >= CL_RTSP_OPEN
                                   ? lv_color_make(0x66, 0x22, 0x22)
                                   : lv_color_make(0x33, 0x33, 0x33));
        char l1[80];
        snprintf(l1, sizeof(l1), "%s  %u.%u.%u.%u",
                 f.vendor ? f.vendor : "Camera",
                 (f.ip >> 24) & 0xFF, (f.ip >> 16) & 0xFF, (f.ip >> 8) & 0xFF, f.ip & 0xFF);
        add_text(card, l1, &lv_font_montserrat_20, col);

        char l2[88];
        if (f.has_mac)
            snprintf(l2, sizeof(l2), "%s   %02X:%02X:%02X:%02X:%02X:%02X",
                     f.note, f.mac[0], f.mac[1], f.mac[2], f.mac[3], f.mac[4], f.mac[5]);
        else
            snprintf(l2, sizeof(l2), "%s", f.note);
        add_text(card, l2, &lv_font_montserrat_14, lv_color_make(0x99, 0x99, 0x99));
    }
}

// ---- status / buttons -------------------------------------------------------

static void update_buttons()
{
    bool rf = (s_mode == CM_RF);
    lv_obj_set_style_bg_color(btn_rf,
        rf ? lv_color_make(0x00, 0x88, 0xCC) : lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_lan,
        rf ? lv_color_make(0x22, 0x22, 0x22) : lv_color_make(0xCC, 0x66, 0x00), LV_PART_MAIN);
}

static void update_status()
{
    char buf[80];
    if (s_mode == CM_RF) {
        if (s_dev_count == 0) {
            lv_label_set_text(status_label, "Off-air scan: WiFi + BLE");
            lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
        } else {
            snprintf(buf, sizeof(buf), "%d device%s off-air", s_dev_count, s_dev_count == 1 ? "" : "s");
            lv_label_set_text(status_label, buf);
            lv_obj_set_style_text_color(status_label, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN);
        }
        return;
    }

    // LAN mode
    if (WiFi.status() != WL_CONNECTED) {
        lv_label_set_text(status_label, "Not connected - join WiFi (WiFi tile), then LAN AUDIT");
        lv_obj_set_style_text_color(status_label, lv_color_make(0x00, 0x88, 0x00), LV_PART_MAIN);
        return;
    }
    int exposed = 0;
    for (int i = 0; i < s_find_count; i++) if (s_find[i].level >= CL_RTSP_OPEN) exposed++;

    switch (cam_audit_phase()) {
    case CA_SWEEP:
        lv_label_set_text(status_label, "Sweeping LAN for hosts...");
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
        break;
    case CA_PROBE:
        snprintf(buf, sizeof(buf), "Probing %d/%d hosts   %d cam%s",
                 cam_audit_scanned(), cam_audit_total(),
                 s_find_count, s_find_count == 1 ? "" : "s");
        lv_label_set_text(status_label, buf);
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
        break;
    default:  // IDLE / DONE
        if (cam_audit_phase() == CA_DONE) {
            snprintf(buf, sizeof(buf), "Done - %d cam%s, %d exposed",
                     s_find_count, s_find_count == 1 ? "" : "s", exposed);
            lv_label_set_text(status_label, buf);
            lv_obj_set_style_text_color(status_label,
                exposed ? lv_color_make(0xFF, 0x44, 0x44) : lv_color_make(0x00, 0xCC, 0x66),
                LV_PART_MAIN);
        } else {
            snprintf(buf, sizeof(buf), "Connected to %s - tap LAN AUDIT", WiFi.SSID().c_str());
            lv_label_set_text(status_label, buf);
            lv_obj_set_style_text_color(status_label, lv_color_make(0x00, 0x88, 0x00), LV_PART_MAIN);
        }
        break;
    }
}

// ---- mode switches ----------------------------------------------------------

static void enter_rf()
{
    cam_audit_stop();
    s_mode = CM_RF;
    s_find_count = 0;
    rf_on();
    lv_obj_clean(list_box);
    placeholder("Scanning... no cameras yet");
    update_buttons();
    update_status();
}

static void enter_lan(bool run_audit)
{
    rf_off();
    s_mode = CM_LAN;
    s_find_count = 0;
    lv_obj_clean(list_box);
    if (run_audit && WiFi.status() == WL_CONNECTED) {
        cam_audit_start();
        placeholder("Sweeping LAN...");
    } else if (WiFi.status() == WL_CONNECTED) {
        placeholder("Tap LAN AUDIT to scan this network");
    } else {
        placeholder("Not connected - join WiFi first");
    }
    update_buttons();
    update_status();
}

// ---- events -----------------------------------------------------------------

static void on_rf_btn(lv_event_t *)  { enter_rf(); }
static void on_lan_btn(lv_event_t *) { enter_lan(true); }   // tapping always (re)runs

static void on_timer(lv_timer_t *)
{
    if (lv_screen_active() != screen) return;

    if (s_mode == CM_RF) {
        CamHit h;
        int drained = 0;
        while (s_queue && drained < 32 && xQueueReceive(s_queue, &h, 0) == pdTRUE) {
            upsert(&h); drained++;
        }
        if (s_dirty) { rf_repaint(); update_status(); s_dirty = false; }
    } else {
        CamFinding f;
        bool any = false;
        while (s_find_count < CAM_MAX && cam_audit_next_finding(&f)) {
            bool dup = false;
            for (int i = 0; i < s_find_count; i++) if (s_find[i].ip == f.ip) { dup = true; break; }
            if (!dup) { s_find[s_find_count++] = f; any = true; }
        }
        if (any) lan_repaint();
        update_status();
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        camera_screen_stop();
        tools_screen_show();
    }
}

// ---- layout -----------------------------------------------------------------

static lv_obj_t *make_button(lv_obj_t *parent, lv_coord_t w, lv_color_t bg, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, w, 42);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    // White: this button's bg is a caller-supplied color, so the label
    // stays readable regardless of which one gets passed in.
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
}

void camera_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "Cameras");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_set_size(row, 404, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 62);

    btn_rf = make_button(row, 196, lv_color_make(0x00, 0x88, 0xCC), &btn_rf_lbl);
    lv_obj_align(btn_rf, LV_ALIGN_LEFT_MID, 6, 0);
    lv_label_set_text(btn_rf_lbl, "OFF-AIR");
    lv_obj_add_event_cb(btn_rf, on_rf_btn, LV_EVENT_CLICKED, NULL);

    btn_lan = make_button(row, 196, lv_color_make(0x22, 0x22, 0x22), &btn_lan_lbl);
    lv_obj_align(btn_lan, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_label_set_text(btn_lan_lbl, "LAN AUDIT");
    lv_obj_add_event_cb(btn_lan, on_lan_btn, LV_EVENT_CLICKED, NULL);

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x00, 0x88, 0x00), LV_PART_MAIN);
    lv_label_set_text(status_label, "Off-air scan: WiFi + BLE");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 116);

    list_box = lv_obj_create(screen);
    lv_obj_set_size(list_box, 404, 344);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 144);
    lv_obj_set_style_bg_color(list_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(list_box, lv_color_make(0x00, 0x33, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list_box, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);

    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_timer, 800, NULL);
}

void camera_screen_show()
{
    if (!s_queue) s_queue = xQueueCreate(CAM_QLEN, sizeof(CamHit));
    lv_scr_load(screen);
    // Already on a network? Start in LAN mode (idle) so we don't tear down the
    // association with promiscuous. Otherwise fall back to the off-air scan.
    if (WiFi.status() == WL_CONNECTED) enter_lan(false);
    else                              enter_rf();
}

void camera_screen_stop()
{
    rf_off();
    cam_audit_stop();
}

bool camera_screen_is_active()
{
    return lv_screen_active() == screen;
}
