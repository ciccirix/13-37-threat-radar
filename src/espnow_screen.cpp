#include "espnow_screen.h"
#include "esp_now_link.h"
#include "send_message_screen.h"
#include "tools_screen.h"          // tools_screen_show() for the back-gesture
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

static lv_obj_t *s_screen   = nullptr;
static lv_obj_t *s_status   = nullptr;   // channel + self id line
static lv_obj_t *s_toggle   = nullptr;   // ON/OFF pill
static lv_obj_t *s_toggle_lbl = nullptr;
static lv_obj_t *s_list     = nullptr;   // scrollable flex-column of peer rows
static lv_obj_t *s_inbox    = nullptr;   // last-received message preview
static lv_timer_t *s_timer  = nullptr;
static bool s_active = false;

// Tap a peer row (or the broadcast row) -> open the shared compose screen
// pre-targeted over ESP-NOW. The target id is stashed in the row's user_data.
static void on_peer_clicked(lv_event_t *e)
{
    auto row = (lv_obj_t *)lv_event_get_target(e);
    uint32_t id = (uint32_t)(uintptr_t)lv_obj_get_user_data(row);
    if (!esp_now_link_is_active()) return;   // nothing to send over yet
    send_message_screen_show_espnow_to(id);
}

static lv_obj_t *make_row(uint32_t id, const char *title, const char *sub,
                          lv_color_t accent)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, 380, 72);
    lv_obj_set_style_bg_color(row, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, accent, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 14, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(row, (void *)(uintptr_t)id);
    lv_obj_add_event_cb(row, on_peer_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *head = lv_label_create(row);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(head, accent, LV_PART_MAIN);
    lv_label_set_text(head, title);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 8, 2);
    lv_obj_add_flag(head, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *m = lv_label_create(row);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(m, lv_color_make(0x00, 0xAA, 0x00), LV_PART_MAIN);
    lv_label_set_text(m, sub);
    lv_obj_align(m, LV_ALIGN_TOP_LEFT, 8, 32);
    lv_obj_add_flag(m, LV_OBJ_FLAG_EVENT_BUBBLE);
    return row;
}

static void update_toggle()
{
    bool on = esp_now_link_is_active();
    lv_color_t c = on ? lv_color_make(0x00, 0xCC, 0x66)
                      : lv_color_make(0x66, 0x66, 0x66);
    lv_obj_set_style_border_color(s_toggle, c, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_toggle_lbl, c, LV_PART_MAIN);
    lv_label_set_text(s_toggle_lbl, on ? LV_SYMBOL_WIFI "  ON" : LV_SYMBOL_POWER "  OFF");
}

static void refresh()
{
    update_toggle();

    bool on = esp_now_link_is_active();
    if (on) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s  ·  id !%08lx  ·  ch %u",
                 esp_now_link_self_name(),
                 (unsigned long)esp_now_link_self_id(),
                 (unsigned)esp_now_link_channel());
        lv_label_set_text(s_status, buf);
    } else {
        lv_label_set_text(s_status, "Link off — tap ON to start beaconing");
    }

    lv_obj_clean(s_list);

    if (!on) return;

    // Broadcast row is always first — message everyone on the channel at once.
    make_row(ENOW_BROADCAST_ID, LV_SYMBOL_CALL "  Broadcast",
             "message everyone on the channel",
             lv_color_make(0x33, 0xBB, 0xFF));

    int n = esp_now_link_peer_count();
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(empty, lv_color_make(0x00, 0x66, 0x00), LV_PART_MAIN);
        lv_label_set_text(empty,
            "\nNo peers yet.\n\n"
            "Another watch on channel 1 with\n"
            "ESP-NOW ON will show up here\n"
            "within a few seconds.");
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        return;
    }

    for (int i = 0; i < n; i++) {
        const EnowPeer *p = esp_now_link_peer(i);
        if (!p) continue;
        char sub[64];
        uint32_t age_s = (millis() - p->last_heard_ms) / 1000;
        snprintf(sub, sizeof(sub), "%s  ·  heard %lus ago  ·  !%08lx",
                 p->encrypted ? LV_SYMBOL_EYE_CLOSE " enc" : LV_SYMBOL_EYE_OPEN " clear",
                 (unsigned long)age_s, (unsigned long)p->id);
        make_row(p->id, p->name, sub, lv_color_make(0x33, 0xDD, 0xAA));
    }
}

static void update_inbox()
{
    const EnowMessage *m = esp_now_link_msg(0);
    if (!m) {
        lv_label_set_text(s_inbox, "");
        return;
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "%s %s [%s]: %.48s",
             m->outgoing ? LV_SYMBOL_UPLOAD : LV_SYMBOL_DOWNLOAD,
             m->name, m->time_str, m->text);
    lv_label_set_text(s_inbox, buf);
    lv_obj_set_style_text_color(s_inbox,
        m->outgoing ? lv_color_make(0x88, 0x88, 0x88)
                    : lv_color_make(0xDD, 0xDD, 0xDD), LV_PART_MAIN);
}

static void on_timer(lv_timer_t *)
{
    if (!s_active) return;
    refresh();
    update_inbox();
}

static void on_toggle(lv_event_t *)
{
    esp_now_link_set_active(!esp_now_link_is_active());
    refresh();
    update_inbox();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        // Leave the link running in the background if it's on — the beacon +
        // RX keep working from bg_tick even off-screen; only the UI stops.
        tools_screen_show();
    }
}

void espnow_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(0x33, 0xDD, 0xAA), LV_PART_MAIN);
    lv_label_set_text(title, "ESP-NOW LINK");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    s_status = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_make(0x00, 0x99, 0x00), LV_PART_MAIN);
    lv_label_set_text(s_status, "");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 54);

    // Scrollable peer list.
    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, 404, 300);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 80);

    // Last-received message preview above the bottom controls.
    s_inbox = lv_label_create(s_screen);
    lv_obj_set_width(s_inbox, 384);
    lv_obj_set_style_text_font(s_inbox, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_inbox, lv_color_make(0x00, 0xDD, 0x00), LV_PART_MAIN);
    lv_label_set_long_mode(s_inbox, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_inbox, "");
    lv_obj_align(s_inbox, LV_ALIGN_BOTTOM_MID, 0, -84);

    // ON/OFF pill.
    s_toggle = lv_obj_create(s_screen);
    lv_obj_set_size(s_toggle, 200, 48);
    lv_obj_set_style_radius(s_toggle, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_toggle, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_toggle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_toggle, 1, LV_PART_MAIN);
    lv_obj_clear_flag(s_toggle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_toggle, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_add_event_cb(s_toggle, on_toggle, LV_EVENT_CLICKED, NULL);
    s_toggle_lbl = lv_label_create(s_toggle);
    lv_obj_set_style_text_font(s_toggle_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(s_toggle_lbl);
    lv_obj_add_flag(s_toggle_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_timer = lv_timer_create(on_timer, 1500, NULL);
}

void espnow_screen_show()
{
    if (!s_screen) espnow_screen_create();
    s_active = true;
    esp_now_link_mark_read();
    refresh();
    update_inbox();
    lv_scr_load(s_screen);
}

bool espnow_screen_is_active() { return s_active; }
