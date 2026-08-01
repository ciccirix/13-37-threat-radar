#pragma once
#include <stdint.h>

// Fire the garage/gate remote code once (best-effort OOK on the SX1262).
// Returns false if the shared radio is busy (LoRa/APRS/pager) or setup failed.
bool    garage_transmit();
int16_t garage_last_error();
