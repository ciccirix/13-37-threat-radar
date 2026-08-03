#include "wifi_creds.h"
#include <WiFi.h>
#include <SD.h>
#include <Preferences.h>
#include <string.h>
#include <ctype.h>

#define WC_NS "wifinet"   // NVS namespace

static void trim(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1]=='\r' || s[n-1]=='\n' || s[n-1]==' ' || s[n-1]=='\t'))
        s[--n] = '\0';
    int i = 0;
    while (s[i]==' ' || s[i]=='\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

// Case-insensitive check that `s` starts with `key`.
static bool starts_with_ci(const char *s, const char *key)
{
    while (*key) { if (tolower((unsigned char)*s) != *key) return false; s++; key++; }
    return true;
}

bool wifi_creds_seed_from_sd()
{
    if (!SD.exists("/wifi.txt")) return false;
    File f = SD.open("/wifi.txt", FILE_READ);
    if (!f) return false;

    char ssid[33] = {0}, pass[64] = {0};
    bool have_ssid = false, have_pass = false;
    int positional = 0;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        char buf[160];
        strncpy(buf, line.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        trim(buf);
        if (buf[0] == '\0' || buf[0] == '#') continue;

        // "ssid: value" / "ssid=value" (and pass/password) — else positional.
        char *sep = strpbrk(buf, ":=");
        if (sep && (starts_with_ci(buf, "ssid") || starts_with_ci(buf, "pass"))) {
            bool is_ssid = starts_with_ci(buf, "ssid");
            char *val = sep + 1; trim(val);
            if (is_ssid) { strncpy(ssid, val, sizeof(ssid)-1); have_ssid = true; }
            else         { strncpy(pass, val, sizeof(pass)-1); have_pass = true; }
        } else if (positional == 0) {
            strncpy(ssid, buf, sizeof(ssid)-1); have_ssid = true; positional = 1;
        } else if (positional == 1) {
            strncpy(pass, buf, sizeof(pass)-1); have_pass = true; positional = 2;
        }
    }
    f.close();

    if (!have_ssid || ssid[0] == '\0') return false;
    wifi_creds_save(ssid, have_pass ? pass : "");
    return true;
}

bool wifi_creds_get(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    Preferences p;
    p.begin(WC_NS, true);
    String s = p.getString("ssid", "");
    String pw = p.getString("pass", "");
    p.end();

    if (s.length() == 0 && wifi_creds_seed_from_sd()) {   // empty NVS → try the SD file
        p.begin(WC_NS, true);
        s  = p.getString("ssid", "");
        pw = p.getString("pass", "");
        p.end();
    }
    if (s.length() == 0) return false;

    strncpy(ssid, s.c_str(),  ssid_sz - 1); ssid[ssid_sz - 1] = '\0';
    strncpy(pass, pw.c_str(), pass_sz - 1); pass[pass_sz - 1] = '\0';
    return true;
}

void wifi_creds_save(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return;
    Preferences p;
    p.begin(WC_NS, false);
    p.putString("ssid", ssid);
    p.putString("pass", pass ? pass : "");
    p.end();
}

bool wifi_creds_pass_for(const char *ssid, char *pass, size_t pass_sz)
{
    char ss[33], pp[64];
    if (!wifi_creds_get(ss, sizeof(ss), pp, sizeof(pp))) return false;
    if (strcmp(ss, ssid) != 0) return false;
    strncpy(pass, pp, pass_sz - 1); pass[pass_sz - 1] = '\0';
    return true;
}

bool wifi_creds_autoconnect_enabled()
{
    Preferences p;
    p.begin(WC_NS, true);
    bool on = p.getBool("auto", false);   // default OFF (don't fight the scanners)
    p.end();
    return on;
}

void wifi_creds_set_autoconnect(bool on)
{
    Preferences p;
    p.begin(WC_NS, false);
    p.putBool("auto", on);
    p.end();
    if (!on) WiFi.disconnect(false);       // dropping association frees the radio now
}

bool wifi_creds_autoconnect()
{
    if (!wifi_creds_autoconnect_enabled()) return false;
    if (WiFi.status() == WL_CONNECTED) return true;
    char ssid[33], pass[64];
    if (!wifi_creds_get(ssid, sizeof(ssid), pass, sizeof(pass))) return false;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    if (pass[0]) WiFi.begin(ssid, pass);
    else         WiFi.begin(ssid);
    return true;
}
