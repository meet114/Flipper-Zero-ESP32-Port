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
#define WLAN_HAL_WORKER_STACK 4096

/* NOTE: full file restored from local journey tree via MCP push_files */
