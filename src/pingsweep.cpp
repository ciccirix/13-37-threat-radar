#include "pingsweep.h"

void clock_screen_get_local_time(struct tm *out);
#include "hostresolve.h"
#include "usb_sd.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <SD.h>
#include <WiFi.h>
#include <string.h>
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"

// ---- state -----------------------------------------------------------------

static PingDevice        s_devices[PINGSWEEP_MAX_DEVICES];
static volatile int      s_device_count = 0;
static volatile int      s_scanned      = 0;
static int               s_total        = 0;
static volatile bool     s_running      = false;
static volatile bool     s_done         = false;
static volatile bool     s_abort        = false;
static bool              s_logged       = false;
static volatile bool     s_just_finished = false;

static uint8_t s_net_a, s_net_b, s_net_c;   // /24 prefix being swept
static char    s_ssid[33];

// Per-host ping result, shared between the esp_ping callbacks and sweep_task.
static SemaphoreHandle_t   s_sem = nullptr;
static volatile bool       s_cur_alive = false;
static volatile uint32_t   s_cur_rtt   = 0;
static esp_ping_callbacks_t s_cbs;

// ---- esp_ping callbacks (run on the ping session's internal task) ----------

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint32_t t = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
    s_cur_rtt   = t;
    s_cur_alive = true;
}
static void on_ping_timeout(esp_ping_handle_t hdl, void *args) { (void)hdl; (void)args; }
static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    (void)hdl; (void)args;
    if (s_sem) xSemaphoreGive(s_sem);
}

// Best-effort MAC lookup: the host was just pinged, so it is still fresh in
// the lwIP ARP cache.
static void lookup_mac(uint8_t a, uint8_t b, uint8_t c, uint8_t d, PingDevice *dev)
{
    dev->has_mac = false;
    uint32_t want = (uint32_t)a | ((uint32_t)b << 8)
                  | ((uint32_t)c << 16) | ((uint32_t)d << 24);   // network order
    for (int i = 0; i < 32; i++) {
        ip4_addr_t      *ip  = nullptr;
        struct netif    *nif = nullptr;
        struct eth_addr *eth = nullptr;
        if (!etharp_get_entry(i, &ip, &nif, &eth)) continue;
        if (ip && eth && ip->addr == want) {
            memcpy(dev->mac, eth->addr, 6);
            dev->has_mac = true;
            return;
        }
    }
}

// ---- sweep task ------------------------------------------------------------

static void sweep_task(void *)
{
    // Batched, PATIENT ARP scan. ICMP is useless here: WiFi power-save IP
    // cameras drop ping and take up to ~1.7 s just to wake and answer even an
    // ARP who-has (measured: 1720 ms first reply). So we ARP a small batch,
    // wait long enough for a sleepy device to answer, then read back only the
    // RESOLVED entries (etharp_find_addr skips pending ones — no bogus MACs).
    // Small batches keep the tiny lwIP ARP table from overflowing with pending
    // requests. ~32 batches * 1.8 s ≈ 58 s for the whole /24.
    struct netif *nif = netif_default;
    const int BATCH = 8;
    for (int base = 1; base <= 254 && !s_abort; base += BATCH) {
        int last = base + BATCH - 1; if (last > 254) last = 254;

        // THREE rounds of ARP who-has spread over ~2.4 s. A deep power-save
        // camera only wakes every ~1.7 s and drops the first probe or two
        // (measured on a PC: 2 failed pings, then 1720 ms), so a single request
        // misses it — repeating gives it several chances to wake and answer.
        for (int rep = 0; rep < 3 && !s_abort; rep++) {
            for (int d = base; d <= last && nif; d++) {
                ip4_addr_t t;
                t.addr = (uint32_t)s_net_a | ((uint32_t)s_net_b << 8)
                       | ((uint32_t)s_net_c << 16) | ((uint32_t)d << 24);
                etharp_request(nif, &t);
            }
            for (int w = 0; w < 16 && !s_abort; w++) vTaskDelay(pdMS_TO_TICKS(50));  // 0.8 s
        }

        for (int d = base; d <= last && nif; d++) {
            ip4_addr_t t;
            t.addr = (uint32_t)s_net_a | ((uint32_t)s_net_b << 8)
                   | ((uint32_t)s_net_c << 16) | ((uint32_t)d << 24);
            struct eth_addr  *eth = nullptr;
            const ip4_addr_t *ipr = nullptr;
            if (etharp_find_addr(nif, &t, &eth, &ipr) < 0 || !eth) continue;
            uint32_t hostip = ((uint32_t)s_net_a << 24) | ((uint32_t)s_net_b << 16)
                            | ((uint32_t)s_net_c << 8)  | (uint32_t)d;
            bool have = false;
            for (int j = 0; j < s_device_count; j++)
                if (s_devices[j].ip == hostip) { have = true; break; }
            if (have || s_device_count >= PINGSWEEP_MAX_DEVICES) continue;
            PingDevice dev; memset(&dev, 0, sizeof(dev));
            dev.ip = hostip;
            memcpy(dev.mac, eth->addr, 6);
            dev.has_mac = true;
            s_devices[s_device_count++] = dev;
        }
        s_scanned = last;
    }

    s_done    = true;
    s_running = false;

    // Kick the name resolver — it walks the device list and fills in
    // d.name / d.name_source via pingsweep_set_name(). Runs on its own
    // task so the sweep task can exit immediately.
    hostresolve_start();

    vTaskDelete(NULL);
}

// ---- SD log ----------------------------------------------------------------

static void write_log()
{
    if (usb_sd_is_running() || !instance.isCardReady()) return;
    if (!SD.exists("/PingSweeps")) SD.mkdir("/PingSweeps");

    struct tm t;
    clock_screen_get_local_time(&t);
    char path[64];
    snprintf(path, sizeof(path), "/PingSweeps/sweep_%04d%02d%02d_%02d%02d%02d.txt",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);

    File f = SD.open(path, FILE_WRITE);
    if (!f) return;

    f.printf("Ping sweep  %04d-%02d-%02d %02d:%02d:%02d\n",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    f.printf("Network: %u.%u.%u.0/24   SSID \"%s\"\n",
        s_net_a, s_net_b, s_net_c, s_ssid);
    f.printf("Devices found: %d\n\n", s_device_count);

    for (int i = 0; i < s_device_count; i++) {
        const PingDevice &d = s_devices[i];
        f.printf("%u.%u.%u.%u\t%u ms\t",
            (d.ip >> 24) & 0xFF, (d.ip >> 16) & 0xFF,
            (d.ip >> 8) & 0xFF, d.ip & 0xFF, d.rtt_ms);
        if (d.has_mac)
            f.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
        else
            f.print("(MAC unknown)");
        // Trailing tab + name source tag + name (only when we resolved one)
        // so the log mirrors what the user saw on screen.
        if (d.name_source != PNAME_NONE && d.name[0]) {
            const char *src =
                (d.name_source == PNAME_MDNS) ? "mdns" :
                (d.name_source == PNAME_NBNS) ? "nbns" :
                (d.name_source == PNAME_PTR)  ? "ptr"  :
                (d.name_source == PNAME_OUI)  ? "oui"  : "?";
            f.printf("\t%s\t%s", src, d.name);
        }
        f.print("\n");
    }
    f.close();
}

// ---- public API ------------------------------------------------------------

void pingsweep_start()
{
    if (s_running) return;
    if (WiFi.status() != WL_CONNECTED) return;

    IPAddress ip = WiFi.localIP();
    s_net_a = ip[0];
    s_net_b = ip[1];
    s_net_c = ip[2];
    String ss = WiFi.SSID();
    strncpy(s_ssid, ss.c_str(), sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';

    s_device_count  = 0;
    s_scanned       = 0;
    s_total         = 254;
    s_done          = false;
    s_abort         = false;
    s_logged        = false;
    s_just_finished = false;

    if (!s_sem) s_sem = xSemaphoreCreateBinary();

    s_cbs.cb_args         = nullptr;
    s_cbs.on_ping_success = on_ping_success;
    s_cbs.on_ping_timeout = on_ping_timeout;
    s_cbs.on_ping_end     = on_ping_end;

    s_running = true;
    xTaskCreatePinnedToCore(sweep_task, "pingsweep", 4096, NULL, 1, NULL, 0);
}

void pingsweep_stop() { s_abort = true; }

bool pingsweep_is_running() { return s_running; }

void pingsweep_poll()
{
    if (!s_done || s_logged) return;
    s_logged        = true;
    s_just_finished = true;
    write_log();
}

int pingsweep_scanned()      { return s_scanned; }
int pingsweep_total()        { return s_total; }
int pingsweep_device_count() { return s_device_count; }

const PingDevice *pingsweep_device(int idx)
{
    if (idx < 0 || idx >= s_device_count) return nullptr;
    return &s_devices[idx];
}

void pingsweep_set_name(int idx, const char *name, uint8_t src)
{
    if (idx < 0 || idx >= s_device_count) return;
    if (src == PNAME_NONE || !name || !name[0]) return;
    // Don't overwrite a more-authoritative name with a weaker one.
    // Ranking (high → low): MDNS=4, NBNS=3, PTR=2, OUI=1, NONE=0.
    static const uint8_t rank_of[] = { 0, 4, 3, 2, 1 }; // index = PingNameSource
    uint8_t cur = s_devices[idx].name_source;
    if (cur < (uint8_t)(sizeof(rank_of) / sizeof(rank_of[0])) &&
        src < (uint8_t)(sizeof(rank_of) / sizeof(rank_of[0]))) {
        if (rank_of[cur] >= rank_of[src]) return;
    }
    strncpy(s_devices[idx].name, name, PINGSWEEP_NAME_MAX - 1);
    s_devices[idx].name[PINGSWEEP_NAME_MAX - 1] = '\0';
    s_devices[idx].name_source = src;
}

bool pingsweep_just_finished()
{
    bool f = s_just_finished;
    s_just_finished = false;
    return f;
}
