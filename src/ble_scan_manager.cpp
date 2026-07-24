#include "ble_scan_manager.h"
#include "esp_bt.h"
#include "esp_bt_main.h"

#define BLE_SCAN_MAX_CONSUMERS 4

static ble_scan_cb_t     s_consumers[BLE_SCAN_MAX_CONSUMERS] = {};
static int               s_consumer_count = 0;
static int               s_stack_holds    = 0;   // ble_stack_acquire() refs
static ble_gap_forward_t s_gap_forward    = nullptr;

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    // Advertiser (phone link) sees everything first — it filters for the
    // adv-lifecycle events and ignores the scan traffic below.
    if (s_gap_forward) s_gap_forward(event, param);

    // Re-arm the scan after the controller acknowledges our params. If every
    // consumer left during the brief async window, do nothing.
    if (event == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) {
        if (s_consumer_count > 0)
            esp_ble_gap_start_scanning(0);
        return;
    }
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT) return;
    if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;

    // Fan out to every registered consumer. Each one applies its own filter.
    for (int i = 0; i < s_consumer_count; i++) {
        if (s_consumers[i]) s_consumers[i](param);
    }
}

// State-aware bring-up of the controller + Bluedroid, WITHOUT touching scan
// state — mirrors the pattern wardriver_screen.cpp had before this refactor so
// we can re-attach gracefully if the controller was already initialised by
// something outside our control. Scan arming lives in arm_scanning() so a
// GATT-only user doesn't burn power sweeping channels it never reads.
static bool stack_up()
{
    bool ok = true;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ok = (esp_bt_controller_init(&bt_cfg) == ESP_OK);
    }
    if (ok && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
        ok = (esp_bt_controller_enable(ESP_BT_MODE_BLE) == ESP_OK);
    if (ok && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED)
        ok = (esp_bluedroid_init() == ESP_OK);
    if (ok && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED)
        ok = (esp_bluedroid_enable() == ESP_OK);
    if (!ok) return false;

    esp_ble_gap_register_callback(gap_cb);
    return true;
}

static void arm_scanning()
{
    esp_ble_scan_params_t scan_params = {
        BLE_SCAN_TYPE_PASSIVE,
        BLE_ADDR_TYPE_PUBLIC,
        BLE_SCAN_FILTER_ALLOW_ALL,
        0x50,
        0x30,
        BLE_SCAN_DUPLICATE_DISABLE
    };
    // esp_ble_gap_set_scan_params will trigger SCAN_PARAM_SET_COMPLETE_EVT,
    // which gap_cb above turns into a start_scanning call.
    esp_ble_gap_set_scan_params(&scan_params);
}

static void tear_down_controller()
{
    esp_ble_gap_stop_scanning();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
}

// The stack stays up while anyone — scanner or GATT holder — still wants it.
static int total_users() { return s_consumer_count + s_stack_holds; }

bool ble_scan_add(ble_scan_cb_t cb)
{
    if (!cb) return false;

    // Idempotent: already-registered cbs aren't added twice.
    for (int i = 0; i < s_consumer_count; i++)
        if (s_consumers[i] == cb) return true;

    if (s_consumer_count >= BLE_SCAN_MAX_CONSUMERS) return false;

    bool stack_was_down = (total_users() == 0);
    bool first_scanner  = (s_consumer_count == 0);
    s_consumers[s_consumer_count++] = cb;

    if (stack_was_down && !stack_up()) {
        s_consumer_count--;
        return false;
    }
    // Arm the scan whenever scanning transitions 0 -> 1, even if a GATT
    // holder already had the stack up without scanning.
    if (first_scanner) arm_scanning();
    return true;
}

void ble_scan_remove(ble_scan_cb_t cb)
{
    for (int i = 0; i < s_consumer_count; i++) {
        if (s_consumers[i] == cb) {
            for (int j = i; j < s_consumer_count - 1; j++)
                s_consumers[j] = s_consumers[j + 1];
            s_consumers[--s_consumer_count] = nullptr;
            if (s_consumer_count == 0) {
                if (total_users() == 0)
                    tear_down_controller();
                else
                    // A GATT holder still needs the stack — just go quiet.
                    esp_ble_gap_stop_scanning();
            }
            return;
        }
    }
}

bool ble_stack_acquire()
{
    if (total_users() == 0 && !stack_up())
        return false;
    s_stack_holds++;
    return true;
}

void ble_stack_release()
{
    if (s_stack_holds <= 0) return;
    s_stack_holds--;
    if (total_users() == 0)
        tear_down_controller();
}

void ble_gap_forward_set(ble_gap_forward_t cb)
{
    s_gap_forward = cb;
}

bool ble_scan_active()
{
    return s_consumer_count > 0;
}

int ble_scan_consumer_count()
{
    return s_consumer_count;
}
