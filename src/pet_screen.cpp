#include "pet_screen.h"
#include "pwnagotchi_peer.h"
#include "threat_radar.h"
#include "tools_screen.h"
#include "wifi_beacon_manager.h"
#include "handshake.h"
#include "fish_img.h"
#include <lvgl.h>
#include <LilyGoLib.h>
#include <SD.h>
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include "esp_heap_caps.h"

// The pwnpet is a REAL goldfish (frames from a clip, RGB565 on the SD card at
// /pwn/fish/NN.bin) that swims in a loop and physically GROWS with XP — baby at
// level 1, full-size as XP climbs. Mood drives what it blurts out (ASCII only;
// the on-device font has no emoji). XP persists to /pwn/pet.txt. If the SD swim
// pack is missing it falls back to the single baked still (fish_img).
enum { PET_BORED = 0, PET_HAPPY, PET_EXCITED, PET_WARY };
static const char *kSpeech[4] = {
    "blub... blub...",
    "swimming the airwaves ~",
    "",                         // EXCITED filled with the peer name at refresh
    "danger in the water!"
};

// Swim-frame geometry — must match the SD pack. Now the FULL pwnfish clip as a
// seamless forward loop (make_loop.py: 256x144 RGB565, 16:9, ~38 frames with a
// crossfaded head so the loop never visibly restarts).
#define FISH_W        256
#define FISH_H        144
#define FISH_BYTES    (FISH_W * FISH_H * 2)
#define FISH_MAXF     64

static lv_obj_t  *s_screen   = nullptr;
static lv_obj_t  *s_fish     = nullptr;   // lv_image, src swapped per frame
static lv_obj_t  *s_speech   = nullptr;
static lv_obj_t  *s_lvl      = nullptr;
static lv_obj_t  *s_bar_fill = nullptr;
static lv_obj_t  *s_stats    = nullptr;
static lv_timer_t *s_timer   = nullptr;   // 1 Hz mood/XP
static lv_timer_t *s_anim    = nullptr;   // ~12 Hz swim + bob
static bool       s_active   = false;

// Real swim frames loaded into PSRAM (loaded once, kept).
static lv_image_dsc_t s_dsc[FISH_MAXF];
static uint8_t       *s_buf[FISH_MAXF] = { nullptr };
static int            s_nframes = 0;
static bool           s_loaded  = false;
static int            s_fidx = 0, s_fdir = 1;

static uint32_t s_phase = 0;

static long     s_xp          = 0;
static bool     s_xp_loaded   = false;
static uint8_t  s_mood        = PET_BORED;
static uint32_t s_mood_until  = 0;
static int      s_last_peers  = 0;
static int      s_last_pwnd   = 0;
static bool     s_eat         = false;
static bool     s_dirty       = false;
static uint32_t s_last_save   = 0;

static int level_of(long xp) { return 1 + (int)(xp / 100); }
static int xp_into(long xp)  { return (int)(xp % 100); }

// The SD swim pack (gen_swim.py) is a SINGLE continuous swim loop of frames,
// played end-to-end in on_anim. (It used to be split into FISH_STAGES groups of
// FISH_PER_STAGE for "growth", but the frames aren't stage-organised, so that
// only ever cycled 3 of them and the swim looked like it was missing frames.)

// Wardriving XP: the pwnfish "swims the airwaves", so each NEW WiFi network it
// sees earns a little XP — XP then climbs anywhere there's WiFi, not only on the
// rare handshake/peer/threat events. Deduped by BSSID; touched only on the WiFi
// task, the count is read by on_tick on the main task.
#define PET_NET_SEEN_MAX 96
static uint8_t           s_net_seen[PET_NET_SEEN_MAX][6];
static int               s_net_seen_count = 0;
static volatile uint32_t s_nets_total = 0;
static uint32_t          s_last_nets  = 0;

static void pet_wifi_cb(const WifiBeacon *b)
{
    for (int i = 0; i < s_net_seen_count; i++)
        if (memcmp(s_net_seen[i], b->bssid, 6) == 0) return;   // already counted
    if (s_net_seen_count < PET_NET_SEEN_MAX) {
        memcpy(s_net_seen[s_net_seen_count++], b->bssid, 6);
        s_nets_total++;
    }
}

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
    File f = SD.open("/pwn/pet.txt", FILE_WRITE);
    if (!f) return;
    f.printf("%ld", s_xp);
    f.close();
    s_dirty = false;
    s_last_save = millis();
}

// Pull the real swim frames off the SD card into PSRAM (once). Each NN.bin is a
// raw little-endian RGB565 FISH_W x FISH_H frame; count.txt holds how many.
static void load_fish_frames()
{
    if (s_loaded) return;
    s_loaded = true;                 // one attempt; falls back to the still on miss
    if (!instance.isCardReady()) return;

    int want = FISH_MAXF;
    File cf = SD.open("/pwn/fish/count.txt", FILE_READ);
    if (cf) { char b[8] = {0}; int n = cf.readBytes(b, 7); cf.close();
              if (n > 0) { int c = atoi(b); if (c > 0 && c < FISH_MAXF) want = c; } }

    for (int i = 0; i < want; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/pwn/fish/%02d.bin", i);
        File f = SD.open(path, FILE_READ);
        if (!f) break;
        if (f.size() != FISH_BYTES) { f.close(); break; }
        uint8_t *buf = (uint8_t *)heap_caps_malloc(FISH_BYTES, MALLOC_CAP_SPIRAM);
        if (!buf) { f.close(); break; }
        f.read(buf, FISH_BYTES);
        f.close();
        s_buf[i] = buf;
        s_dsc[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_dsc[i].header.cf     = LV_COLOR_FORMAT_RGB565;
        s_dsc[i].header.w      = FISH_W;
        s_dsc[i].header.h      = FISH_H;
        s_dsc[i].header.stride = FISH_W * 2;
        s_dsc[i].data_size     = FISH_BYTES;
        s_dsc[i].data          = buf;
        s_nframes = i + 1;
    }
}

static void refresh()
{
    if (s_mood == PET_EXCITED) {
        if (s_eat) lv_label_set_text(s_speech, "nom! caught a handshake!");
        else       lv_label_set_text_fmt(s_speech, "hi %s! blub!", pwnagotchi_last_name());
    } else {
        lv_label_set_text(s_speech, kSpeech[s_mood]);
    }

    lv_label_set_text_fmt(s_lvl, "LVL %d", level_of(s_xp));
    lv_obj_set_width(s_bar_fill, 2 + (xp_into(s_xp) * 296) / 100);

    int peers = pwnagotchi_peer_count();
    lv_label_set_text_fmt(s_stats, "PWND %d   friends %d   xp %ld",
        handshake_pwnd_count(), peers, s_xp);

    // The SD pack is a single continuous swim (not life-stage groups), so the
    // whole loop plays in on_anim; the frame index is left alone here so the
    // swim isn't reset every second.
}

static void on_tick(lv_timer_t *)
{
    if (!s_active) return;
    uint32_t now = millis();

    // Wardriving XP: 1.5 per new WiFi network the pwnfish has swum past. XP is an
    // integer, so award floor(1.5 * total) incrementally (2 nets = 3 xp).
    uint32_t nets = s_nets_total;
    if (nets > s_last_nets) {
        s_xp += ((long)nets * 3 / 2) - ((long)s_last_nets * 3 / 2);
        s_last_nets = nets;
        s_dirty = true;
    }

    int peers  = pwnagotchi_peer_count();
    int threat = threatradar_top_level();
    int pwnd   = handshake_pwnd_count();

    uint8_t newmood;
    if (pwnd > s_last_pwnd) {
        s_xp += 30L * (pwnd - s_last_pwnd);
        s_last_pwnd = pwnd;
        newmood = PET_EXCITED; s_eat = true;  s_mood_until = now + 5000; s_dirty = true;
    } else if (peers > s_last_peers) {
        s_xp += 50L * (peers - s_last_peers);
        s_last_peers = peers;
        newmood = PET_EXCITED; s_eat = false; s_mood_until = now + 6000; s_dirty = true;
    } else if (threat >= TR_LVL_LIKELY) {
        newmood = PET_WARY;    s_mood_until = now + 4000;
    } else if (now < s_mood_until) {
        newmood = s_mood;
    } else {
        newmood = peers > 0 ? PET_HAPPY : PET_BORED;
    }
    if (newmood == PET_WARY && s_mood != PET_WARY) { s_xp += 10; s_dirty = true; }
    s_mood = newmood;

    if (s_dirty && now - s_last_save > 15000) save_xp();
    refresh();
}

// Swim: cycle the real frames (ping-pong for a seamless loop) + a gentle float.
static void on_anim(lv_timer_t *)
{
    if (!s_active) return;
    s_phase++;

    // The SD pack is ONE continuous swim clip — play it forward on a loop, like
    // a video (0->last->0). Ping-pong would replay the second half BACKWARDS,
    // which looks like the fish swimming in reverse.
    if (s_nframes >= 2) {
        s_fidx++;
        if (s_fidx >= s_nframes) s_fidx = 0;
        lv_image_set_src(s_fish, &s_dsc[s_fidx]);
    }

    int dy = (int)(6.0f * sinf(s_phase * 0.16f));
    int dx = (int)(4.0f * sinf(s_phase * 0.08f));
    lv_obj_align(s_fish, LV_ALIGN_TOP_MID, dx, 8 + dy);
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        if (s_dirty) save_xp();
        wifi_beacon_remove(pet_wifi_cb);
        tools_screen_show();
    }
}

void pet_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_make(0x04, 0x10, 0x1c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // The fish — an image whose source is swapped each frame for the swim. Kept
    // in the TOP part of the screen (video-window style), stats below.
    s_fish = lv_image_create(s_screen);
    lv_image_set_src(s_fish, &fish_img);            // still fallback until frames load
    // Pivot at TOP-centre so the (later) scale-up grows the video down + sideways
    // from a fixed top edge, keeping it anchored in the top band.
    lv_image_set_pivot(s_fish, FISH_W / 2, 0);
    lv_obj_align(s_fish, LV_ALIGN_TOP_MID, 0, 8);

    s_speech = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_speech, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_speech, lv_color_make(0x7B, 0xDC, 0xFF), LV_PART_MAIN);
    lv_label_set_text(s_speech, kSpeech[PET_BORED]);
    lv_obj_align(s_speech, LV_ALIGN_CENTER, 0, 60);

    s_lvl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_lvl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lvl, lv_color_make(0xFF, 0x8C, 0x1A), LV_PART_MAIN);
    lv_label_set_text(s_lvl, "LVL 1");
    lv_obj_align(s_lvl, LV_ALIGN_CENTER, 0, 100);   // own line, centred above the bar

    lv_obj_t *bar_bg = lv_obj_create(s_screen);
    lv_obj_set_size(bar_bg, 300, 16);
    lv_obj_set_style_radius(bar_bg, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_bg, lv_color_make(0x0E, 0x1C, 0x28), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar_bg, lv_color_make(0x1C, 0x38, 0x50), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_bg, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar_bg, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar_bg, LV_ALIGN_CENTER, 0, 128);

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
    lv_obj_set_style_text_color(s_stats, lv_color_make(0x8A, 0xA8, 0xBA), LV_PART_MAIN);
    lv_label_set_text(s_stats, "friends 0   xp 0");
    lv_obj_align(s_stats, LV_ALIGN_CENTER, 0, 158);

    s_timer = lv_timer_create(on_tick, 1000, NULL);
    s_anim  = lv_timer_create(on_anim, 80,   NULL);
}

void pet_screen_show()
{
    if (!s_screen) pet_screen_create();
    if (!s_xp_loaded) load_xp();
    load_fish_frames();
    if (s_nframes > 0) {
        s_fidx = 0; s_fdir = 1;
        lv_image_set_src(s_fish, &s_dsc[0]);
        lv_image_set_scale(s_fish, 384);   // 1.5x: 256x144 -> 384x216, fills the width
    }
    s_last_peers = pwnagotchi_peer_count();
    s_last_pwnd  = handshake_pwnd_count();
    s_net_seen_count = 0; s_nets_total = 0; s_last_nets = 0;   // fresh wardriving tally
    s_active = true;
    wifi_beacon_add(pet_wifi_cb);      // power the scanner: meet peers + earn net XP
    refresh();
    lv_scr_load(s_screen);
}

bool pet_screen_is_active() { return s_active; }
