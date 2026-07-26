#include "onboarding.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_system.h"
#include <string.h>
#include <stdio.h>

// Defined in main.cpp
void clock_screen_show();

// --- backend endpoint (optional telemetry) -----------------------------------
// Leave ONBOARD_ENABLE at 0 and the disclaimer still shows + stores consent, but
// nothing is sent and NO TLS/HTTP code is linked (no flash cost). To turn on the
// anonymous install ping + optional email upload: deploy backend/onboard.gs, set
// ONBOARD_ENABLE to 1, and paste its /exec URL into ONBOARD_ENDPOINT.
#define ONBOARD_ENABLE   0
#define ONBOARD_ENDPOINT ""
#define ONBOARD_FW       "13-37"

#if ONBOARD_ENABLE
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

// --- bilingual authorized-use disclaimer -------------------------------------
static const char *DISCLAIMER =
"IT\n"
"13:37 e' uno strumento di RICERCA sulla SICUREZZA e a scopo EDUCATIVO. Include "
"funzioni radio (WiFi, Bluetooth, LoRa) che possono interferire con reti e "
"dispositivi.\n\n"
"USALO SOLO su apparecchiature, reti e sistemi di tua PROPRIETA' o per i quali "
"hai un'AUTORIZZAZIONE ESPLICITA e SCRITTA a effettuare test. L'uso non "
"autorizzato (deauth, intercettazione, scansione di reti o telecamere altrui) "
"puo' costituire REATO secondo la legge.\n\n"
"Sei l'UNICO RESPONSABILE dell'uso che ne fai. L'autore NON e' responsabile di "
"danni, perdite o usi illeciti. Software fornito \"COSI' COM'E'\", SENZA "
"GARANZIE.\n\n"
"Procedendo dichiari di aver letto e compreso, di avere l'eta' legale e di "
"accettare questi termini e le leggi applicabili.\n\n"
"Privacy: l'email (facoltativa) sara' usata SOLO per notificarti gli "
"aggiornamenti; puoi chiederne la cancellazione. Un ID installazione casuale e "
"anonimo puo' essere inviato per contare le installazioni. Nessun altro dato.\n\n"
"------------------------------\n\n"
"EN\n"
"13:37 is a SECURITY RESEARCH and EDUCATIONAL tool. It includes radio features "
"(WiFi, Bluetooth, LoRa) that can interfere with networks and devices.\n\n"
"USE IT ONLY on equipment, networks and systems you OWN or for which you have "
"EXPLICIT, WRITTEN AUTHORIZATION to test. Unauthorized use (deauth, "
"interception, scanning of networks or cameras you don't own) may be a CRIMINAL "
"OFFENSE under applicable law.\n\n"
"You are SOLELY RESPONSIBLE for how you use it. The author is NOT liable for any "
"damage, loss or unlawful use. Software provided \"AS IS\", WITHOUT WARRANTY.\n\n"
"By continuing you confirm you have read and understood this, are of legal age, "
"and accept these terms and all applicable laws.\n\n"
"Privacy: the email (optional) is used ONLY to notify you of updates; you may "
"request deletion. A random, anonymous install id may be sent to count "
"installs. No other data is collected.\n\nCALMATI";

static lv_obj_t *screen, *cb, *email_ta, *keyboard, *hint;
static Preferences s_prefs;
static char s_iid[20]  = {0};
static char s_email[64] = {0};
static bool s_consent  = false;
static bool s_pinged   = true;      // assume sent until we know otherwise
static volatile bool s_sending = false;

// --- NVS ----------------------------------------------------------------------
bool onboarding_needed()
{
    s_prefs.begin("onboard", true);
    bool done = s_prefs.getBool("done", false);
    s_prefs.end();
    return !done;
}

static void gen_install_id(char *out)   // 16 hex chars, random & anonymous
{
    for (int i = 0; i < 16; i += 8) {
        uint32_t r = esp_random();
        snprintf(out + i, 9, "%08x", (unsigned)r);
    }
}

// --- install ping (one-shot task) --------------------------------------------
#if ONBOARD_ENABLE
static void send_task(void *)
{
    char body[300];
    snprintf(body, sizeof(body),
             "{\"install_id\":\"%s\",\"email\":\"%s\",\"fw\":\"%s\",\"event\":\"install\"}",
             s_iid, s_email, ONBOARD_FW);

    WiFiClientSecure cli;
    cli.setInsecure();                       // telemetry only; skip cert pinning
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // Apps Script 302 -> googleusercontent
    http.setTimeout(9000);
    if (http.begin(cli, ONBOARD_ENDPOINT)) {
        http.addHeader("Content-Type", "application/json");
        int code = http.POST((uint8_t *)body, strlen(body));
        if (code > 0 && code < 400) {
            s_pinged = true;
            s_prefs.begin("onboard", false);
            s_prefs.putBool("pinged", true);
            s_prefs.end();
        }
        http.end();
    }
    s_sending = false;
    vTaskDelete(nullptr);
}
#endif  // ONBOARD_ENABLE

void onboarding_bg_tick()
{
#if ONBOARD_ENABLE
    if (s_pinged || s_sending) return;
    if (WiFi.status() != WL_CONNECTED) return;
    s_sending = true;
    xTaskCreatePinnedToCore(send_task, "onboard_ping", 8192, nullptr, 1, nullptr, 0);
#endif
}

// --- screen ------------------------------------------------------------------
static void on_email_focus(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(keyboard, email_ta);
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (c == LV_EVENT_DEFOCUSED || c == LV_EVENT_READY || c == LV_EVENT_CANCEL) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_accept(lv_event_t *)
{
    if (!lv_obj_has_state(cb, LV_STATE_CHECKED)) {
        lv_label_set_text(hint, "Spunta la conferma per procedere / tick the box to continue");
        lv_obj_set_style_text_color(hint, lv_color_make(0xFF, 0x66, 0x44), LV_PART_MAIN);
        return;
    }
    const char *em = lv_textarea_get_text(email_ta);
    strncpy(s_email, (em && strchr(em, '@')) ? em : "", sizeof(s_email) - 1);
    if (!s_iid[0]) gen_install_id(s_iid);

    s_prefs.begin("onboard", false);
    s_prefs.putBool("done", true);
    s_prefs.putString("iid", s_iid);
    s_prefs.putString("email", s_email);
    s_prefs.putBool("pinged", false);
    s_prefs.end();

    s_consent = true;
    s_pinged  = false;               // let bg_tick send the ping once online
    clock_screen_show();
}

void onboarding_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(0xFF, 0xAA, 0x33), LV_PART_MAIN);
    lv_label_set_text(title, "USO AUTORIZZATO / AUTHORIZED USE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // scrollable disclaimer
    lv_obj_t *box = lv_obj_create(screen);
    lv_obj_set_size(box, 404, 286);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(box, lv_color_make(0x0d, 0x0f, 0x12), LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 10, LV_PART_MAIN);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_t *txt = lv_label_create(box);
    lv_obj_set_width(txt, 380);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(txt, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_label_set_text(txt, DISCLAIMER);

    cb = lv_checkbox_create(screen);
    lv_checkbox_set_text(cb, "Confermo l'uso autorizzato / I confirm authorized use");
    lv_obj_set_style_text_font(cb, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(cb, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(cb, LV_ALIGN_TOP_MID, 0, 334);

    lv_obj_t *el = lv_label_create(screen);
    lv_obj_set_style_text_font(el, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(el, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(el, "Email per aggiornamenti (opzionale) / updates email (optional):");
    lv_obj_align(el, LV_ALIGN_TOP_MID, 0, 366);

    email_ta = lv_textarea_create(screen);
    lv_textarea_set_one_line(email_ta, true);
    lv_textarea_set_placeholder_text(email_ta, "you@example.com");
    lv_obj_set_size(email_ta, 404, 42);
    lv_obj_align(email_ta, LV_ALIGN_TOP_MID, 0, 388);
    lv_obj_set_style_bg_color(email_ta, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_text_color(email_ta, lv_color_white(), LV_PART_MAIN);
    lv_obj_add_event_cb(email_ta, on_email_focus, LV_EVENT_ALL, NULL);

    hint = lv_label_create(screen);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(hint, "");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 436);

    lv_obj_t *btn = lv_obj_create(screen);
    lv_obj_set_size(btn, 260, 46);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x00, 0x88, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_accept, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(bl, "ACCETTO / ACCEPT");
    lv_obj_center(bl);

    keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(keyboard, 410, 200);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

void onboarding_screen_show()
{
    lv_scr_load(screen);
}
