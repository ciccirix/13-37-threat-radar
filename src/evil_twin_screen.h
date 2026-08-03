#pragma once

// Evil Twin panel — scan for APs, pick one, stand up a cloning rogue AP with a
// captive portal, and watch captured credentials land live. Replaces the old
// standalone evil-twin *detector* tile (that classifier now runs inside the
// unified Scanner).
void evil_twin_screen_show();
void evil_twin_screen_create();
void evil_twin_screen_stop();
bool evil_twin_screen_is_active();
