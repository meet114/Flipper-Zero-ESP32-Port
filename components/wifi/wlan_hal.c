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
            uint8_t* buf;
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

static bool wlan_reason_is_auth(uint8_t reason) {
    switch(reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_CONNECTION_FAIL:
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

/* truncated mid-restore - continue next part */
