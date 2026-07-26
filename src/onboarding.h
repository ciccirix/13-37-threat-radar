#pragma once
#include <lvgl.h>

// One-time onboarding shown on the FIRST boot after a flash: a bilingual
// authorized-use disclaimer the user must accept, plus an OPTIONAL email for
// update notifications. On accept it stores consent in NVS (so it never shows
// again) and, when WiFi is later available, sends a single install ping (a
// random anonymous install id + the optional email) to the configured endpoint.
bool onboarding_needed();          // true until the user has accepted once
void onboarding_screen_create();
void onboarding_screen_show();
void onboarding_bg_tick();          // call from loop(): sends the install ping when online
