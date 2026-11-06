#include "wlan_hal.h"

#include <esp_wifi.h>
#include <furi_hal_power.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <esp_private/wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_sntp.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>
#if CONFIG_BT_NIMBLE_ENABLED
#include <ble_core.h>
#else
#include <esp_bt.h>
#include <esp_bt_main.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <furi.h>
#include <btshim.h>
#include <string.h>
#include <stdlib.h>

#define TAG "WlanHal"
// Worker macht esp_wifi/lwIP-Calls; 4096 Words (16 KB) statt 8192 (32 KB),
// um internes RAM für esp_wifi_init zu sparen. Bei Stack-Overflow erhöhen.
#define WLAN_HAL_WORKER_STACK 4096

typedef enum {
    WCMD_INIT_START,
    WCMD_STOP_DEINIT,
    WCMD_SCAN,
    WCMD_CONNECT,
    WCMD_DISCONNECT,
    WCMD_SEND_ETH_RAW,
    WCMD_SET_CHANNEL,
    WCMD_SET_PROMISC,
    WCMD_SEND_RAW,
    WCMD_RUN_FN,
    WCMD_QUIT,
} WlanCmdType;

typedef struct {
    WlanCmdType type;
    bool survey_owned;
    union {
        struct {
            wifi_scan_config_t* config;
            wifi_ap_record_t** out_records;
            uint16_t* out_count;
            uint16_t max_count;
        } scan;
        struct {
            char ssid[33];
            char password[65];
            uint8_t bssid[6];
            uint8_t channel;
            bool bssid_set;
        } connect;
        struct {
            uint8_t* buf;   // heap-alloziert vom Sender, free durch Consumer
            uint16_t len;
        } send_eth;
        struct {
            uint8_t channel;
        } set_channel;
        struct {
            bool enable;
            wifi_promiscuous_cb_t cb;
        } set_promisc;
        struct {
            uint8_t buf[64];
            uint16_t len;
        } send_raw;
        struct {
            WlanHalWorkerFn fn;
            void* arg;
        } run_fn;
    };
    volatile bool* done;
    volatile bool* result;
} WlanCmd;

static bool s_sntp_started = false;
static bool s_started = false;
static bool s_survey_active = false;
static bool s_radio_transition = false;
static bool s_survey_bt_restore = false;
static const char* s_survey_status = "RADIO READY";
static struct {
    bool started, suspended, reconnect, bt_restore, auth_failed, valid;
    wifi_config_t config;
    esp_err_t error;
} s_survey_saved;

const char* wlan_hal_survey_status(void) {
    return __atomic_load_n(&s_survey_status, __ATOMIC_ACQUIRE);
}
static void survey_status(const char* status) {
    __atomic_store_n(&s_survey_status, status, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "survey: %s", status);
}

static bool survey_active(void) {
    return __atomic_load_n(&s_survey_active, __ATOMIC_ACQUIRE);
}
static bool radio_lock(void) {
    bool expected = false;
    return __atomic_compare_exchange_n(
        &s_radio_transition, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
static void radio_unlock(void) {
    __atomic_store_n(&s_radio_transition, false, __ATOMIC_RELEASE);
}
static bool s_bt_was_on = false;
static bool s_netif_inited = false;
static esp_netif_t* s_netif_sta = NULL;
static volatile bool s_wifi_connected = false;
static volatile bool s_wifi_auto_reconnect = false;
static volatile bool s_auth_fail_latched = false;
static volatile bool s_channel_locked = false;

/** true für Disconnect-Reasons, die auf ein falsches Passwort / fehlgeschlagene
 *  Authentifizierung hindeuten. Bewusst OHNE „AP nicht gefunden"/Beacon-Timeout,
 *  damit ein korrektes gespeichertes Passwort bei einem bloßen Ausfall bleibt. */
static bool wlan_reason_is_auth(uint8_t reason) {
    switch(reason) {
    case WIFI_REASON_AUTH_EXPIRE: // 2
    case WIFI_REASON_MIC_FAILURE: // 14
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: // 15 – klassisch: falsches WPA2-Passwort
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: // 16
    case WIFI_REASON_IE_IN_4WAY_DIFFERS: // 17
    case WIFI_REASON_AUTH_FAIL: // 202
    case WIFI_REASON_HANDSHAKE_TIMEOUT: // 204
    case WIFI_REASON_CONNECTION_FAIL: // 205
        return true;
    default:
        return false;
    }
}
static bool s_event_handlers_registered = false;
static volatile uint32_t s_own_ip = 0;
static volatile uint32_t s_own_netmask = 0;

static bool s_light_sleep_inhibited = false;
static FuriMutex* s_connection_sleep_inhibit_mutex = NULL;
static bool s_connection_sleep_inhibited = false;

static bool wlan_connection_sleep_inhibit_mutex_init(void) {
    static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
    if(s_connection_sleep_inhibit_mutex) return true;
    FuriMutex* candidate = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!candidate) return false;
    portENTER_CRITICAL(&init_lock);
    if(!s_connection_sleep_inhibit_mutex) {
        s_connection_sleep_inhibit_mutex = candidate;
    }
    FuriMutex* installed = s_connection_sleep_inhibit_mutex;
    portEXIT_CRITICAL(&init_lock);
    if(installed != candidate) furi_mutex_free(candidate);
    return true;
}

static void wlan_set_connection_sleep_inhibited(bool inhibited) {
    furi_check(s_connection_sleep_inhibit_mutex);
    furi_check(
        furi_mutex_acquire(s_connection_sleep_inhibit_mutex, FuriWaitForever) == FuriStatusOk);
    if(s_connection_sleep_inhibited != inhibited) {
        if(inhibited) furi_hal_power_light_sleep_inhibit_enter();
        else furi_hal_power_light_sleep_inhibit_exit();
        s_connection_sleep_inhibited = inhibited;
    }
    furi_check(furi_mutex_release(s_connection_sleep_inhibit_mutex) == FuriStatusOk);
}

#if CONFIG_BT_NIMBLE_ENABLED
static bool wlan_bt_stack_up(void) { return ble_core_stack_is_up(); }
static bool wlan_bt_stack_down(void) { return !ble_core_stack_is_up(); }
#else
static bool wlan_bt_stack_up(void) {
    return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
}
static bool wlan_bt_stack_down(void) {
    return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE &&
           esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED;
}
#endif

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_worker_task = NULL;
static StackType_t* s_worker_stack = NULL;
static StaticTask_t s_worker_buf;

static void wlan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    UNUSED(arg);
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)event_data;
        if(d) {
            ESP_LOGW(TAG, "STA disconnected: reason=%u (ssid_len=%u)", (unsigned)d->reason, (unsigned)d->ssid_len);
            if(wlan_reason_is_auth((uint8_t)d->reason)) s_auth_fail_latched = true;
        }
        s_wifi_connected = false;
        s_own_ip = 0;
        s_own_netmask = 0;
        if(!s_wifi_auto_reconnect) {
            wlan_set_connection_sleep_inhibited(false);
        }
        if(s_wifi_auto_reconnect && !survey_active()) {
            esp_wifi_connect();
        }
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        s_own_ip = event->ip_info.ip.addr;
        s_own_netmask = event->ip_info.netmask.addr;
        s_wifi_connected = true;
        wlan_set_connection_sleep_inhibited(true);
        // Sync the RTC via SNTP once we have an IP. Without a correct clock
        // NTLMv2 (SMB) and anything else time-sensitive uses ~1970. UTC is
        // fine for these uses. Harmless if the network has no internet (the
        // SMB code also falls back to the server-supplied timestamp).
        if(!s_sntp_started) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            s_sntp_started = true;
            ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
        }
    }
}

static void wlan_worker_fn(void* arg) {
    UNUSED(arg);
    ESP_LOGI(TAG, "Worker started");

    WlanCmd cmd;
    while(1) {
        if(xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        /* Also reject commands queued immediately before acquisition. Never
         * execute another app's connection, active scan or transmit callback
         * while an external passive survey owns the radio. */
        if(survey_active() && !cmd.survey_owned) {
            if(cmd.type == WCMD_SEND_ETH_RAW) free(cmd.send_eth.buf);
            if(cmd.type == WCMD_SCAN) {
                *cmd.scan.out_count = 0;
                *cmd.scan.out_records = NULL;
            }
            if(cmd.result) *cmd.result = false;
            if(cmd.done) *cmd.done = true;
            continue;
        }

        bool ok = true;
        esp_err_t err;

        switch(cmd.type) {
        case WCMD_INIT_START:
            if(!s_netif_inited) {
                esp_netif_init();
                esp_event_loop_create_default();
                s_netif_sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if(!s_netif_sta) {
                    s_netif_sta = esp_netif_create_default_wifi_sta();
                }
                s_netif_inited = true;
            }
            if(!s_event_handlers_registered) {
                esp_event_handler_register(
                    WIFI_EVENT, ESP_EVENT_ANY_ID, &wlan_event_handler, NULL);
                esp_event_handler_register(
                    IP_EVENT, IP_EVENT_STA_GOT_IP, &wlan_event_handler, NULL);
                s_event_handlers_registered = true;
            }

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            cfg.static_rx_buf_num = 2;
            cfg.dynamic_rx_buf_num = 4;
            cfg.dynamic_tx_buf_num = 8;

            err = esp_wifi_init(&cfg);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(err));
                ok = false;
                break;
            }
            esp_wifi_set_storage(WIFI_STORAGE_RAM);
            esp_wifi_set_mode(WIFI_MODE_STA);
            err = esp_wifi_start();
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
                esp_wifi_deinit();
                ok = false;
            }
            break;

        case WCMD_STOP_DEINIT:
            s_wifi_auto_reconnect = false;
            wlan_set_connection_sleep_inhibited(false);
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
            esp_wifi_set_promiscuous(false);
            esp_wifi_stop();
            esp_wifi_deinit();
            break;

        case WCMD_CONNECT: {
            s_wifi_auto_reconnect = false;
            wlan_set_connection_sleep_inhibited(false);
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
            s_auth_fail_latched = false;
            vTaskDelay(pdMS_TO_TICKS(100));

            wifi_config_t wcfg = {0};
            strncpy((char*)wcfg.sta.ssid, cmd.connect.ssid, 32);
            if(cmd.connect.password[0]) {
                strncpy((char*)wcfg.sta.password, cmd.connect.password, 64);
                wcfg.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
            } else {
                wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
            }
            if(cmd.connect.bssid_set) {
                wcfg.sta.bssid_set = true;
                memcpy(wcfg.sta.bssid, cmd.connect.bssid, 6);
            }
            if(cmd.connect.channel) {
                wcfg.sta.channel = cmd.connect.channel;
            }
            wcfg.sta.pmf_cfg.capable = false;
            wcfg.sta.pmf_cfg.required = false;
            s_wifi_auto_reconnect = true;
            wlan_set_connection_sleep_inhibited(true);
