#include "scan_screen.h"
#include "scan_engine.h"
#include "threat_radar.h"     // top-level follow score for the banner
#include "tools_screen.h"     // back-gesture target
#include <lvgl.h>
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

// ─── palette ─────────────────────────────────────────────────────────────────
static lv_color_t cat_color(uint8_t cat)
{
    switch (cat) {
        case TR_CAT_AIRTAG:   return lv_color_make(0xFF, 0x33, 0x55); // red — Apple tracker
        case TR_CAT_FLIPPER:  return lv_color_make(0xFF, 0x8C, 0x1A); // orange
        case TR_CAT_SKIMMER:  return lv_color_make(0xFF, 0xD0, 0x00); // amber
        case TR_CAT_FLOCK:    return lv_color_make(0xB2, 0x66, 0xFF); // violet — surveillance
        case TR_CAT_EVILTWIN: return lv_color_make(0xFF, 0x44, 0xAA); // magenta
        default:              return lv_color_make(0x33, 0xDD, 0x66); // green
    }
}

static const char *cat_short(uint8_t cat)
{
    switch (cat) {
        case TR_CAT_AIRTAG:   return "AirTag";
        case TR_CAT_FLIPPER:  return "Flipper";
        case TR_CAT_SKIMMER:  return "Skimmer";
        case TR_CAT_FLOCK:    return "Flock";
        case TR_CAT_EVILTWIN: return "Evil Twin";
        default:              return "Device";
    }
}

// ─── widgets ─────────────────────────────────────────────────────────────────
static lv_obj_t *s_screen      = nullptr;
static lv_obj_t *s_phase_dot   = nullptr;
static lv_obj_t *s_phase_lbl   = nullptr;
static lv_obj_t *s_banner      = nullptr;
static lv_obj_t *s_banner_lbl  = nullptr;
static lv_obj_t *s_badges      = nullptr;   // per-category pill row
static lv_obj_t *s_list        = nullptr;   // flagged-device cards
static lv_timer_t *s_timer     = nullptr;
static bool      s_active      = false;

// Big-number stat card: {value label, caption label, base colour, prev value}.
struct StatCard {
    lv_obj_t *num;
    lv_color_t base;
    int prev;
};
static StatCard s_wifi_card, s_ble_card, s_flag_card;

// Signature of the last list rebuild — skip the rebuild when nothing changed,
// so the list never flickers on a quiet tick.
static uint32_t s_list_sig = 0xFFFFFFFF;

// ─── stat card factory ───────────────────────────────────────────────────────
static void make_stat_card(lv_obj_t *parent, int x, StatCard *sc,
                           const char *caption, lv_color_t base)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 122, 96);
    lv_obj_set_style_bg_color(card, lv_color_make(0x0A, 0x0A, 0x0A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, base, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_TOP_MID, x, 108);

    lv_obj_t *num = lv_label_create(card);
    lv_obj_set_style_text_font(num, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(num, base, LV_PART_MAIN);
    lv_label_set_text(num, "0");
    lv_obj_align(num, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *cap = lv_label_create(card);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(cap, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(cap, caption);
    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, -10);

    sc->num  = num;
    sc->base = base;
    sc->prev = 0;
}

// Update a card's number; flash white for one tick when it climbs (a "pulse").
static void update_stat(StatCard *sc, int value)
{
    if (value != sc->prev) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", value);
        lv_label_set_text(sc->num, buf);
    }
    lv_color_t c = (value > sc->prev) ? lv_color_white() : sc->base;
    lv_obj_set_style_text_color(sc->num, c, LV_PART_MAIN);
    sc->prev = value;
}

// ─── per-category badge row ──────────────────────────────────────────────────
static void add_badge(uint8_t cat, int count)
{
    lv_color_t c = cat_color(cat);
    lv_obj_t *pill = lv_obj_create(s_badges);
    lv_obj_set_height(pill, 28);
    lv_obj_set_width(pill, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pill, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(pill, 14, LV_PART_MAIN);
    lv_obj_set_style_border_color(pill, c, LV_PART_MAIN);
    lv_obj_set_style_border_width(pill, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(pill, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(pill, 2, LV_PART_MAIN);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(pill);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, c, LV_PART_MAIN);
    lv_label_set_text_fmt(l, "%s %d", cat_short(cat), count);
    lv_obj_center(l);
}

// ─── flagged-device card ─────────────────────────────────────────────────────
static void add_dev_row(const ScanDev *d, uint32_t now)
{
    bool active = (now - d->last_ms) <= 60000;
    lv_color_t c = active ? cat_color(d->category) : lv_color_make(0x66, 0x66, 0x66);

    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, 384, 62);
    lv_obj_set_style_bg_color(row, lv_color_make(0x0A, 0x0A, 0x0A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, c, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 16, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Left accent stripe.
    lv_obj_t *bar = lv_obj_create(row);
    lv_obj_set_size(bar, 5, 40);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, c, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, active ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, -8, 0);

    // Headline: "AirTag · <name>"  (falls back to MAC tail when nameless).
    lv_obj_t *head = lv_label_create(row);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(head, c, LV_PART_MAIN);
    lv_label_set_long_mode(head, LV_LABEL_LONG_DOT);
    lv_obj_set_width(head, 300);
    if (d->name[0])
        lv_label_set_text_fmt(head, "%s  " LV_SYMBOL_RIGHT "  %s",
                              cat_short(d->category), d->name);
    else
        lv_label_set_text_fmt(head, "%s  " LV_SYMBOL_RIGHT "  ..%02X:%02X:%02X",
                              cat_short(d->category), d->mac[3], d->mac[4], d->mac[5]);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 6, 2);

    // Footer: radio · signal · hits.
    lv_obj_t *info = lv_label_create(row);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(info, lv_color_make(0x00, 0xBB, 0x66), LV_PART_MAIN);
    lv_label_set_text_fmt(info, "%s  ·  %d dBm  ·  %ux",
                          d->kind == SCAN_KIND_WIFI ? LV_SYMBOL_WIFI : LV_SYMBOL_BLUETOOTH,
                          (int)d->best_rssi, (unsigned)d->hits);
    lv_obj_align(info, LV_ALIGN_BOTTOM_LEFT, 6, -2);

    // Signal strength pip on the right (-100..-40 dBm -> 4 bars).
    int bars = (d->best_rssi + 100) / 15;      // ~0..4
    if (bars < 0) bars = 0; if (bars > 4) bars = 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *p = lv_obj_create(row);
        lv_obj_set_size(p, 6, 8 + i * 6);
        lv_obj_set_style_radius(p, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(p, c, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(p, i < bars ? LV_OPA_COVER : LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN);
        lv_obj_align(p, LV_ALIGN_RIGHT_MID, -4 - (3 - i) * 9, 8);
    }
}

// ─── refresh ─────────────────────────────────────────────────────────────────
static void refresh()
{
    scan_engine_tick();          // drain queued radio hits into the store FIRST
    ScanStats st;
    scan_engine_stats(&st);
    uint32_t now = millis();

    // Phase indicator.
    bool wifi = scan_engine_wifi_phase();
    lv_color_t pc = wifi ? lv_color_make(0x22, 0xDD, 0xEE)   // cyan for WiFi
                         : lv_color_make(0xB2, 0x66, 0xFF);  // violet for BLE
    lv_obj_set_style_bg_color(s_phase_dot, pc, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_phase_lbl, pc, LV_PART_MAIN);
    lv_label_set_text(s_phase_lbl, wifi ? "WiFi" : "BLE");

    // Big counters (with pulse).
    update_stat(&s_wifi_card, st.wifi);
    update_stat(&s_ble_card,  st.ble);
    update_stat(&s_flag_card, st.threats);

    // Threat banner reflects the worst FOLLOWING contact (co-moving), not just
    // a static sighting — that's the question that matters on the move.
    int top = threatradar_top_level();
    lv_color_t bcol; const char *btext;
    if (top >= TR_LVL_LIKELY) {
        bcol = lv_color_make(0xFF, 0x22, 0x22);
        btext = LV_SYMBOL_WARNING "  QUALCUNO TI STA SEGUENDO";
    } else if (top == TR_LVL_POSSIBLE) {
        bcol = lv_color_make(0xCC, 0x88, 0x00);
        btext = LV_SYMBOL_EYE_OPEN "  Possibile pedinamento";
    } else if (st.threats > 0) {
        bcol = lv_color_make(0x22, 0x44, 0x00);
        btext = LV_SYMBOL_EYE_OPEN "  Dispositivi segnalati nei paraggi";
    } else {
        bcol = lv_color_make(0x00, 0x55, 0x2A);
        btext = LV_SYMBOL_OK "  Aria pulita";
    }
    lv_obj_set_style_bg_color(s_banner, bcol, LV_PART_MAIN);
    lv_label_set_text(s_banner_lbl, btext);

    // Category badges — only the ones that actually fired.
    lv_obj_clean(s_badges);
    for (int cat = 0; cat < TR_CAT_COUNT; cat++)
        if (cat != TR_CAT_VEHICLE && st.per_cat[cat] > 0)
            add_badge((uint8_t)cat, st.per_cat[cat]);

    // Flagged-device list. Rebuild only when the flagged set actually changed
    // (cheap signature) so a steady scene never flickers.
    ScanDev devs[16];
    int nd = scan_engine_get_devices(devs, 16);
    uint32_t sig = (uint32_t)nd * 2654435761u;
    for (int i = 0; i < nd; i++)
        sig ^= (uint32_t)(devs[i].mac[5] ^ (devs[i].category << 4) ^
                          (uint8_t)devs[i].best_rssi) << ((i % 4) * 8);
    if (sig != s_list_sig) {
        s_list_sig = sig;
        lv_obj_clean(s_list);
        if (nd == 0) {
            lv_obj_t *empty = lv_label_create(s_list);
            lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, LV_PART_MAIN);
            lv_obj_set_style_text_color(empty, lv_color_make(0x00, 0x77, 0x44), LV_PART_MAIN);
            lv_label_set_text(empty,
                "Nessun dispositivo segnalato.\n\n"
                "WiFi e BLE vengono scansionati\n"
                "a turno: AirTag, Flipper, skimmer,\n"
                "telecamere Flock ed evil-twin\n"
                "appariranno qui appena rilevati.");
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        } else {
            for (int i = 0; i < nd; i++) add_dev_row(&devs[i], now);
        }
    }
}

static void on_timer(lv_timer_t *) { if (s_active) refresh(); }

static void on_clear(lv_event_t *)
{
    scan_engine_reset();
    s_wifi_card.prev = s_ble_card.prev = s_flag_card.prev = 0;
    s_list_sig = 0xFFFFFFFF;
    refresh();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        scan_engine_stop();      // radios down when we leave
        tools_screen_show();
    }
}

// ─── build ───────────────────────────────────────────────────────────────────
void scan_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    // Title.
    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(0x33, 0xFF, 0x88), LV_PART_MAIN);
    lv_label_set_text(title, "SCANNER");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 22);

    // Phase indicator (top-right): a coloured dot + "WiFi"/"BLE".
    s_phase_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_phase_lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_label_set_text(s_phase_lbl, "WiFi");
    lv_obj_align(s_phase_lbl, LV_ALIGN_TOP_RIGHT, -20, 28);

    s_phase_dot = lv_obj_create(s_screen);
    lv_obj_set_size(s_phase_dot, 14, 14);
    lv_obj_set_style_radius(s_phase_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_phase_dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_phase_dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_phase_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(s_phase_dot, s_phase_lbl, LV_ALIGN_OUT_LEFT_MID, -8, 0);

    // Threat banner.
    s_banner = lv_obj_create(s_screen);
    lv_obj_set_size(s_banner, 388, 38);
    lv_obj_set_style_radius(s_banner, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_banner, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_banner, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 62);
    s_banner_lbl = lv_label_create(s_banner);
    lv_obj_set_style_text_font(s_banner_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_banner_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(s_banner_lbl);

    // Three big stat cards.
    make_stat_card(s_screen, -132, &s_wifi_card, LV_SYMBOL_WIFI "  WiFi",
                   lv_color_make(0x22, 0xDD, 0xEE));
    make_stat_card(s_screen, 0,    &s_ble_card,  LV_SYMBOL_BLUETOOTH "  BLE",
                   lv_color_make(0x66, 0x99, 0xFF));
    make_stat_card(s_screen, 132,  &s_flag_card, LV_SYMBOL_WARNING "  FLAGS",
                   lv_color_make(0xFF, 0x9A, 0x22));

    // Category badge row (flex, wraps).
    s_badges = lv_obj_create(s_screen);
    lv_obj_set_size(s_badges, 400, 34);
    lv_obj_set_style_bg_opa(s_badges, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_badges, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_badges, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_badges, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_badges, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_badges, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_badges, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_badges, LV_ALIGN_TOP_MID, 0, 214);

    // Flagged-device list.
    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, 404, 190);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 252);

    // CLEAR button.
    lv_obj_t *clr = lv_obj_create(s_screen);
    lv_obj_set_size(clr, 150, 40);
    lv_obj_set_style_radius(clr, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(clr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(clr, lv_color_make(0x00, 0x66, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(clr, 1, LV_PART_MAIN);
    lv_obj_clear_flag(clr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(clr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(clr, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_event_cb(clr, on_clear, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clr_lbl = lv_label_create(clr);
    lv_obj_set_style_text_font(clr_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(clr_lbl, lv_color_make(0x00, 0xCC, 0x00), LV_PART_MAIN);
    lv_label_set_text(clr_lbl, LV_SYMBOL_TRASH "  CLEAR");
    lv_obj_center(clr_lbl);
    lv_obj_add_flag(clr_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_timer = lv_timer_create(on_timer, 400, NULL);
}

void scan_screen_show()
{
    if (!s_screen) scan_screen_create();
    s_active = true;
    s_wifi_card.prev = s_ble_card.prev = s_flag_card.prev = 0;
    s_list_sig = 0xFFFFFFFF;
    scan_engine_start();     // radios up, store cleared
    refresh();
    lv_scr_load(s_screen);
}

bool scan_screen_is_active() { return s_active; }
