#include "phone_link.h"
#include "ble_scan_manager.h"
#include "threat_radar.h"
#include "gps_screen.h"
#include <Arduino.h>
#include <string.h>
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"

// See phone_link.h for the protocol. Implementation notes:
//  - Bluedroid attr-table server with ESP_GATT_AUTO_RSP everywhere: the stack
//    serves reads (incl. long/blob reads of the ~300-byte THREATS value) from
//    its own attribute DB, so no cross-task buffer juggling on reads.
//  - GATTS callbacks run on the BT task; they only flip flags. Everything that
//    touches the threat-radar store happens in phone_link_bg_tick (main task).
//  - Advertising restarts automatically on disconnect; scanning (detectors)
//    and advertising coexist fine on the S3.

#define PL_APP_ID       0x1337
#define PL_DEVICE_NAME  "13-37"
#define PL_MAX_THREATS  10

#define PL_STATUS_LEN       8
#define PL_REC_LEN          30
#define PL_THREATS_MAX_LEN  (2 + PL_MAX_THREATS * PL_REC_LEN)

// 1337NNNN-1337-4001-8000-746872616461 ("thradar" tail), little-endian as
// Bluedroid wants it. Only bytes [12..13] (NNNN) differ per attribute.
#define PL_UUID128(n) { 0x61,0x64,0x61,0x72,0x68,0x74, 0x00,0x80, 0x01,0x40, \
                        0x37,0x13, (uint8_t)((n) & 0xFF), (uint8_t)((n) >> 8), 0x37,0x13 }

static const uint8_t PL_SVC_UUID[16]     = PL_UUID128(0x0000);
static const uint8_t PL_STATUS_UUID[16]  = PL_UUID128(0x0001);
static const uint8_t PL_THREATS_UUID[16] = PL_UUID128(0x0002);
static const uint8_t PL_CMD_UUID[16]     = PL_UUID128(0x0003);

static const uint16_t UUID_PRI_SERVICE = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t UUID_CHAR_DECL   = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t UUID_CHAR_CCC    = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t  PROP_READ_NOTIFY = ESP_GATT_CHAR_PROP_BIT_READ |
                                         ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t  PROP_READ        = ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t  PROP_WRITE       = ESP_GATT_CHAR_PROP_BIT_WRITE;

// Initial attribute values the stack copies into its DB at table creation.
static uint8_t s_status_init[PL_STATUS_LEN]       = {1, 0, 0, 0, 0, 0, 0, 0};
static uint8_t s_threats_init[2]                  = {1, 0};
static uint8_t s_ccc_init[2]                      = {0, 0};
static uint8_t s_cmd_init[1]                      = {0};

enum {
    IDX_SVC,
    IDX_STATUS_DECL,
    IDX_STATUS_VAL,
    IDX_STATUS_CCC,
    IDX_THREATS_DECL,
    IDX_THREATS_VAL,
    IDX_CMD_DECL,
    IDX_CMD_VAL,
    PL_IDX_NB
};

static const esp_gatts_attr_db_t pl_gatt_db[PL_IDX_NB] = {
    // Primary service declaration, value = our 128-bit service UUID.
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&UUID_PRI_SERVICE,
      ESP_GATT_PERM_READ, sizeof(PL_SVC_UUID), sizeof(PL_SVC_UUID), (uint8_t *)PL_SVC_UUID}},

    // STATUS: read + notify, with the CCC descriptor the phone subscribes on.
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&UUID_CHAR_DECL,
      ESP_GATT_PERM_READ, 1, 1, (uint8_t *)&PROP_READ_NOTIFY}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)PL_STATUS_UUID,
      ESP_GATT_PERM_READ, PL_STATUS_LEN, PL_STATUS_LEN, s_status_init}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&UUID_CHAR_CCC,
      ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(s_ccc_init), sizeof(s_ccc_init), s_ccc_init}},

    // THREATS: read-only serialized contact list.
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&UUID_CHAR_DECL,
      ESP_GATT_PERM_READ, 1, 1, (uint8_t *)&PROP_READ}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)PL_THREATS_UUID,
      ESP_GATT_PERM_READ, PL_THREATS_MAX_LEN, sizeof(s_threats_init), s_threats_init}},

    // CMD: write-only control byte.
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&UUID_CHAR_DECL,
      ESP_GATT_PERM_READ, 1, 1, (uint8_t *)&PROP_WRITE}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)PL_CMD_UUID,
      ESP_GATT_PERM_WRITE, sizeof(s_cmd_init), sizeof(s_cmd_init), s_cmd_init}},
};

static volatile bool    s_active      = false;
static volatile bool    s_connected   = false;
static volatile bool    s_advertising = false;
static volatile bool    s_svc_ready   = false;
static volatile uint8_t s_adv_cfg     = 0;      // bit0 adv data, bit1 scan rsp
static volatile bool    s_cmd_reset   = false;  // set by BT task, drained by bg_tick
static esp_gatt_if_t    s_gatts_if    = ESP_GATT_IF_NONE;
static uint16_t         s_conn_id     = 0;
static uint16_t         s_handles[PL_IDX_NB] = {};

static esp_ble_adv_params_t s_adv_params;

static void init_adv_params()
{
    memset(&s_adv_params, 0, sizeof(s_adv_params));
    s_adv_params.adv_int_min       = 0xA0;   // 100 ms
    s_adv_params.adv_int_max      = 0x140;   // 200 ms — battery-friendly, still quick to find
    s_adv_params.adv_type          = ADV_TYPE_IND;
    s_adv_params.own_addr_type     = BLE_ADDR_TYPE_PUBLIC;
    s_adv_params.channel_map       = ADV_CHNL_ALL;
    s_adv_params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
}

// The name lives in the 31-byte adv packet, the 128-bit service UUID in the
// scan response — together they let Web Bluetooth filter by either.
static void config_adv_data()
{
    static esp_ble_adv_data_t adv;
    memset(&adv, 0, sizeof(adv));
    adv.set_scan_rsp = false;
    adv.include_name = true;
    adv.flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
    esp_ble_gap_config_adv_data(&adv);

    static esp_ble_adv_data_t rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.set_scan_rsp     = true;
    rsp.service_uuid_len = sizeof(PL_SVC_UUID);
    rsp.p_service_uuid   = (uint8_t *)PL_SVC_UUID;
    esp_ble_gap_config_adv_data(&rsp);
}

// Both adv payloads acknowledged + service started + nobody connected -> beacon.
static void maybe_start_adv()
{
    if (s_active && s_svc_ready && !s_connected && s_adv_cfg == 3)
        esp_ble_gap_start_advertising(&s_adv_params);
}

// Fed every GAP event by ble_scan_manager's forward slot; we only care about
// the advertising lifecycle, scan traffic falls through.
static void pl_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_cfg |= 1;
        maybe_start_adv();
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        s_adv_cfg |= 2;
        maybe_start_adv();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        s_advertising = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_advertising = false;
        break;
    default:
        break;
    }
}

static void pl_gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                           esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        s_gatts_if = gatts_if;
        esp_ble_gap_set_device_name(PL_DEVICE_NAME);
        config_adv_data();
        esp_ble_gatts_create_attr_tab(pl_gatt_db, gatts_if, PL_IDX_NB, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status == ESP_GATT_OK &&
            param->add_attr_tab.num_handle == PL_IDX_NB) {
            memcpy(s_handles, param->add_attr_tab.handles, sizeof(s_handles));
            esp_ble_gatts_start_service(s_handles[IDX_SVC]);
            s_svc_ready = true;
            maybe_start_adv();
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id     = param->connect.conn_id;
        s_connected   = true;
        s_advertising = false;   // controller stops advertising on connect
        break;

    case ESP_GATTS_DISCONNECT_EVT: {
        s_connected = false;
        // Re-arm the CCC — the next phone must subscribe afresh.
        uint8_t zero[2] = {0, 0};
        esp_ble_gatts_set_attr_value(s_handles[IDX_STATUS_CCC], sizeof(zero), zero);
        maybe_start_adv();
        break;
    }

    case ESP_GATTS_WRITE_EVT:
        // AUTO_RSP already acked the write; we just observe. Store mutation
        // is deferred to bg_tick so it happens on the main task.
        if (!param->write.is_prep &&
            param->write.handle == s_handles[IDX_CMD_VAL] &&
            param->write.len >= 1 && param->write.value[0] == 0x01)
            s_cmd_reset = true;
        break;

    default:
        break;
    }
}

bool phone_link_start()
{
    if (s_active) return true;
    if (!ble_stack_acquire()) return false;

    init_adv_params();
    s_adv_cfg     = 0;
    s_svc_ready   = false;
    s_connected   = false;
    s_advertising = false;
    s_cmd_reset   = false;
    ble_gap_forward_set(pl_gap_event);

    if (esp_ble_gatts_register_callback(pl_gatts_event) != ESP_OK ||
        esp_ble_gatts_app_register(PL_APP_ID) != ESP_OK) {
        ble_gap_forward_set(nullptr);
        ble_stack_release();
        return false;
    }
    s_active = true;
    return true;
}

void phone_link_stop()
{
    if (!s_active) return;
    s_active = false;
    if (s_advertising) esp_ble_gap_stop_advertising();
    if (s_connected && s_gatts_if != ESP_GATT_IF_NONE)
        esp_ble_gatts_close(s_gatts_if, s_conn_id);
    if (s_gatts_if != ESP_GATT_IF_NONE) {
        esp_ble_gatts_app_unregister(s_gatts_if);   // drops the service too
        s_gatts_if = ESP_GATT_IF_NONE;
    }
    s_svc_ready   = false;
    s_connected   = false;
    s_advertising = false;
    ble_gap_forward_set(nullptr);
    ble_stack_release();
}

bool phone_link_active()    { return s_active; }
bool phone_link_connected() { return s_active && s_connected; }

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_f32(uint8_t *p, float v)    { memcpy(p, &v, 4); }  // ESP32 is LE

void phone_link_bg_tick()
{
    if (!s_active) return;

    // Phone-requested wipe, applied here so the store is only ever touched
    // from the main task.
    if (s_cmd_reset) {
        s_cmd_reset = false;
        threatradar_reset();
    }

    // 1 Hz is plenty — the radar itself dedups sightings per 5 min.
    static uint32_t s_last_ms = 0;
    uint32_t now = millis();
    if (now - s_last_ms < 1000) return;
    s_last_ms = now;
    if (!s_svc_ready) return;

    // --- THREATS: serialize the ranked list, bump seq only on real change ---
    static uint8_t s_threats_seq = 0;
    TrThreat t[PL_MAX_THREATS];
    int n = threatradar_get_threats(t, PL_MAX_THREATS);

    uint8_t buf[PL_THREATS_MAX_LEN];
    buf[0] = 1;              // protocol version
    buf[1] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        uint8_t *r = &buf[2 + i * PL_REC_LEN];
        memcpy(r, t[i].mac, 6);
        r[6]  = t[i].category;
        r[7]  = t[i].level;
        r[8]  = t[i].waypoints;
        r[9]  = (uint8_t)t[i].best_rssi;
        r[10] = (t[i].active    ? 1 : 0)
              | (t[i].community ? 2 : 0)
              | (t[i].familiar  ? 4 : 0);
        r[11] = 0;
        put_u16(&r[12], t[i].span_m);
        put_u16(&r[14], t[i].span_min);
        put_f32(&r[16], t[i].first_lat);
        put_f32(&r[20], t[i].first_lon);
        memcpy(&r[24], t[i].first_time, sizeof(t[i].first_time));
    }
    uint16_t tlen = 2 + n * PL_REC_LEN;

    static uint16_t s_prev_tlen = 0;
    static uint8_t  s_prev_threats[PL_THREATS_MAX_LEN];
    if (tlen != s_prev_tlen || memcmp(buf, s_prev_threats, tlen) != 0) {
        memcpy(s_prev_threats, buf, tlen);
        s_prev_tlen = tlen;
        s_threats_seq++;
        esp_ble_gatts_set_attr_value(s_handles[IDX_THREATS_VAL], tlen, buf);
    }

    // --- alert edge: worst contact just crossed into LIKELY -----------------
    static uint8_t s_alert_seq = 0;
    static int     s_prev_top  = TR_LVL_NONE;
    int top = threatradar_top_level();
    if (top >= TR_LVL_LIKELY && s_prev_top < TR_LVL_LIKELY) s_alert_seq++;
    s_prev_top = top;

    // --- STATUS: 8 bytes, notify on change. The uptime minute counter makes
    // this fire at least once a minute — a free keep-alive for the phone. ----
    uint8_t st[PL_STATUS_LEN];
    st[0] = 1;
    st[1] = (uint8_t)threatradar_threat_count();
    st[2] = (uint8_t)top;
    st[3] = s_alert_seq;
    st[4] = s_threats_seq;
    st[5] = gps_screen_is_powered() ? 1 : 0;
    put_u16(&st[6], (uint16_t)(now / 60000));

    static uint8_t s_prev_status[PL_STATUS_LEN] = {0xFF};
    if (memcmp(st, s_prev_status, PL_STATUS_LEN) != 0) {
        memcpy(s_prev_status, st, PL_STATUS_LEN);
        esp_ble_gatts_set_attr_value(s_handles[IDX_STATUS_VAL], PL_STATUS_LEN, st);

        if (s_connected) {
            // Only push if the phone actually subscribed (CCC notify bit).
            uint16_t       clen = 0;
            const uint8_t *ccc  = nullptr;
            if (esp_ble_gatts_get_attr_value(s_handles[IDX_STATUS_CCC], &clen, &ccc) == ESP_GATT_OK &&
                clen >= 1 && (ccc[0] & 1)) {
                esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                            s_handles[IDX_STATUS_VAL],
                                            PL_STATUS_LEN, st, false);
            }
        }
    }
}
