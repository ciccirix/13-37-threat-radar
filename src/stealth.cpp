#include "stealth.h"
#include <LilyGoLib.h>
#include <lvgl.h>
#include <Arduino.h>

void clock_screen_show();   // defined in main.cpp

static bool s_active = false;

bool stealth_active() { return s_active; }

void stealth_enter()
{
    if (s_active) return;
    s_active = true;
    instance.vibrator();        // one short blip confirms the disguise engaged
    clock_screen_show();        // loop() keeps us pinned to the clock while active
}

void stealth_exit()
{
    if (!s_active) return;
    s_active = false;
    instance.vibrator();
}

// ── Shake detector ───────────────────────────────────────────────────────────
// Count large magnitude deltas inside a sliding window. A deliberate shaker
// motion produces several >SHAKE_DELTA_G swings per second; gentle wrist tilts
// and a motorcycle's high-frequency buzz stay well under the bar.
#define SHAKE_DELTA_G    1.5f
#define SHAKE_HITS       6
#define SHAKE_WINDOW_MS  1300

static uint8_t  s_hits         = 0;
static uint32_t s_window_start = 0;

void stealth_feed_accel_delta(float delta_g)
{
    if (s_active) return;                         // already disguised
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
