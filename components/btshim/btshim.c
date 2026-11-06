/**
 * @file btshim.c
 * BT service — ESP32 implementation
 *
 * Based on STM32 bt.c + bt_api.c. Uses the same message-queue architecture
 * and GapEvent callback system. RPC and Power integration are excluded
 * until those services are ported.
 */

#include "bt_i.h"

#include <furi_hal_bt.h>
#include <gui/elements.h>
#include <assets_icons.h>
#include <notification/notification_messages.h>
#include <ble_profile/extra_profiles/hid_profile.h>
#include <ble_profile/extra_profiles/serial_profile.h>
#include <rpc/rpc.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>
#if CONFIG_BT_NIMBLE_ENABLED
#include <ble_core.h>
#else
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#endif
#include <ble_serial.h>


#define BT_RPC_EVENT_BUFF_SENT    (1UL << 0)
#define BT_RPC_EVENT_DISCONNECTED (1UL << 1)
#define BT_RPC_EVENT_ALL          (BT_RPC_EVENT_BUFF_SENT | BT_RPC_EVENT_DISCONNECTED)

/* RX feeder thread event flags */
#define BT_RX_EVENT_DATA      (1UL << 0)
#define BT_RX_EVENT_STOP      (1UL << 1)
#define BT_RX_EVENT_ALL       (BT_RX_EVENT_DATA | BT_RX_EVENT_STOP)

#define BT_RX_STREAM_SIZE     (8192)

#define BT_LOG_I(fmt, ...) ESP_LOGI("BtSrv", fmt, ##__VA_ARGS__)
#define BT_LOG_E(fmt, ...) ESP_LOGE("BtSrv", fmt, ##__VA_ARGS__)
#define BT_LOG_W(fmt, ...) ESP_LOGW("BtSrv", fmt, ##__VA_ARGS__)
#define BT_LOG_D(fmt, ...) ESP_LOGD("BtSrv", fmt, ##__VA_ARGS__)

#define TAG "BtSrv"
#define ICON_SPACER          2
#define BT_DEFAULT_MTU       BLE_PROFILE_SERIAL_PACKET_SIZE_MAX

/* truncated mid-restore for payload — use full local file next */
static void bt_draw_beacon_statusbar_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    canvas_draw_icon(canvas, 0, 0, &I_BLE_beacon_7x8);
}
