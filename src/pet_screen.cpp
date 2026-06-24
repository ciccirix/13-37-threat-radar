#include "pet_screen.h"
#include "pwnagotchi_peer.h"
#include "threat_radar.h"
#include "tools_screen.h"
#include "wifi_beacon_manager.h"
#include <lvgl.h>
#include <LilyGoLib.h>
#include <SD.h>
#include <Arduino.h>
#include <stdio.h>

// Moods — faces are ASCII only (the LVGL Montserrat build has no fancy
// emoticon glyphs, so unicode faces would render as missing boxes on device).
enum { PET_BORED = 0, PET_HAPPY, PET_EXCITED, PET_WARY };
static const char *kFace[4]   = { "(-_-)", "(^_^)", "\\(^o^)/", "(O_o)" };
static const char *kSpeech[4] = {
    "no friends nearby...",
    "sniffing the airwaves",
    "",                         // EXCITED is filled with the peer name at refresh
    "someone is tailing us!"
};

static lv_obj_t  *s_screen   = nullptr;
static lv_obj_t  *s_face     = nullptr;
static lv_obj_t  *s_speech   = nullptr;
static lv_obj_t  *s_lvl      = nullptr;
static lv_obj_t  *s_bar_fill = nullptr;
static lv_obj_t  *s_stats    = nullptr;
static lv_timer_t *s_timer   = nullptr;
static bool       s_active   = false;

static long     s_xp          = 0;
static bool     s_xp_loaded   = false;
static uint8_t  s_mood        = PET_BORED;
static uint32_t s_mood_until  = 0;
static int      s_last_peers  = 0;
static bool     s_dirty       = false;
static uint32_t s_last_save   = 0;

static int level_of(long xp)    { return 1 + (int)(xp / 100); }
static int xp_into(long xp)     { return (int)(xp % 100); }

// Keep the WiFi scanner alive while the pet is on screen so peers are met live.
// The actual detection happens in the beacon manager's parser; this consumer
// just refcounts the radio on.
static void pet_wifi_noop(const WifiBeacon *b) { (void)b; }

static void load_xp()
{
    s_xp_loaded = true;
    if (!instance.isCardReady()) return;
    File f = SD.open("/pwn/pet.txt", FILE_READ);
    if (!f) return;
    char buf[24] = {0};
    int n = f.readBytes(buf, sizeof(buf) - 1);
    f.close();
    if (n > 0) { buf[n] = '\0'; s_xp = atol(buf); if (s_xp < 0) s_xp = 0; }
}

static void save_xp()
{
    if (!instance.isCardReady()) return;
    if (!SD.exists("/pwn")) SD.mkdir("/pwn");
    File f = SD.open("/pwn/pet.txt", FILE_WRITE);   // truncate + rewrite
    if (!f) return;
    f.printf("%ld", s_xp);
    f.close();
    s_dirty = false;
    s_last_save = millis();
}

static void refresh()
{
    lv_label_set_text(s_face, kFace[s_mood]);

    if (s_mood == PET_EXCITED) lv_label_set_text_fmt(s_speech, "hi %s!", pwnagotchi_last_name());
    else                       lv_label_set_text(s_speech, kSpeech[s_mood]);

    lv_color_t face_col =
        (s_mood == PET_WARY)    ? lv_color_make(0xFF, 0x6A, 0x3A) :
        (s_mood == PET_EXCITED) ? lv_color_make(0x33, 0xDD, 0x88) :
                                  lv_color_make(0xCC, 0xCC, 0xCC);
    lv_obj_set_style_text_color(s_face, face_col, LV_PART_MAIN);

    lv_label_set_text_fmt(s_lvl, "LVL %d", level_of(s_xp));
    lv_obj_set_width(s_bar_fill, 2 + (xp_into(s_xp) * 296) / 100);

    int peers = pwnagotchi_peer_count();
    lv_label_set_text_fmt(s_stats, "friends %d   xp %ld   up %lum",
        peers, s_xp, (unsigned long)(millis() / 60000));
}

static void on_tick(lv_timer_t *)
{
    if (!s_active) return;
    uint32_t now = millis();

    int peers  = pwnagotchi_peer_count();
    int threat = threatradar_top_level();

    uint8_t newmood;
    if (peers > s_last_peers) {                       // met a new Pwnagotchi
        s_xp += 50L * (peers - s_last_peers);
        s_last_peers = peers;
        newmood = PET_EXCITED; s_mood_until = now + 6000; s_dirty = true;
    } else if (threat >= TR_LVL_LIKELY) {
        newmood = PET_WARY;    s_mood_until = now + 4000;
    } else if (now < s_mood_until) {
        newmood = s_mood;                             // hold the transient mood
    } else {
        newmood = peers > 0 ? PET_HAPPY : PET_BORED;
    }
    if (newmood == PET_WARY && s_mood != PET_WARY) { s_xp += 10; s_dirty = true; }
    s_mood = newmood;

    if (s_dirty && now - s_last_save > 15000) save_xp();
    refresh();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        if (s_dirty) save_xp();
        wifi_beacon_remove(pet_wifi_noop);
        tools_screen_show();
    }
}

void pet_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *name = lv_label_create(s_screen);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_make(0x33, 0xBB, 0xFF), LV_PART_MAIN);
    lv_label_set_text(name, "1337  the pwnpet");
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 30);

    s_face = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_face, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_face, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_label_set_text(s_face, kFace[PET_BORED]);
    lv_obj_align(s_face, LV_ALIGN_CENTER, 0, -50);

    s_speech = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_speech, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_speech, lv_color_make(0x9A, 0xA4, 0xB2), LV_PART_MAIN);
    lv_label_set_text(s_speech, kSpeech[PET_BORED]);
    lv_obj_align(s_speech, LV_ALIGN_CENTER, 0, 10);

    s_lvl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_lvl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lvl, lv_color_make(0xFF, 0x8C, 0x1A), LV_PART_MAIN);
    lv_label_set_text(s_lvl, "LVL 1");
    lv_obj_align(s_lvl, LV_ALIGN_CENTER, 0, 70);

    lv_obj_t *bar_bg = lv_obj_create(s_screen);
    lv_obj_set_size(bar_bg, 300, 16);
    lv_obj_set_style_radius(bar_bg, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_bg, lv_color_make(0x16, 0x1B, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar_bg, lv_color_make(0x2B, 0x33, 0x40), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_bg, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar_bg, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar_bg, LV_ALIGN_CENTER, 0, 100);

    s_bar_fill = lv_obj_create(bar_bg);
    lv_obj_set_size(s_bar_fill, 2, 14);
    lv_obj_set_style_radius(s_bar_fill, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_fill, lv_color_make(0xFF, 0x8C, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar_fill, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_bar_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_bar_fill, LV_ALIGN_LEFT_MID, 0, 0);

    s_stats = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_stats, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_stats, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(s_stats, "friends 0   xp 0   up 0m");
    lv_obj_align(s_stats, LV_ALIGN_CENTER, 0, 140);

    s_timer = lv_timer_create(on_tick, 1000, NULL);
}

void pet_screen_show()
{
    if (!s_screen) pet_screen_create();
    if (!s_xp_loaded) load_xp();
    s_last_peers = pwnagotchi_peer_count();
    s_active = true;
    wifi_beacon_add(pet_wifi_noop);   // power the scanner so we meet peers live
    refresh();
    lv_scr_load(s_screen);
}

bool pet_screen_is_active() { return s_active; }
