#pragma once
#include <lvgl.h>

// "Analizzatore Banda" — port IDENTICO del waterfall FFT del Marauder C5
// (replica ESP32-DIV/cifertech): FFT 256pt della serie temporale del contatore
// pacchetti su un canale fisso, spettro a specchio + area-graph + waterfall che
// scorre, palette identica. 2.4GHz only (l'ESP32-S3 non ha i 5GHz del C5).
void waterfall_screen_create();
void waterfall_screen_show();
void waterfall_screen_stop();     // rilascia il promiscuous (cleanup radio)
bool waterfall_screen_is_active();
