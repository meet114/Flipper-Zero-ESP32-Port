#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TRACKER_MAX_DEVICES 24

typedef enum {
    TrackerKindUnknown,
    TrackerKindAirTag,
    TrackerKindSmartTag,
    TrackerKindTile,
    TrackerKindApplePP,
    TrackerKindAppleNearby,
} TrackerKind;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    TrackerKind kind;
    char name[24];
    int8_t rssi;
    int8_t best_rssi;
    uint32_t last_seen_ms;
} TrackerDevice;

bool ble_tracker_hal_start_scan(void);
void ble_tracker_hal_stop_scan(void);
TrackerDevice* ble_tracker_hal_get_devices(uint16_t* count);

const char* ble_tracker_kind_label(TrackerKind k);
