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

/* Use ESP_LOG for BLE/RPC debug since FURI_LOG may not work in BTC task context */
#define BT_LOG_I(fmt, ...) ESP_LOGI("BtSrv", fmt, ##__VA_ARGS__)
#define BT_LOG_E(fmt, ...) ESP_LOGE("BtSrv", fmt, ##__VA_ARGS__)
#define BT_LOG_W(fmt, ...) ESP_LOGW("BtSrv", fmt, ##__VA_ARGS__)
#define BT_LOG_D(fmt, ...) ESP_LOGD("BtSrv", fmt, ##__VA_ARGS__)

#define TAG "BtSrv"

#define ICON_SPACER          2
#define BT_DEFAULT_MTU       BLE_PROFILE_SERIAL_PACKET_SIZE_MAX

/* ---- Statusbar ---- */

/* Beacon and Bluetooth state are separate view ports so each can be hidden in
 * Settings > Interface > Status Bar. The GUI puts the same 2 px gap between
 * them that the combined icon used (ICON_SPACER). */
static void bt_draw_beacon_statusbar_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    canvas_draw_icon(canvas, 0, 0, &I_BLE_beacon_7x8);
}

static void bt_draw_statusbar_callback(Canvas* canvas, void* context) {
    furi_assert(context);

    Bt* bt = context;
    if(bt->status == BtStatusAdvertising) {
        canvas_draw_icon(canvas, 0, 0, &I_Bluetooth_Idle_5x8);
    } else if(bt->status == BtStatusConnected) {
        canvas_draw_icon(canvas, 0, 0, &I_Bluetooth_Connected_16x8);
    }
}

static ViewPort* bt_statusbar_view_port_alloc(
    Bt* bt,
    ViewPortDrawCallback callback,
    uint8_t width,
    ViewPortStatusBarIcon icon) {
    ViewPort* statusbar_view_port = view_port_alloc();
    view_port_set_width(statusbar_view_port, width);
    view_port_draw_callback_set(statusbar_view_port, callback, bt);
    view_port_set_statusbar_icon(statusbar_view_port, icon);
    view_port_enabled_set(statusbar_view_port, false);
    return statusbar_view_port;
}

/* ---- PIN code display ---- */

static void bt_pin_code_view_port_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    Bt* bt = context;
    if(bt->pairing_stalled) {
        canvas_clear(canvas);
        elements_multiline_text_aligned(
            canvas,
            64,
            4,
            AlignCenter,
            AlignTop,
            "PAIRING NOT STARTED\nForget Flipper in QFlipper\nand phone Bluetooth settings.\nDevice: Settings > Bluetooth\n> Forget All");
        elements_button_left(canvas, "Back");
        return;
    }
    char pin_code_info[24];
    canvas_draw_icon(canvas, 0, 0, &I_BLE_Pairing_128x64);
    snprintf(pin_code_info, sizeof(pin_code_info), "Pairing code\n%06lu", bt->pin_code);
    elements_multiline_text_aligned(canvas, 64, 4, AlignCenter, AlignTop, pin_code_info);
    elements_button_left(canvas, "Quit");
}

static void bt_pin_code_view_port_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    Bt* bt = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyLeft || event->key == InputKeyBack) {
            view_port_enabled_set(bt->pin_code_view_port, false);
        }
    }
}

static ViewPort* bt_pin_code_view_port_alloc(Bt* bt) {
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, bt_pin_code_view_port_draw_callback, bt);
    view_port_input_callback_set(view_port, bt_pin_code_view_port_input_callback, bt);
    view_port_enabled_set(view_port, false);
    return view_port;
}

static void bt_pin_code_show(Bt* bt, uint32_t pin_code) {
    bt->pin_code = pin_code;
    bt->pairing_stalled = false;
    if(!bt->pin_code_view_port) {
        bt->pin_code_view_port = bt_pin_code_view_port_alloc(bt);
        gui_add_view_port(bt->gui, bt->pin_code_view_port, GuiLayerFullscreen);
    }
    notification_message(bt->notification, &sequence_display_backlight_on);
    BT_LOG_I("Pairing passkey UI requested (suppressed=%u)", bt->suppress_pin_screen);
    if(bt->suppress_pin_screen) {
        BT_LOG_W("Pairing passkey display suppressed");
        return;
    }

    gui_view_port_send_to_front(bt->gui, bt->pin_code_view_port);
    view_port_enabled_set(bt->pin_code_view_port, true);
    BT_LOG_I("Pairing passkey viewport enabled");
}

static void bt_pin_code_hide(Bt* bt) {
    bt->pin_code = 0;
    bt->pairing_stalled = false;
    if(bt->pin_code_view_port && view_port_is_enabled(bt->pin_code_view_port)) {
        view_port_enabled_set(bt->pin_code_view_port, false);
    }
}

static void bt_pairing_stalled_show(Bt* bt) {
    bt->pin_code = 0;
    bt->pairing_stalled = true;
    if(!bt->pin_code_view_port) {
        bt->pin_code_view_port = bt_pin_code_view_port_alloc(bt);
        gui_add_view_port(bt->gui, bt->pin_code_view_port, GuiLayerFullscreen);
    }
    notification_message(bt->notification, &sequence_display_backlight_on);
    gui_view_port_send_to_front(bt->gui, bt->pin_code_view_port);
    view_port_enabled_set(bt->pin_code_view_port, true);
}

/* ---- Storage callback for key reload ---- */

static void bt_storage_callback(const void* message, void* context) {
    furi_assert(context);
    Bt* bt = context;
    const StorageEvent* event = message;

    if(event->type == StorageEventTypeCardMount) {
        const BtMessage msg = {
            .type = BtMessageTypeReloadKeysSettings,
        };

        furi_check(
            furi_message_queue_put(bt->message_queue, &msg, FuriWaitForever) == FuriStatusOk);
    }
}

/* ---- Allocation ---- */

static Bt* bt_alloc(void) {
    Bt* bt = calloc(1, sizeof(Bt));
    furi_check(bt);

    bt->max_packet_size = BT_DEFAULT_MTU;
    bt->status = BtStatusOff;
    bt->current_profile = NULL;

    /* Keys storage (stub on ESP32) */
    bt->keys_storage = bt_keys_storage_alloc(BT_KEYS_STORAGE_PATH);

    /* Message queue */
    bt->message_queue = furi_message_queue_alloc(8, sizeof(BtMessage));

    /* Statusbar view ports */
    bt->beacon_view_port = bt_statusbar_view_port_alloc(
        bt,
        bt_draw_beacon_statusbar_callback,
        icon_get_width(&I_BLE_beacon_7x8),
        ViewPortStatusBarIconBleBeacon);
    bt->statusbar_view_port = bt_statusbar_view_port_alloc(
        bt,
        bt_draw_statusbar_callback,
        icon_get_width(&I_Bluetooth_Idle_5x8),
        ViewPortStatusBarIconBluetooth);

    /* Notification */
    bt->notification = furi_record_open(RECORD_NOTIFICATION);

    /* GUI */
    bt->gui = furi_record_open(RECORD_GUI);
    /* Beacon first: it is drawn left of the Bluetooth state, as before. */
    gui_add_view_port(bt->gui, bt->beacon_view_port, GuiLayerStatusBarLeft);
    gui_add_view_port(bt->gui, bt->statusbar_view_port, GuiLayerStatusBarLeft);

    /* The rpc_start service creates RECORD_RPC before the BT service starts. */
    bt->rpc = furi_record_open(RECORD_RPC);
    bt->rpc_event = furi_event_flag_alloc();

    /* Intermediate RX buffer between GATTS callback and RPC */
    bt->rx_stream = furi_stream_buffer_alloc(BT_RX_STREAM_SIZE, 1);

    bt->pin = 0;
    bt->sleep_suspended = false;
    bt->stack_recovery_requested = false;
    bt->stack_recovery_pending = false;

    return bt;
}

/* ---- RX feeder thread: drains intermediate buffer into RPC ---- */

static int32_t bt_rx_feeder_thread(void* context) {
    Bt* bt = context;
    uint8_t buf[512];

    BT_LOG_I("RX feeder thread started");

    while(true) {
        uint32_t flags = furi_thread_flags_wait(BT_RX_EVENT_ALL, FuriFlagWaitAny, FuriWaitForever);

        if(flags & BT_RX_EVENT_STOP) {
            break;
        }

        if(flags & BT_RX_EVENT_DATA) {
            /* Drain all available data from intermediate buffer into RPC */
            while(!furi_stream_buffer_is_empty(bt->rx_stream)) {
                size_t available = furi_stream_buffer_bytes_available(bt->rx_stream);
                size_t to_read = (available > sizeof(buf)) ? sizeof(buf) : available;
                size_t got = furi_stream_buffer_receive(bt->rx_stream, buf, to_read, 0);
                if(got == 0) break;

                /* Feed to RPC — this CAN block, which is fine on this thread */
                size_t fed = 0;
                while(fed < got) {
                    size_t n = rpc_session_feed(
                        bt->rpc_session, buf + fed, got - fed, 5000);
                    if(n == 0) {
                        BT_LOG_E("RPC feed stalled, dropping %zu bytes", got - fed);
                        break;
                    }
                    fed += n;
                }
            }
        }
    }

    BT_LOG_I("RX feeder thread stopped");
    return 0;
}

/* ---- Serial service event callback (called from BLE GATTS task) ---- */

static uint16_t bt_serial_event_callback(SerialServiceEvent event, void* context) {
    furi_assert(context);
    Bt* bt = context;
    uint16_t ret = 0;

    if(event.event == SerialServiceEventTypeDataReceived) {
        /* Non-blocking write to intermediate buffer — MUST NOT block GATTS task */
        size_t sent = furi_stream_buffer_send(
            bt->rx_stream, event.data.buffer, event.data.size, 0);
