#include "task_manager.h"
#include "tools_screen.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <lvgl.h>
#include <stdio.h>

// Every subsystem we can report + kill. Each has an is-running probe and a stop.
#include "meshtastic.h"
#include "esp_now_link.h"
#include "phone_link.h"
#include "scan_engine.h"
#include "evil_portal.h"
#include "airtag.h"
#include "flipper.h"
#include "skimmer.h"
#include "flock.h"
#include "evil_twin.h"
#include "meta_glasses.h"
#include "handshake.h"
#include "pager.h"
#include "tpms.h"
#include "aprs.h"
#include "deauther.h"
#include "cam_audit.h"
#include "pingsweep.h"
#include "mouse_hid.h"

// ─── task table ──────────────────────────────────────────────────────────────
static bool wifi_on()   { return WiFi.status() == WL_CONNECTED; }
static void wifi_off()  { WiFi.disconnect(true, false); }
static void mesh_off()  { meshtastic_set_active(false); }
static void espnow_off(){ esp_now_link_set_active(false); }

struct Task { const char *name; bool (*running)(); void (*stop)(); };

static const Task TASKS[] = {
    { "WiFi (STA)",       wifi_on,                 wifi_off },
    { "LoRa / Meshtastic",meshtastic_is_active,    mesh_off },
    { "ESP-NOW link",     esp_now_link_is_active,  espnow_off },
    { "Phone link (BLE)", phone_link_active,       phone_link_stop },
    { "Scanner",          scan_engine_running,     scan_engine_stop },
    { "Evil Twin portal", evil_portal_running,     evil_portal_stop },
    { "AirTag scan",      airtag_is_running,       airtag_stop },
    { "Flipper scan",     flipper_is_running,      flipper_stop },
    { "Skimmer scan",     skimmer_is_running,      skimmer_stop },
    { "Flock scan",       flock_is_running,        flock_stop },
    { "Evil Twin detect", evil_twin_is_running,    evil_twin_stop },
    { "Meta glasses",     meta_glasses_is_running, meta_glasses_stop },
    { "Pwn capture",      handshake_is_running,    handshake_stop },
    { "Pager",            pager_is_running,        pager_stop },
    { "TPMS",             tpms_is_running,         tpms_stop },
    { "APRS",             aprs_is_running,         aprs_stop },
    { "Deauther",         deauther_is_running,     deauther_stop },
    { "Cam LAN audit",    cam_audit_is_running,    cam_audit_stop },
    { "Ping sweep",       pingsweep_is_running,    pingsweep_stop },
    { "Mouse HID (BLE)",  mouse_hid_is_running,    mouse_hid_stop },
};
#define NTASKS (int)(sizeof(TASKS) / sizeof(TASKS[0]))

// ─── widgets ─────────────────────────────────────────────────────────────────
static lv_obj_t   *s_screen = nullptr;
static lv_obj_t   *s_batt   = nullptr;
static lv_obj_t   *s_list   = nullptr;
static lv_timer_t *s_timer  = nullptr;
static bool        s_active = false;

static void refresh();

static void on_kill(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < NTASKS && TASKS[idx].stop) TASKS[idx].stop();
    refresh();
}

static void on_kill_all(lv_event_t *)
{
    for (int i = 0; i < NTASKS; i++)
        if (TASKS[i].running && TASKS[i].running() && TASKS[i].stop) TASKS[i].stop();
    refresh();
}

static void add_row(int idx)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, 384, 52);
    lv_obj_set_style_bg_color(row, lv_color_make(0x12, 0x0A, 0x0A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_make(0xCC, 0x44, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 14, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_kill, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *nm = lv_label_create(row);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm, lv_color_make(0xFF, 0xAA, 0x66), LV_PART_MAIN);
    lv_label_set_text(nm, TASKS[idx].name);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(k, lv_color_make(0xFF, 0x44, 0x44), LV_PART_MAIN);
    lv_label_set_text(k, LV_SYMBOL_CLOSE "  STOP");
    lv_obj_align(k, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_flag(k, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void refresh()
{
    int pct = instance.pmu.getBatteryPercent();
    lv_label_set_text_fmt(s_batt, LV_SYMBOL_BATTERY_FULL "  %d%%%s",
                          pct, instance.pmu.isCharging() ? "  " LV_SYMBOL_CHARGE : "");

    lv_obj_clean(s_list);
    int active = 0;
    for (int i = 0; i < NTASKS; i++)
        if (TASKS[i].running && TASKS[i].running()) { add_row(i); active++; }

    if (active == 0) {
        lv_obj_t *e = lv_label_create(s_list);
        lv_obj_set_style_text_font(e, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(e, lv_color_make(0x00, 0x99, 0x55), LV_PART_MAIN);
        lv_label_set_text(e, LV_SYMBOL_OK "  Tutto spento.\n\nNessuna radio o scansione attiva:\nbatteria al sicuro.");
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
}

static void on_timer(lv_timer_t *) { if (s_active) refresh(); }

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        tools_screen_show();
    }
}

void task_manager_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(0xFF, 0x9A, 0x33), LV_PART_MAIN);
    lv_label_set_text(title, "TASK MANAGER");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 20);

    s_batt = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_batt, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_batt, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN);
    lv_label_set_text(s_batt, "");
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -18, 26);

    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, 404, 340);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t *ka = lv_obj_create(s_screen);
    lv_obj_set_size(ka, 200, 44);
    lv_obj_set_style_radius(ka, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ka, lv_color_make(0x88, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ka, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ka, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ka, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ka, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(ka, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(ka, on_kill_all, LV_EVENT_CLICKED, NULL);
    lv_obj_t *kal = lv_label_create(ka);
    lv_obj_set_style_text_font(kal, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(kal, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(kal, LV_SYMBOL_POWER "  STOP ALL");
    lv_obj_center(kal);
    lv_obj_add_flag(kal, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_timer = lv_timer_create(on_timer, 800, NULL);
}

void task_manager_show()
{
    if (!s_screen) task_manager_create();
    s_active = true;
    refresh();
    lv_scr_load(s_screen);
}

bool task_manager_is_active() { return s_active; }
