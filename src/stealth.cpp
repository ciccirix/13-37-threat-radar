#include "stealth.h"
#include <LilyGoLib.h>
#include <lvgl.h>
#include <Arduino.h>
#include <Preferences.h>

void clock_screen_show();   // defined in main.cpp

static bool s_active = false;
static bool s_armed  = false;   // duress is opt-in; off by default

// State lives in internal NVS (not the SD card) so the disguise can't be
// bypassed by yanking the microSD, and survives a power cycle.
static void persist()
{
    Preferences p;
    if (p.begin("duress", false)) {
        p.putBool("armed",  s_armed);
        p.putBool("active", s_active);
        p.end();
    }
}

void stealth_load()
{
    Preferences p;
    if (p.begin("duress", true)) {
        s_armed  = p.getBool("armed",  false);
        s_active = p.getBool("active", false);
        p.end();
    }
    // If restored disguised, do NOT vibrate or call stealth_enter() — the boot
    // is silent and the loop guard + hidden badges enforce the disguise.
}

bool stealth_active() { return s_active; }
bool stealth_armed()  { return s_armed; }

void stealth_set_armed(bool on)
{
    s_armed = on;
    if (!on) s_active = false;   // disarming drops any active disguise
    persist();
}

void stealth_enter()
{
    if (s_active) return;
    s_active = true;
    persist();                  // survive a reboot while disguised
    instance.vibrator();        // one short blip confirms the disguise engaged
    clock_screen_show();        // loop() keeps us pinned to the clock while active
}

void stealth_exit()
{
    if (!s_active) return;
    s_active = false;
    persist();                  // the secret 4 s hold clears the persisted flag
    instance.vibrator();
}

// ── Shake detector ───────────────────────────────────────────────────────────
// Count large magnitude deltas inside a sliding window. A deliberate shaker
// motion produces several >SHAKE_DELTA_G swings per second; gentle wrist tilts
// and a motorcycle's high-frequency buzz stay well under the bar.
#define SHAKE_DELTA_G    2.0f
#define SHAKE_HITS       8
#define SHAKE_WINDOW_MS  1400

static uint8_t  s_hits         = 0;
static uint32_t s_window_start = 0;

void stealth_feed_accel_delta(float delta_g)
{
    if (!s_armed || s_active) return;             // dormant unless armed
    uint32_t now = millis();
    if (now - s_window_start > SHAKE_WINDOW_MS) { // window expired — restart count
        s_window_start = now;
        s_hits = 0;
    }
    if (delta_g >= SHAKE_DELTA_G) {
        if (s_hits < 255) s_hits++;
        if (s_hits >= SHAKE_HITS) stealth_enter();
    }
}

// ── 4-second long-press to disarm ────────────────────────────────────────────
// Reads the touch indev directly (like screenshot_poll) so it fires regardless
// of how LVGL would otherwise route the press.
#define STEALTH_EXIT_MS 4000

static uint32_t s_press_start = 0;
static bool     s_pressing    = false;

void stealth_poll()
{
    if (!s_active) { s_pressing = false; return; }

    uint32_t now = millis();
    static uint32_t s_last = 0;
    if (now - s_last < 100) return;               // ~10 Hz is plenty for a 4 s hold
    s_last = now;

    lv_indev_t *indev = NULL;
    for (lv_indev_t *it = lv_indev_get_next(NULL); it; it = lv_indev_get_next(it))
        if (lv_indev_get_type(it) == LV_INDEV_TYPE_POINTER) { indev = it; break; }
    if (!indev) return;

    bool down = (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED);
    if (down) {
        if (!s_pressing) { s_pressing = true; s_press_start = now; }
        else if (now - s_press_start >= STEALTH_EXIT_MS) { s_pressing = false; stealth_exit(); }
    } else {
        s_pressing = false;
    }
}
