#pragma once
#include <lvgl.h>

// Dedicated deauther UI: scan for APs, tap one to target, START/STOP the flood.
void deauther_screen_create();
void deauther_screen_show();
void deauther_screen_stop();        // stop attack + radio cleanup (back-button exit)
bool deauther_screen_is_active();
