#pragma once

// Live Meta / smart-glasses detector screen. Opening it starts a BLE scan fed
// into meta_glasses_check(); leaving it (swipe up) stops the scan.
void meta_screen_show();
void meta_screen_create();
bool meta_screen_is_active();
