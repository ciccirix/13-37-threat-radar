#include "deauther_screen.h"
#include "deauther.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <string.h>

// Defined in tools_screen.cpp
void tools_screen_show();

enum DState {
    DST_IDLE,       // nothing scanned yet
    DST_SCANNING,   // async survey running
    DST_LIST,       // AP list shown, tap to target
    DST_RUNNING,    // deauth flood in progress
};

struct DeAp {
    char    ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int32_t rssi;
};

static lv_obj_t *screen;
static lv_obj_t *status_label;
static lv_obj_t *list_box;
static lv_obj_t *btn_scan, *btn_scan_lbl;
static lv_obj_t *btn_atk,  *btn_atk_lbl;

static DState  s_state  = DST_IDLE;
static DeAp    s_aps[24];
static int     s_ap_count = 0;
static int     s_target   = -1;

// ---- list rendering ---------------------------------------------------------

static void on_ap_clicked(lv_event_t *e);

static void show_aps()
{
    lv_obj_clean(list_box);
    if (s_ap_count == 0) {
        lv_obj_t *l = lv_label_create(list_box);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_make(0x00, 0x66, 0x00), LV_PART_MAIN);
        lv_label_set_text(l, "No APs found");
        lv_obj_add_flag(l, LV_OBJ_FLAG_FLOATING);
        lv_obj_center(l);
        return;
    }
    for (int i = 0; i < s_ap_count; i++) {
        bool sel = (i == s_target);
        lv_obj_t *card = lv_obj_create(list_box);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card,
            sel ? lv_color_make(0x3a, 0x10, 0x10) : lv_color_make(0x16, 0x16, 0x16),
            LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card,
            sel ? lv_color_make(0xcc, 0x33, 0x33) : lv_color_make(0x33, 0x33, 0x33),
            LV_PART_MAIN);
        lv_obj_set_style_border_width(card, sel ? 2 : 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(card, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_ap_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l1 = lv_label_create(card);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(l1, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
        lv_label_set_text(l1, s_aps[i].ssid[0] ? s_aps[i].ssid : "(hidden)");

        char det[80];
        snprintf(det, sizeof(det),
                 "%02X:%02X:%02X:%02X:%02X:%02X  ch %d  %d dBm",
                 s_aps[i].bssid[0], s_aps[i].bssid[1], s_aps[i].bssid[2],
                 s_aps[i].bssid[3], s_aps[i].bssid[4], s_aps[i].bssid[5],
                 s_aps[i].channel, (int)s_aps[i].rssi);
        lv_obj_t *l2 = lv_label_create(card);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l2, lv_color_make(0x00, 0x99, 0x00), LV_PART_MAIN);
        lv_label_set_text(l2, det);
    }
}

// ---- status / buttons -------------------------------------------------------

static void update_status()
{
    char buf[80];
    const char *txt = buf;
    lv_color_t  col = lv_color_make(0x88, 0x88, 0x88);

    switch (s_state) {
    case DST_IDLE:
        txt = "Tap SCAN to find nearby APs";
        break;
    case DST_SCANNING:
        txt = "Scanning...";
        col = lv_color_make(0xFF, 0xCC, 0x00);
        break;
    case DST_LIST:
        if (s_target >= 0) {
            snprintf(buf, sizeof(buf), "Target: %s  (ch %d)",
                     s_aps[s_target].ssid[0] ? s_aps[s_target].ssid : "(hidden)",
                     s_aps[s_target].channel);
        } else {
            snprintf(buf, sizeof(buf), "%d APs - tap one to target", s_ap_count);
        }
        break;
    case DST_RUNNING:
        snprintf(buf, sizeof(buf), "Deauthing %s  -  %lu frames",
                 s_aps[s_target].ssid[0] ? s_aps[s_target].ssid : "(hidden)",
                 (unsigned long)deauther_frames_sent());
        col = lv_color_make(0xFF, 0x44, 0x44);
        break;
    }
    lv_label_set_text(status_label, txt);
    lv_obj_set_style_text_color(status_label, col, LV_PART_MAIN);
}

static void update_buttons()
{
    if (s_state == DST_RUNNING) {
        lv_obj_add_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(btn_atk_lbl, "STOP");
        lv_obj_set_style_bg_color(btn_atk, lv_color_make(0x88, 0x22, 0x22), LV_PART_MAIN);
        lv_obj_align(btn_atk, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    lv_obj_clear_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);
    const char *sl = (s_state == DST_SCANNING) ? "SCANNING..." :
                     (s_target >= 0)           ? "RESCAN"      : "SCAN";
    lv_label_set_text(btn_scan_lbl, sl);
    lv_obj_set_style_bg_color(btn_scan, lv_color_black(), LV_PART_MAIN);

    if (s_target >= 0 && s_state != DST_SCANNING) {
        lv_obj_clear_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(btn_atk_lbl, "START");
        lv_obj_set_style_bg_color(btn_atk, lv_color_make(0xCC, 0x33, 0x33), LV_PART_MAIN);
        lv_obj_align(btn_scan, LV_ALIGN_LEFT_MID,  6, 0);
        lv_obj_align(btn_atk,  LV_ALIGN_RIGHT_MID, -6, 0);
    } else {
        lv_obj_add_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(btn_scan, LV_ALIGN_CENTER, 0, 0);
    }
}

// ---- flow -------------------------------------------------------------------

static void start_scan()
{
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.scanNetworks(true);   // async
    s_state = DST_SCANNING;
}

static void on_ap_clicked(lv_event_t *e)
{
    if (s_state == DST_RUNNING) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ap_count) return;
    s_target = idx;
    show_aps();
    update_status();
    update_buttons();
}

static void on_scan_btn(lv_event_t *)
{
    if (s_state == DST_RUNNING) return;
    start_scan();
    update_status();
    update_buttons();
}

static void on_attack_btn(lv_event_t *)
{
    if (s_state == DST_RUNNING) {
        deauther_stop();
        s_state = DST_LIST;
    } else if (s_target >= 0) {
        deauther_start(s_aps[s_target].bssid, s_aps[s_target].channel);
        s_state = DST_RUNNING;
    }
    update_status();
    update_buttons();
}

// ---- periodic refresh -------------------------------------------------------

static void on_refresh(lv_timer_t *)
{
    if (lv_screen_active() != screen) return;

    if (s_state == DST_SCANNING) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            s_ap_count = n > 24 ? 24 : n;
            for (int i = 0; i < s_ap_count; i++) {
                strncpy(s_aps[i].ssid, WiFi.SSID(i).c_str(), 32);
                s_aps[i].ssid[32] = '\0';
                s_aps[i].rssi    = WiFi.RSSI(i);
                s_aps[i].channel = WiFi.channel(i);
                const uint8_t *b = WiFi.BSSID(i);
                if (b) memcpy(s_aps[i].bssid, b, 6);
            }
            WiFi.scanDelete();
            if (s_target >= s_ap_count) s_target = -1;
            s_state = DST_LIST;
            show_aps();
        } else if (n == WIFI_SCAN_FAILED) {
            s_state = (s_ap_count > 0) ? DST_LIST : DST_IDLE;
        }
        update_status();
        update_buttons();
    } else if (s_state == DST_RUNNING) {
        update_status();   // refresh the frame counter
    }
}

// ---- events / layout --------------------------------------------------------

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        deauther_screen_stop();
        tools_screen_show();
    }
}

static lv_obj_t *make_button(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                             lv_color_t bg, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    // White: this button's bg is a caller-supplied status color (START/STOP),
    // so the label must stay readable against whichever one is showing.
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
}

void deauther_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "Deauth");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Responsible-use reminder — this tool disrupts networks.
    lv_obj_t *warn = lv_label_create(screen);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(warn, lv_color_make(0x88, 0x55, 0x55), LV_PART_MAIN);
    lv_label_set_text(warn, "Only on networks you're authorised to test");
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 58);

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x00, 0x88, 0x00), LV_PART_MAIN);
    lv_label_set_text(status_label, "Tap SCAN to find nearby APs");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 82);

    lv_obj_t *btn_row = lv_obj_create(screen);
    lv_obj_set_size(btn_row, 404, 50);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn_row, LV_ALIGN_TOP_MID, 0, 108);

    btn_scan = make_button(btn_row, 196, 48, lv_color_make(0x00, 0x88, 0xCC), &btn_scan_lbl);
    lv_obj_align(btn_scan, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_scan, on_scan_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_scan_lbl, "SCAN");

    btn_atk = make_button(btn_row, 196, 48, lv_color_make(0xCC, 0x33, 0x33), &btn_atk_lbl);
    lv_obj_align(btn_atk, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_event_cb(btn_atk, on_attack_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_atk_lbl, "START");
    lv_obj_add_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);

    list_box = lv_obj_create(screen);
    lv_obj_set_size(list_box, 404, 322);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 168);
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
    lv_timer_create(on_refresh, 500, NULL);
}

void deauther_screen_show()
{
    if (s_ap_count > 0) {
        s_state = DST_LIST;
        show_aps();
    } else {
        s_state = DST_IDLE;
        lv_obj_clean(list_box);
    }
    update_status();
    update_buttons();
    lv_scr_load(screen);
}

void deauther_screen_stop()
{
    deauther_stop();
    s_state = (s_ap_count > 0) ? DST_LIST : DST_IDLE;
}

bool deauther_screen_is_active()
{
    return lv_screen_active() == screen;
}
