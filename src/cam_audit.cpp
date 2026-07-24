#include "cam_audit.h"
#include "pingsweep.h"
#include "flock.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <MD5Builder.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>

#define CA_QLEN 24

static TaskHandle_t     s_task = nullptr;
static volatile bool    s_run  = false;
static volatile CamAuditPhase s_phase = CA_IDLE;
static volatile int     s_scanned = 0;
static volatile int     s_total   = 0;
static QueueHandle_t    s_q = nullptr;

// Well-known IP-camera / DVR default credentials, tried against any camera that
// answers 401. Small, high-hit-rate list (Hikvision admin:12345, Dahua/generic
// admin:admin, Axis root:pass, Foscam admin:<blank>, …).
struct Cred { const char *u; const char *p; };
static const Cred CREDS[] = {
    {"admin", "admin"},   {"admin", ""},        {"admin", "12345"},
    {"admin", "123456"},  {"admin", "1234"},    {"admin", "password"},
    {"admin", "admin123"},{"admin", "9999"},    {"admin", "4321"},
    {"root",  "root"},    {"root",  "pass"},    {"root",  "12345"},
    {"root",  "admin"},   {"service","service"},{"supervisor","supervisor"},
};
#define NCREDS (int)(sizeof(CREDS) / sizeof(CREDS[0]))

// ---- low-level probe helpers -----------------------------------------------

static int read_resp(WiFiClient &c, char *buf, int maxlen, uint32_t to_ms)
{
    int n = 0;
    uint32_t start = millis();
    while (millis() - start < to_ms && n < maxlen - 1) {
        while (c.available() && n < maxlen - 1) buf[n++] = (char)c.read();
        if (n > 0 && !c.connected() && !c.available()) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    buf[n] = '\0';
    return n;
}

static int status_code(const char *buf)
{
    const char *sp = strchr(buf, ' ');
    if (!sp) return -1;
    return atoi(sp + 1);
}

static void lower_copy(char *dst, const char *src, int n)
{
    int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

// Extract a token/quoted value for `key` from a WWW-Authenticate line.
static bool www_val(const char *resp, const char *key, char *out, int n)
{
    size_t klen = strlen(key);
    const char *hit = nullptr;
    for (const char *p = resp; *p; p++)
        if (strncasecmp(p, key, klen) == 0) { hit = p + klen; break; }
    if (!hit) return false;
    while (*hit == ' ' || *hit == '=') hit++;
    bool q = (*hit == '"');
    if (q) hit++;
    int i = 0;
    while (*hit && i < n - 1) {
        if (q && *hit == '"') break;
        if (!q && (*hit == ',' || *hit == ' ' || *hit == '\r' || *hit == '\n')) break;
        out[i++] = *hit++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool ci_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0) return true;
    return false;
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(const char *in, char *out)
{
    int len = strlen(in), o = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint8_t)in[i] << 16;
        if (i + 1 < len) v |= (uint8_t)in[i + 1] << 8;
        if (i + 2 < len) v |= (uint8_t)in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? B64[v & 63] : '=';
    }
    out[o] = '\0';
}

static void md5hex(const char *s, char *out33)
{
    MD5Builder b;
    b.begin();
    b.add((uint8_t *)s, strlen(s));
    b.calculate();
    b.getChars(out33);   // 32 hex chars + NUL
}

// Build the Authorization header value for Basic or Digest auth.
static void build_auth(char *out, int outn, bool digest, const char *realm,
                       const char *nonce, const char *qop, const char *method,
                       const char *uri, const char *user, const char *pass)
{
    if (!digest) {
        char up[96], b64[160];
        snprintf(up, sizeof(up), "%s:%s", user, pass);
        b64_encode(up, b64);
        snprintf(out, outn, "Authorization: Basic %s", b64);
        return;
    }
    char ha1in[160], ha1[33], ha2in[160], ha2[33], resp[33];
    snprintf(ha1in, sizeof(ha1in), "%s:%s:%s", user, realm, pass); md5hex(ha1in, ha1);
    snprintf(ha2in, sizeof(ha2in), "%s:%s", method, uri);          md5hex(ha2in, ha2);
    if (qop && qop[0]) {
        char ri[220];
        snprintf(ri, sizeof(ri), "%s:%s:%s:%s:%s:%s",
                 ha1, nonce, "00000001", "1337c0de", qop, ha2);
        md5hex(ri, resp);
        snprintf(out, outn,
                 "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                 "uri=\"%s\", qop=%s, nc=00000001, cnonce=\"1337c0de\", "
                 "response=\"%s\", algorithm=MD5",
                 user, realm, nonce, uri, qop, resp);
    } else {
        char ri[140];
        snprintf(ri, sizeof(ri), "%s:%s:%s", ha1, nonce, ha2);
        md5hex(ri, resp);
        snprintf(out, outn,
                 "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                 "uri=\"%s\", response=\"%s\", algorithm=MD5",
                 user, realm, nonce, uri, resp);
    }
}

// ---- service probes --------------------------------------------------------

// RTSP probe. Returns CL_RTSP_OPEN / CL_RTSP_AUTH, or 0 if not RTSP.
static uint8_t probe_rtsp(IPAddress ip, uint16_t port)
{
    WiFiClient c;
    if (!c.connect(ip, port, 700)) return 0;
    char ips[16], uri[48], req[160], resp[256];
    snprintf(ips, sizeof(ips), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    snprintf(uri, sizeof(uri), "rtsp://%s:%u/", ips, port);

    snprintf(req, sizeof(req), "OPTIONS %s RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: 1337\r\n\r\n", uri);
    c.print(req);
    read_resp(c, resp, sizeof(resp), 700);
    if (strncmp(resp, "RTSP/", 5) != 0) { c.stop(); return 0; }

    snprintf(req, sizeof(req),
             "DESCRIBE %s RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\nUser-Agent: 1337\r\n\r\n", uri);
    c.print(req);
    int n = read_resp(c, resp, sizeof(resp), 800);
    c.stop();
    int code = (n > 0) ? status_code(resp) : -1;
    if (code == 200) return CL_RTSP_OPEN;
    return CL_RTSP_AUTH;
}

// Try the default-cred list against an RTSP endpoint that needs auth.
// Returns the matching CREDS[] index, or -1.
static int rtsp_try_creds(IPAddress ip, uint16_t port)
{
    WiFiClient c;
    if (!c.connect(ip, port, 700)) return -1;
    char ips[16], uri[48], req[700], resp[512];
    snprintf(ips, sizeof(ips), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    snprintf(uri, sizeof(uri), "rtsp://%s:%u/", ips, port);

    int cseq = 1;
    snprintf(req, sizeof(req), "DESCRIBE %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: 1337\r\n\r\n", uri, cseq++);
    c.print(req);
    read_resp(c, resp, sizeof(resp), 800);
    if (status_code(resp) != 401) { c.stop(); return -1; }

    bool digest = ci_contains(resp, "Digest");
    char realm[80] = "", nonce[96] = "", qop[24] = "";
    www_val(resp, "realm", realm, sizeof(realm));
    if (digest) { www_val(resp, "nonce", nonce, sizeof(nonce)); www_val(resp, "qop", qop, sizeof(qop)); }

    for (int k = 0; k < NCREDS && s_run; k++) {
        char auth[560];
        build_auth(auth, sizeof(auth), digest, realm, nonce, qop, "DESCRIBE", uri, CREDS[k].u, CREDS[k].p);
        if (!c.connected()) { c.stop(); if (!c.connect(ip, port, 700)) break; }
        snprintf(req, sizeof(req),
                 "DESCRIBE %s RTSP/1.0\r\nCSeq: %d\r\n%s\r\nAccept: application/sdp\r\nUser-Agent: 1337\r\n\r\n",
                 uri, cseq++, auth);
        c.print(req);
        int n = read_resp(c, resp, sizeof(resp), 800);
        int cc = (n > 0) ? status_code(resp) : -1;
        if (cc == 200) { c.stop(); return k; }
        if (cc == 401 && digest) {   // server may rotate the nonce on failure
            char nn[96] = "";
            if (www_val(resp, "nonce", nn, sizeof(nn))) strncpy(nonce, nn, sizeof(nonce) - 1);
        }
    }
    c.stop();
    return -1;
}

// HTTP probe. Returns 1 if the response looks like a camera/DVR, else 0.
// *needs_auth set true when the server answered 401.
static int http_probe(IPAddress ip, uint16_t port, bool *needs_auth)
{
    *needs_auth = false;
    WiFiClient c;
    if (!c.connect(ip, port, 600)) return 0;
    char ips[16], req[96], resp[768], low[768];
    snprintf(ips, sizeof(ips), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    snprintf(req, sizeof(req), "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: 1337\r\n\r\n", ips);
    c.print(req);
    int n = read_resp(c, resp, sizeof(resp), 700);
    c.stop();
    if (n <= 0) return 0;
    *needs_auth = (status_code(resp) == 401);
    lower_copy(low, resp, sizeof(low));
    static const char *sig[] = {
        "uc-httpd", "boa/", "thttpd", "goahead", "hipcam", "netwave",
        "ip camera", "ipcam", "webcam", "network camera", "dvr", "nvr",
        "hikvision", "dahua", "axis", "reolink", "surveillance", "rtsp",
    };
    for (unsigned i = 0; i < sizeof(sig) / sizeof(sig[0]); i++)
        if (strstr(low, sig[i])) return 1;
    return 0;
}

static int http_try_creds(IPAddress ip, uint16_t port)
{
    // Fetch the challenge once.
    char ips[16], req[700], resp[768];
    snprintf(ips, sizeof(ips), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    {
        WiFiClient c;
        if (!c.connect(ip, port, 600)) return -1;
        snprintf(req, sizeof(req), "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: 1337\r\n\r\n", ips);
        c.print(req);
        read_resp(c, resp, sizeof(resp), 700);
        c.stop();
        if (status_code(resp) != 401) return -1;
    }
    bool digest = ci_contains(resp, "Digest");
    char realm[80] = "", nonce[96] = "", qop[24] = "";
    www_val(resp, "realm", realm, sizeof(realm));
    if (digest) { www_val(resp, "nonce", nonce, sizeof(nonce)); www_val(resp, "qop", qop, sizeof(qop)); }

    for (int k = 0; k < NCREDS && s_run; k++) {
        WiFiClient c;
        if (!c.connect(ip, port, 600)) continue;
        char auth[560];
        build_auth(auth, sizeof(auth), digest, realm, nonce, qop, "GET", "/", CREDS[k].u, CREDS[k].p);
        snprintf(req, sizeof(req), "GET / HTTP/1.0\r\nHost: %s\r\n%s\r\nUser-Agent: 1337\r\n\r\n", ips, auth);
        c.print(req);
        int n = read_resp(c, resp, sizeof(resp), 700);
        int cc = (n > 0) ? status_code(resp) : -1;
        c.stop();
        if (cc == 200) return k;
        if (cc == 401 && digest) {
            char nn[96] = "";
            if (www_val(resp, "nonce", nn, sizeof(nn))) strncpy(nonce, nn, sizeof(nonce) - 1);
        }
    }
    return -1;
}

// ---- audit task ------------------------------------------------------------

static void enqueue(uint32_t ip, const uint8_t *mac, bool has_mac,
                    const char *vendor, uint8_t level, uint16_t port, const char *note)
{
    if (!s_q) return;
    CamFinding f = {};
    f.ip = ip;
    if (has_mac) { memcpy(f.mac, mac, 6); f.has_mac = true; }
    f.vendor = vendor;
    f.level  = level;
    f.port   = port;
    if (note) strncpy(f.note, note, sizeof(f.note) - 1);
    xQueueSend(s_q, &f, 0);
}

static void probe_host(const PingDevice *d)
{
    uint32_t ip = d->ip;
    IPAddress addr((ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    const char *vendor = d->has_mac ? flock_classify(d->mac, nullptr) : nullptr;

    uint8_t  level = 0;
    uint16_t port  = 0;
    char     note[40] = "";

    static const uint16_t rtsp_ports[] = { 554, 8554 };
    for (unsigned p = 0; p < 2 && s_run; p++) {
        uint8_t lv = probe_rtsp(addr, rtsp_ports[p]);
        if (!lv) continue;
        port = rtsp_ports[p];
        if (lv == CL_RTSP_OPEN) {
            level = CL_RTSP_OPEN;
            snprintf(note, sizeof(note), "RTSP OPEN :%u", port);
        } else {
            int ci = rtsp_try_creds(addr, port);
            if (ci >= 0) {
                level = CL_DEFAULT_CREDS;
                snprintf(note, sizeof(note), "DEFAULT %s:%s", CREDS[ci].u, CREDS[ci].p);
            } else {
                level = CL_RTSP_AUTH;
                snprintf(note, sizeof(note), "RTSP auth :%u", port);
            }
        }
        break;
    }

    if (level == 0) {
        static const uint16_t http_ports[] = { 80, 8080, 8000 };
        for (unsigned p = 0; p < 3 && s_run; p++) {
            bool na = false;
            if (!http_probe(addr, http_ports[p], &na)) continue;
            port = http_ports[p];
            if (na) {
                int ci = http_try_creds(addr, port);
                if (ci >= 0) {
                    level = CL_DEFAULT_CREDS;
                    snprintf(note, sizeof(note), "DEFAULT %s:%s @http", CREDS[ci].u, CREDS[ci].p);
                } else {
                    level = CL_HTTP;
                    snprintf(note, sizeof(note), "HTTP auth :%u", port);
                }
            } else {
                level = CL_HTTP;
                snprintf(note, sizeof(note), "HTTP cam :%u", port);
            }
            break;
        }
    }

    if (level == 0 && vendor) {
        level = CL_VENDOR;
        snprintf(note, sizeof(note), "OUI only");
    }

    if (level > 0)
        enqueue(ip, d->mac, d->has_mac, vendor, level, port, note);
}

static void audit_task(void *)
{
    if (pingsweep_is_running()) pingsweep_stop();
    for (int i = 0; i < 40 && pingsweep_is_running(); i++) vTaskDelay(pdMS_TO_TICKS(50));

    s_phase = CA_SWEEP;
    pingsweep_start();
    for (int i = 0; i < 800 && s_run && pingsweep_is_running(); i++) vTaskDelay(pdMS_TO_TICKS(50));

    if (!s_run) { s_phase = CA_DONE; s_task = nullptr; vTaskDelete(nullptr); return; }

    int n = pingsweep_device_count();
    s_total   = n;
    s_scanned = 0;
    s_phase   = CA_PROBE;

    for (int i = 0; i < n && s_run; i++) {
        const PingDevice *d = pingsweep_device(i);
        if (d) probe_host(d);
        s_scanned++;
    }

    s_phase = CA_DONE;
    s_run   = false;
    s_task  = nullptr;
    vTaskDelete(nullptr);
}

// ---- public API ------------------------------------------------------------

bool cam_audit_start()
{
    if (s_run) return true;
    if (WiFi.status() != WL_CONNECTED) return false;

    if (!s_q) s_q = xQueueCreate(CA_QLEN, sizeof(CamFinding));
    else      xQueueReset(s_q);

    s_scanned = 0;
    s_total   = 0;
    s_phase   = CA_SWEEP;
    s_run     = true;
    xTaskCreatePinnedToCore(audit_task, "cam_audit", 8192, nullptr, 1, &s_task, 0);
    return true;
}

void cam_audit_stop()
{
    if (!s_run) return;
    s_run = false;
    for (int i = 0; i < 60 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    pingsweep_stop();
    s_phase = CA_IDLE;
}

bool          cam_audit_is_running() { return s_run; }
CamAuditPhase cam_audit_phase()      { return s_phase; }
int           cam_audit_scanned()    { return s_scanned; }
int           cam_audit_total()      { return s_total; }

bool cam_audit_next_finding(CamFinding *out)
{
    if (!s_q) return false;
    return xQueueReceive(s_q, out, 0) == pdTRUE;
}
