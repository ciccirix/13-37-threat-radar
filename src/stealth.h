#pragma once
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Duress disguise ("shaker") mode.
//
// A vigorous shake of the watch instantly drops it into a bare clock face: the
// RF/security tooling is hidden, the scan-count badges disappear and navigation
// is pinned to the clock, so at a glance it is just an ordinary watch. Holding
// the screen for 4 seconds restores the full firmware. For recon in unfriendly
// company — if someone grabs your wrist, a flick hides everything.
//
// The shake is detected from the accelerometer magnitude deltas already sampled
// by the motion-wake poll (so it rides for free on that 10 Hz stream; it does
// require motion-wake to be enabled, which is the default). Thresholds are set
// high so ordinary wrist movement — or a motorcycle's buzz — doesn't trip it.
// ---------------------------------------------------------------------------

void stealth_enter();
void stealth_exit();
bool stealth_active();

// Duress is OPT-IN and OFF by default: a stray shake can never trap the UI
// unless the user has explicitly armed it. Disarming also drops any active
// disguise so the watch can't get stuck.
void stealth_set_armed(bool on);
bool stealth_armed();

// Fed the per-sample accelerometer magnitude delta (in g) from motion_wake_poll;
// a burst of large deltas inside a short window trips the disguise.
void stealth_feed_accel_delta(float delta_g);

// Polled from loop(): runs the 4-second long-press exit detector.
void stealth_poll();
