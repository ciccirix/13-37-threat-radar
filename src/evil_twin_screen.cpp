#include "evil_twin_screen.h"
#include "evil_portal.h"
#include "tools_screen.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

enum EState {
    EST_IDLE,       // nothing scanned yet
    EST_SCANNING,   // async survey running
    EST_LIST,       // AP list shown, tap to target
    EST_RUNNING,    // twin + captive portal live
};

struct EAp {
    char    ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int32_t rssi;
};

static lv_obj_t *screen;
static lv_obj_t *status_label;
static lv_obj_t *list_box;          // AP list (idle/list states)
static lv_obj_t *live_box;          // live capture panel (running state)
static lv_obj_t *live_ssid, *live_sub, *live_list;
static int       s_creds_shown = -1;   // rebuild the list only on a new capture
static lv_obj_t *btn_scan, *btn_scan_lbl;
static lv_obj_t *btn_atk,  *btn_atk_lbl;

static EState s_state    = EST_IDLE;
static EAp    s_aps[24];
static int    s_ap_count = 0;
static int    s_target   = -1;

// ─── AP list ─────────────────────────────────────────────────────────────────
static void on_ap_clicked(lv_event_t *e);

static void show_aps()
{
    lv_obj_clean(list_box);
    if (s_ap_count == 0) {
        lv_obj_t *l = lv_label_create(list_box);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_make(0x00, 0x66, 0x00), LV_PART_MAIN);
        lv_label_set_text(l, "Nessuna rete - tocca SCAN");
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
            sel ? lv_color_make(0x2a, 0x10, 0x2a) : lv_color_make(0x16, 0x16, 0x16),
            LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card,
            sel ? lv_color_make(0xcc, 0x44, 0xaa) : lv_color_make(0x33, 0x33, 0x33),
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
        lv_obj_set_style_text_color(l1, lv_color_make(0xEE, 0x66, 0xCC), LV_PART_MAIN);
        lv_label_set_text(l1, s_aps[i].ssid[0] ? s_aps[i].ssid : "(hidden)");

        char det[80];
        snprintf(det, sizeof(det), "ch %d  ·  %d dBm  ·  %02X:%02X:%02X",
                 s_aps[i].channel, (int)s_aps[i].rssi,
                 s_aps[i].bssid[3], s_aps[i].bssid[4], s_aps[i].bssid[5]);
        lv_obj_t *l2 = lv_label_create(card);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l2, lv_color_make(0x99, 0x55, 0x88), LV_PART_MAIN);
        lv_label_set_text(l2, det);
    }
}

// ─── live capture panel ──────────────────────────────────────────────────────
// Compact header (twin SSID + client/cred counts) over a scrollable list that
// shows every captured secret live, newest on top.
static void build_live_box()
{
    lv_obj_clean(live_box);
    s_creds_shown = -1;

    live_ssid = lv_label_create(live_box);
    lv_obj_set_style_text_font(live_ssid, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(live_ssid, lv_color_make(0xEE, 0x66, 0xCC), LV_PART_MAIN);
    lv_label_set_long_mode(live_ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_width(live_ssid, 380);
    lv_label_set_text(live_ssid, "");
    lv_obj_align(live_ssid, LV_ALIGN_TOP_LEFT, 2, 0);

    live_sub = lv_label_create(live_box);
    lv_obj_set_style_text_font(live_sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(live_sub, lv_color_make(0x66, 0x99, 0xFF), LV_PART_MAIN);
    lv_label_set_text(live_sub, "");
    lv_obj_align(live_sub, LV_ALIGN_TOP_LEFT, 2, 28);

    // Scrollable list of captures.
    live_list = lv_obj_create(live_box);
    lv_obj_set_size(live_list, 384, 250);
    lv_obj_align(live_list, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_opa(live_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(live_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(live_list, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(live_list, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(live_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(live_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(live_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(live_list, LV_FLEX_FLOW_COLUMN);
}

static void rebuild_cred_list()
{
    lv_obj_clean(live_list);

    EvilCred creds[16];
    int n = evil_portal_get_creds(creds, 16);
    if (n == 0) {
        lv_obj_t *w = lv_label_create(live_list);
        lv_obj_set_style_text_font(w, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(w, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
        lv_label_set_text(w, "In attesa di una vittima...\n\nQuando qualcuno inserisce la\npassword nel portale, appare qui\ne su SD /EvilTwin/creds.txt");
        lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_add_flag(w, LV_OBJ_FLAG_FLOATING);
        lv_obj_center(w);
        return;
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *card = lv_obj_create(live_list);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_make(0x12, 0x10, 0x12), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        // Newest capture gets the bright border.
        lv_obj_set_style_border_color(card,
            i == 0 ? lv_color_make(0x22, 0xDD, 0x66) : lv_color_make(0x33, 0x22, 0x33),
            LV_PART_MAIN);
        lv_obj_set_style_border_width(card, i == 0 ? 2 : 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(card, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

        // The captured secret — the loot, in bright green mono-ish.
        lv_obj_t *sec = lv_label_create(card);
        lv_obj_set_style_text_font(sec, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(sec, lv_color_make(0x33, 0xFF, 0x88), LV_PART_MAIN);
        lv_label_set_long_mode(sec, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sec, 350);
        lv_label_set_text(sec, creds[i].secret[0] ? creds[i].secret : "(vuota)");

        lv_obj_t *meta = lv_label_create(card);
        lv_obj_set_style_text_font(meta, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(meta, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
        lv_label_set_text_fmt(meta, LV_SYMBOL_GPS " %s   ·   %s", creds[i].time, creds[i].ip);
    }
}

static void refresh_live()
{
    lv_label_set_text_fmt(live_ssid, LV_SYMBOL_WARNING "  \"%s\"", evil_portal_ssid());
    lv_label_set_text_fmt(live_sub, LV_SYMBOL_WIFI " %d client   ·   %d credenziali",
                          evil_portal_client_count(), evil_portal_cred_count());
    int c = evil_portal_cred_count();
    if (c != s_creds_shown) {     // only rebuild the list when a new one lands
        s_creds_shown = c;
        rebuild_cred_list();
    }
}

// ─── status / buttons ────────────────────────────────────────────────────────
static void update_status()
{
    char buf[96];
    const char *txt = buf;
    lv_color_t  col = lv_color_make(0x88, 0x88, 0x88);
    switch (s_state) {
    case EST_IDLE:
        txt = "Tocca SCAN per trovare le reti";
        break;
    case EST_SCANNING:
        txt = "Scansione...";
        col = lv_color_make(0xFF, 0xCC, 0x00);
        break;
    case EST_LIST:
        if (s_target >= 0)
            snprintf(buf, sizeof(buf), "Bersaglio: %s (ch %d)",
                     s_aps[s_target].ssid[0] ? s_aps[s_target].ssid : "(hidden)",
                     s_aps[s_target].channel);
        else
            snprintf(buf, sizeof(buf), "%d reti - toccane una", s_ap_count);
        break;
    case EST_RUNNING:
        txt = "Portale attivo - swipe su per fermare";
        col = lv_color_make(0xEE, 0x44, 0xAA);
        break;
    }
    lv_label_set_text(status_label, txt);
    lv_obj_set_style_text_color(status_label, col, LV_PART_MAIN);
}

static void update_view()
{
    bool running = (s_state == EST_RUNNING);
    if (running) { lv_obj_add_flag(list_box, LV_OBJ_FLAG_HIDDEN);
                   lv_obj_clear_flag(live_box, LV_OBJ_FLAG_HIDDEN); }
    else         { lv_obj_clear_flag(list_box, LV_OBJ_FLAG_HIDDEN);
                   lv_obj_add_flag(live_box, LV_OBJ_FLAG_HIDDEN); }
}

static void update_buttons()
{
    if (s_state == EST_RUNNING) {
        lv_obj_add_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(btn_atk_lbl, "STOP");
        lv_obj_set_style_bg_color(btn_atk, lv_color_make(0x88, 0x22, 0x22), LV_PART_MAIN);
        lv_obj_align(btn_atk, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    lv_obj_clear_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);
    const char *sl = (s_state == EST_SCANNING) ? "SCAN..." :
                     (s_target >= 0)           ? "RISCAN"  : "SCAN";
    lv_label_set_text(btn_scan_lbl, sl);
    lv_obj_set_style_bg_color(btn_scan, lv_color_make(0x00, 0x66, 0x99), LV_PART_MAIN);

    if (s_target >= 0 && s_state != EST_SCANNING) {
        lv_obj_clear_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(btn_atk_lbl, "CLONA");
        lv_obj_set_style_bg_color(btn_atk, lv_color_make(0xCC, 0x33, 0xAA), LV_PART_MAIN);
        lv_obj_align(btn_scan, LV_ALIGN_LEFT_MID,  6, 0);
        lv_obj_align(btn_atk,  LV_ALIGN_RIGHT_MID, -6, 0);
    } else {
        lv_obj_add_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(btn_scan, LV_ALIGN_CENTER, 0, 0);
    }
}

// ─── flow ────────────────────────────────────────────────────────────────────
static void start_scan()
{
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    s_state = EST_SCANNING;
}

static void on_ap_clicked(lv_event_t *e)
{
    if (s_state == EST_RUNNING) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ap_count) return;
    s_target = idx;
    show_aps();
    update_status();
    update_buttons();
}

static void on_scan_btn(lv_event_t *)
{
    if (s_state == EST_RUNNING) return;
    start_scan();
    update_status();
    update_buttons();
}

static void on_attack_btn(lv_event_t *)
{
    if (s_state == EST_RUNNING) {
        evil_portal_stop();
        s_state = (s_ap_count > 0) ? EST_LIST : EST_IDLE;
    } else if (s_target >= 0) {
        if (evil_portal_start(s_aps[s_target].ssid, s_aps[s_target].channel)) {
            build_live_box();
            refresh_live();
            s_state = EST_RUNNING;
        }
    }
    update_view();
    update_status();
    update_buttons();
}

static void on_refresh(lv_timer_t *)
{
    if (lv_screen_active() != screen) return;

    if (s_state == EST_SCANNING) {
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
            s_state = EST_LIST;
            show_aps();
        } else if (n == WIFI_SCAN_FAILED) {
            s_state = (s_ap_count > 0) ? EST_LIST : EST_IDLE;
        }
        update_status();
        update_buttons();
    } else if (s_state == EST_RUNNING) {
        refresh_live();     // evil_portal_tick() itself is pumped from the main loop
    }
}

// ─── events / layout ─────────────────────────────────────────────────────────
static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        evil_twin_screen_stop();
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
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
}

void evil_twin_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_make(0xEE, 0x44, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(title, "Evil Twin");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *warn = lv_label_create(screen);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(warn, lv_color_make(0x88, 0x55, 0x55), LV_PART_MAIN);
    lv_label_set_text(warn, "Solo su reti che sei autorizzato a testare");
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 50);

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x99, 0x55, 0x88), LV_PART_MAIN);
    lv_label_set_text(status_label, "Tocca SCAN per trovare le reti");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 74);

    lv_obj_t *btn_row = lv_obj_create(screen);
    lv_obj_set_size(btn_row, 404, 50);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn_row, LV_ALIGN_TOP_MID, 0, 100);

    btn_scan = make_button(btn_row, 196, 48, lv_color_make(0x00, 0x66, 0x99), &btn_scan_lbl);
    lv_obj_align(btn_scan, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_scan, on_scan_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_scan_lbl, "SCAN");

    btn_atk = make_button(btn_row, 196, 48, lv_color_make(0xCC, 0x33, 0xAA), &btn_atk_lbl);
    lv_obj_align(btn_atk, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_event_cb(btn_atk, on_attack_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_atk_lbl, "CLONA");
    lv_obj_add_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);

    // AP list (shown idle/list).
    list_box = lv_obj_create(screen);
    lv_obj_set_size(list_box, 404, 322);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_set_style_bg_color(list_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(list_box, lv_color_make(0x33, 0x00, 0x2a), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list_box, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);

    // Live capture panel (shown running), same footprint, hidden by default.
    live_box = lv_obj_create(screen);
    lv_obj_set_size(live_box, 404, 322);
    lv_obj_align(live_box, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_set_style_bg_color(live_box, lv_color_make(0x0A, 0x06, 0x0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(live_box, lv_color_make(0xCC, 0x44, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_border_width(live_box, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(live_box, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(live_box, 10, LV_PART_MAIN);
    lv_obj_clear_flag(live_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(live_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_refresh, 400, NULL);
}

void evil_twin_screen_show()
{
    if (!screen) evil_twin_screen_create();
    if (s_ap_count > 0) { s_state = EST_LIST; show_aps(); }
    else                { s_state = EST_IDLE; lv_obj_clean(list_box); }
    update_view();
    update_status();
    update_buttons();
    lv_scr_load(screen);
}

void evil_twin_screen_stop()
{
    evil_portal_stop();
    s_state = (s_ap_count > 0) ? EST_LIST : EST_IDLE;
    update_view();
}

bool evil_twin_screen_is_active() { return screen && lv_screen_active() == screen; }
