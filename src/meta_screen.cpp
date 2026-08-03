#include "meta_screen.h"
#include "meta_glasses.h"
#include "tools_screen.h"
#include <lvgl.h>
#include <SD.h>
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_screen = nullptr;
static lv_obj_t *s_status = nullptr;
static lv_obj_t *s_list   = nullptr;
static lv_timer_t *s_timer = nullptr;
static bool      s_active = false;
static uint32_t  s_sig = 0xFFFFFFFF;

// ─── model photo (loaded from SD on tap) ─────────────────────────────────────
// Files live on the microSD as raw RGB565 with a tiny header:
//   /meta/<family>.bin  =  [w:u16 LE][h:u16 LE][ w*h*2 bytes RGB565 LE ]
// family is one of: rayban, oakley, quest, meta  (fallback: glasses).
static lv_obj_t     *s_detail   = nullptr;   // full-screen overlay
static lv_obj_t     *s_detail_img = nullptr;
static lv_obj_t     *s_detail_lbl = nullptr;
static lv_obj_t     *s_detail_sub = nullptr;
static uint8_t      *s_img_buf  = nullptr;   // PSRAM pixel buffer
static lv_image_dsc_t s_img_dsc;

// Which glasses family did the matched identifier point at?
static const char *model_name(uint16_t id)
{
    switch (id) {
        case 0x0D53: return "Ray-Ban / Oakley";   // Luxottica
        case 0x01AB: return "Meta";
        case 0x058E: return "Meta / Quest";
        case 0xFD5F: return "Meta / Quest";
        case 0xFEB7:
        case 0xFEB8: return "Meta";
        default:     return "Meta glasses";
    }
}

// Map the matched identifier to a photo family (SD filename stem).
static const char *family_file(uint16_t id)
{
    switch (id) {
        case 0x058E: case 0xFD5F: return "quest";
        case 0x0D53:              return "rayban";  // Luxottica (Ray-Ban / Oakley)
        default:                  return "meta";    // 0x01AB / 0xFEB7 / 0xFEB8
    }
}

static void free_photo()
{
    if (s_img_buf) { heap_caps_free(s_img_buf); s_img_buf = nullptr; }
}

// Load /meta/<name>.bin ([w:u16][h:u16][RGB565…]) into s_img_dsc. False if absent.
static bool load_photo(const char *name)
{
    free_photo();
    char path[48];
    snprintf(path, sizeof(path), "/meta/%s.bin", name);
    if (!SD.exists(path)) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint8_t hdr[4];
    if (f.read(hdr, 4) != 4) { f.close(); return false; }
    uint16_t w = hdr[0] | (hdr[1] << 8);
    uint16_t h = hdr[2] | (hdr[3] << 8);
    size_t px = (size_t)w * h * 2;
    if (!w || !h || w > 410 || h > 460) { f.close(); return false; }
    s_img_buf = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM);
    if (!s_img_buf) { f.close(); return false; }
    size_t rd = f.read(s_img_buf, px);
    f.close();
    if (rd != px) { free_photo(); return false; }
    memset(&s_img_dsc, 0, sizeof(s_img_dsc));
    s_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w      = w;
    s_img_dsc.header.h      = h;
    s_img_dsc.header.stride = w * 2;
    s_img_dsc.data_size     = px;
    s_img_dsc.data          = s_img_buf;
    return true;
}

static void hide_detail()
{
    if (!s_detail) return;
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_detail_img, NULL);
    free_photo();
}

static void show_detail(uint16_t id)
{
    const char *fam = family_file(id);
    bool ok = load_photo(fam);
    if (ok) {
        lv_image_set_src(s_detail_img, &s_img_dsc);
        lv_obj_clear_flag(s_detail_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_detail_img, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_detail_lbl, model_name(id));
    if (ok) lv_label_set_text_fmt(s_detail_sub, "0x%04X  ·  tocca per chiudere", id);
    else    lv_label_set_text_fmt(s_detail_sub, "manca /meta/%s.bin sulla SD", fam);
    lv_obj_move_foreground(s_detail);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
}

static void on_row_click(lv_event_t *e)
{
    uint16_t id = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    show_detail(id);
}

static void add_row(const MetaHit *h)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, 384, 60);
    lv_obj_set_style_bg_color(row, lv_color_make(0x0A, 0x0A, 0x12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_make(0x33, 0x66, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);   // tap → model photo
    lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, (void *)(uintptr_t)h->id);

    lv_obj_t *head = lv_label_create(row);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(head, lv_color_make(0x66, 0x99, 0xFF), LV_PART_MAIN);
    lv_label_set_text_fmt(head, LV_SYMBOL_EYE_OPEN "  %s", model_name(h->id));
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 4, 2);

    lv_obj_t *info = lv_label_create(row);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(info, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text_fmt(info, "%d dBm  ·  id 0x%04X  ·  ..%02X:%02X:%02X  ·  %ux",
        (int)h->rssi, h->id, h->mac[3], h->mac[4], h->mac[5], (unsigned)h->hits);
    lv_obj_align(info, LV_ALIGN_BOTTOM_LEFT, 4, -2);
}

static void refresh()
{
    int n = meta_glasses_count();
    lv_label_set_text_fmt(s_status,
        n > 0 ? LV_SYMBOL_EYE_OPEN "  %d occhiali smart rilevati"
              : LV_SYMBOL_REFRESH "  Scansione BLE...  (%d)", n);

    MetaHit hits[16];
    int got = meta_glasses_get(hits, 16);
    uint32_t sig = (uint32_t)got * 2654435761u;
    for (int i = 0; i < got; i++)
        sig ^= (uint32_t)(hits[i].mac[5] ^ (uint8_t)hits[i].rssi ^ hits[i].id) << ((i % 4) * 8);
    if (sig == s_sig) return;
    s_sig = sig;

    lv_obj_clean(s_list);
    if (got == 0) {
        lv_obj_t *e = lv_label_create(s_list);
        lv_obj_set_style_text_font(e, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(e, lv_color_make(0x55, 0x66, 0x99), LV_PART_MAIN);
        lv_label_set_text(e,
            "Nessun occhiale smart nei paraggi.\n\n"
            "Rileva Ray-Ban Stories/Meta,\n"
            "Oakley Meta e Quest via BLE\n"
            "(company id Meta/Luxottica).");
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        return;
    }
    for (int i = 0; i < got; i++) add_row(&hits[i]);
}

static void on_timer(lv_timer_t *) { if (s_active) refresh(); }

static void on_clear(lv_event_t *) { meta_glasses_reset(); s_sig = 0xFFFFFFFF; refresh(); }

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        if (s_detail && !lv_obj_has_flag(s_detail, LV_OBJ_FLAG_HIDDEN)) {
            hide_detail();   // photo card open → close it first
            return;
        }
        s_active = false;
        meta_glasses_stop();
        tools_screen_show();
    }
}

void meta_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(0x66, 0x99, 0xFF), LV_PART_MAIN);
    lv_label_set_text(title, "META GLASSES");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    s_status = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(s_status, LV_SYMBOL_REFRESH "  Scansione BLE...");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 60);

    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, 404, 340);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *clr = lv_obj_create(s_screen);
    lv_obj_set_size(clr, 150, 40);
    lv_obj_set_style_radius(clr, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(clr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(clr, lv_color_make(0x33, 0x66, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(clr, 1, LV_PART_MAIN);
    lv_obj_clear_flag(clr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(clr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(clr, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_event_cb(clr, on_clear, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(clr);
    lv_obj_set_style_text_font(clbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(clbl, lv_color_make(0x66, 0x99, 0xFF), LV_PART_MAIN);
    lv_label_set_text(clbl, LV_SYMBOL_TRASH "  CLEAR");
    lv_obj_center(clbl);
    lv_obj_add_flag(clbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Photo detail overlay — full screen, tap anywhere to close.
    s_detail = lv_obj_create(s_screen);
    lv_obj_set_size(s_detail, 410, 502);
    lv_obj_align(s_detail, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_detail, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_detail, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_detail, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_detail, [](lv_event_t *) { hide_detail(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);

    s_detail_lbl = lv_label_create(s_detail);
    lv_obj_set_style_text_font(s_detail_lbl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_detail_lbl, lv_color_make(0x66, 0x99, 0xFF), LV_PART_MAIN);
    lv_label_set_text(s_detail_lbl, "");
    lv_obj_align(s_detail_lbl, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_add_flag(s_detail_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_detail_img = lv_image_create(s_detail);
    lv_obj_align(s_detail_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_detail_img, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_detail_sub = lv_label_create(s_detail);
    lv_obj_set_style_text_font(s_detail_sub, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_detail_sub, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(s_detail_sub, "");
    lv_obj_align(s_detail_sub, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_flag(s_detail_sub, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_timer = lv_timer_create(on_timer, 500, NULL);
}

void meta_screen_show()
{
    if (!s_screen) meta_screen_create();
    s_active = true;
    s_sig = 0xFFFFFFFF;
    meta_glasses_start();
    refresh();
    lv_scr_load(s_screen);
}

bool meta_screen_is_active() { return s_active; }
