#include "waterfall_screen.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"

// Defined in tools_screen.cpp
void tools_screen_show();

// ---- Palette IDENTICA a ESP32-DIV / C5 (128 colori RGB565) ------------------
static uint8_t wf_pr[128], wf_pg[128], wf_pb[128];
static void wf_buildPalette()
{
    for (int i = 0;  i < 32;  i++) { wf_pr[i] = i / 2; wf_pg[i] = 0;            wf_pb[i] = i;      }
    for (int i = 32; i < 64;  i++) { wf_pr[i] = i / 2; wf_pg[i] = 0;            wf_pb[i] = 63 - i; }
    for (int i = 64; i < 96;  i++) { wf_pr[i] = 31;    wf_pg[i] = (i - 64) * 2; wf_pb[i] = 0;      }
    for (int i = 96; i < 128; i++) { wf_pr[i] = 31;    wf_pg[i] = 63;           wf_pb[i] = i - 96; }
}
static inline uint16_t wf_color(uint8_t k)
{
    if (k > 127) k = 127;
    return ((uint16_t)wf_pr[k] << 11) | ((uint16_t)wf_pg[k] << 5) | wf_pb[k];
}

// ---- FFT 256pt (identica al C5, radix-2 in-place) ---------------------------
static void wf_fft256(float *re, float *im)
{
    const int N = 256;
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -2.0f * PI / len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < N; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < (len >> 1); k++) {
                int a = i + k, b = a + (len >> 1);
                float tr = cr * re[b] - ci * im[b];
                float ti = cr * im[b] + ci * re[b];
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                float ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

// ---- packet counter (promiscuous cb) ---------------------------------------
static volatile uint32_t s_pkts = 0;
static void wf_promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)buf; (void)type;
    s_pkts++;
}

// ---- layout (adattato al 410x502 del T-Watch, stile identico al C5) ---------
#define CV_W    410
#define CV_H    434
#define CV_SY   62            // canvas screen-y
#define AG_Y    4             // area-graph local-y
#define AG_H    82
#define WF_Y0   102           // waterfall local-y start
#define WF_MAXY (CV_H - 2)
#define CENTER  205
#define HALF    205           // bin per lato (mirror) — 128 bin FFT stirati su 205

static lv_obj_t  *screen, *title_lbl, *info_lbl, *canvas;
static uint16_t  *fb = nullptr;          // buffer canvas RGB565 (PSRAM)
static lv_timer_t *s_timer = nullptr;
static bool       s_active = false;
static int        s_epoch = 0;
static wifi_mode_t s_prev_mode = WIFI_MODE_NULL;
static uint32_t   s_lastPkt = 0, s_lastShow = 0;

// 2.4GHz only: l'ESP32-S3 non ha i canali 5GHz del C5.
static const uint8_t fftChans[] = { 1,2,3,4,5,6,7,8,9,10,11,12,13 };
static const int nCh = (int)(sizeof(fftChans) / sizeof(fftChans[0]));
static int s_chIdx = 5;      // ch 6 di default

static inline void px(int x, int y, uint16_t c)
{
    if ((unsigned)x < CV_W && (unsigned)y < CV_H) fb[y * CV_W + x] = c;
}

// Invalidate only a local rectangle of the canvas (in canvas pixel coords)
// instead of the whole 355 KB buffer. Direct fb writes don't mark LVGL dirty,
// so we push just the changed band to the display — a fraction of the PSRAM
// blit bandwidth, which is what was starving the refresh and making the top
// area-graph flicker/vanish.
static void invalidate_canvas_rect(int lx, int ly, int lw, int lh)
{
    if (!canvas) return;
    lv_area_t c;
    lv_obj_get_coords(canvas, &c);
    lv_area_t a;
    a.x1 = c.x1 + lx;
    a.y1 = c.y1 + ly;
    a.x2 = c.x1 + lx + lw - 1;
    a.y2 = c.y1 + ly + lh - 1;
    lv_obj_invalidate_area(canvas, &a);
}

// ---- WiFi promiscuous (stesso schema di analyze_screen) ---------------------
static void start_wifi()
{
    s_prev_mode = WiFi.getMode();
    s_pkts = 0;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(wf_promisc_cb);
    esp_wifi_set_channel(fftChans[s_chIdx], WIFI_SECOND_CHAN_NONE);
}

static void stop_wifi()
{
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_set_promiscuous(false);
    if (s_prev_mode == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_OFF);
        esp_wifi_stop();
        esp_wifi_deinit();
    } else {
        WiFi.mode(s_prev_mode);
    }
    s_prev_mode = WIFI_MODE_NULL;
}

// ---- un frame: campiona 256x @5kHz -> FFT -> area-graph + riga waterfall -----
static void do_frame()
{
    static float re[256], im[256];

    // campiona il contatore pacchetti 256 volte a 200us (5 kHz), come il C5
    uint32_t t = micros();
    for (int i = 0; i < 256; i++) {
        re[i] = (float)s_pkts * 300.0f;
        im[i] = 0.0f;
        while ((int32_t)(micros() - t) < 200) { }
        t += 200;
    }
    float mean = 0; for (int i = 0; i < 256; i++) mean += re[i];
    mean /= 256.0f;
    for (int i = 0; i < 256; i++) re[i] -= mean;
    wf_fft256(re, im);

    // pulisci l'area-graph (in alto)
    for (int y = AG_Y; y < AG_Y + AG_H; y++) {
        uint16_t *row = &fb[y * CV_W];
        for (int x = 0; x < CV_W; x++) row[x] = 0;
    }

    int wy = WF_Y0 + s_epoch;
    for (int x = 0; x < HALF; x++) {
        int bin = x * 128 / HALF;            // 128 bin FFT stirati sui 205 px/lato
        if (bin > 127) bin = 127;
        float mag = sqrtf(re[bin] * re[bin] + im[bin] * im[bin]);
        int k = (int)(mag / 10.0f);          // attenuation = 10 (identico)
        if (k > 127) k = 127; if (k < 0) k = 0;
        uint16_t col = wf_color((uint8_t)k);

        // waterfall: riga con lo spettro a specchio attorno al centro
        px(CENTER + x, wy, col);
        px(CENTER - x, wy, col);

        // area-graph: barre a specchio (spettro live in alto)
        int h = k * AG_H / 127;
        for (int yy = AG_Y + AG_H - h; yy < AG_Y + AG_H; yy++) {
            px(CENTER + x, yy, col);
            px(CENTER - x, yy, col);
        }
    }

    s_epoch++;
    if (WF_Y0 + s_epoch >= WF_MAXY) s_epoch = 0;

    // Only the area-graph band (fully redrawn each frame) and the one freshly
    // drawn waterfall line changed — repaint just those, not all 355 KB.
    invalidate_canvas_rect(0, AG_Y, CV_W, AG_H);
    invalidate_canvas_rect(0, wy, CV_W, 1);
}

static void on_timer(lv_timer_t *)
{
    if (!s_active || lv_screen_active() != screen) return;
    do_frame();
    if (millis() - s_lastShow > 500) {
        lv_label_set_text_fmt(info_lbl, "Ch %d (2.4)   pkt %lu   tap = canale",
                              fftChans[s_chIdx], (unsigned long)(s_pkts - s_lastPkt));
        s_lastPkt = s_pkts;
        s_lastShow = millis();
    }
}

// ---- eventi -----------------------------------------------------------------
static void on_canvas_click(lv_event_t *)
{
    s_chIdx = (s_chIdx + 1) % nCh;
    esp_wifi_set_channel(fftChans[s_chIdx], WIFI_SECOND_CHAN_NONE);
    s_epoch = 0;
    if (fb) memset(fb, 0, (size_t)CV_W * CV_H * 2);
    lv_obj_invalidate(canvas);
    lv_label_set_text_fmt(title_lbl, "Analizzatore Banda  Ch %d", fftChans[s_chIdx]);
    s_lastPkt = s_pkts;
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        waterfall_screen_stop();
        tools_screen_show();
    }
}

// ---- layout -----------------------------------------------------------------
void waterfall_screen_create()
{
    wf_buildPalette();

    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);

    title_lbl = lv_label_create(screen);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_lbl, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_label_set_text(title_lbl, "Analizzatore Banda  Ch 6");
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 8);

    info_lbl = lv_label_create(screen);
    lv_obj_set_style_text_font(info_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(info_lbl, lv_color_make(0x00, 0x88, 0x00), LV_PART_MAIN);
    lv_label_set_text(info_lbl, "tap = canale   swipe su = esci");
    lv_obj_align(info_lbl, LV_ALIGN_TOP_MID, 0, 36);

    // buffer canvas in PSRAM (355 KB), scritto direttamente come un framebuffer
    if (!fb) fb = (uint16_t *)heap_caps_malloc((size_t)CV_W * CV_H * 2, MALLOC_CAP_SPIRAM);

    canvas = lv_canvas_create(screen);
    if (fb) {
        memset(fb, 0, (size_t)CV_W * CV_H * 2);
        lv_canvas_set_buffer(canvas, fb, CV_W, CV_H, LV_COLOR_FORMAT_RGB565);
    }
    lv_obj_align(canvas, LV_ALIGN_TOP_MID, 0, CV_SY);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(canvas, on_canvas_click, LV_EVENT_CLICKED, NULL);

    s_timer = lv_timer_create(on_timer, 30, NULL);
}

void waterfall_screen_show()
{
    if (fb) memset(fb, 0, (size_t)CV_W * CV_H * 2);
    s_epoch = 0;
    s_lastPkt = 0;
    s_lastShow = millis();
    start_wifi();
    s_active = true;
    lv_obj_invalidate(canvas);
    lv_scr_load(screen);
}

void waterfall_screen_stop()
{
    if (!s_active) return;
    s_active = false;
    stop_wifi();
}

bool waterfall_screen_is_active()
{
    return lv_screen_active() == screen;
}
