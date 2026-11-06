#include "furi_hal_nrf24.h"

#include <esp_log.h>
#include <furi.h>
#include <furi_hal_nvs_blob.h>
#include <furi_hal_power.h>
#include <furi_hal_resources.h>
#include <furi_hal_spi.h>
#include <furi_hal_subghz.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include "boards/board.h"

static const char* TAG = "FuriHalNrf24";

#if defined(BOARD_NRF24_EXT_MODULE)
#define FURI_HAL_NRF24_HAS_EXT 1
#else
#define FURI_HAL_NRF24_HAS_EXT 0
#endif

#define FURI_HAL_NRF24_NVS_NAMESPACE "nrf24"
#define FURI_HAL_NRF24_NVS_KEY       "ext"
#define FURI_HAL_NRF24_BLOB_MAGIC    0xE2u
#define FURI_HAL_NRF24_BLOB_SIZE     4u

/* NRF24 SPI: W_REGISTER(CONFIG) = 0 -> power down (also valid when no chip answers). */
#define NRF24_CMD_W_CONFIG  0x20u
#define NRF24_CMD_W_REGISTER 0x20u
#define NRF24_CMD_R_REGISTER 0x00u
#define NRF24_REG_RF_CH      0x05u
#define NRF24_CONFIG_PWR_UP  0x02u

typedef struct {
    FuriHalNrf24ExtConfig config;
    bool config_loaded;
    /* Session reference count. Scenes nest their hardware use (e.g. the Jam scene keeps
     * its worker — and thus a session — alive while it pushes the Activity-scan scene,
     * which opens its own session): the route must survive until the LAST holder ends it,
     * so begin/end are balanced increments, routing only on 0->1 and tearing down on 1->0. */
    volatile uint8_t refcount;
    bool external; /* the open session routes the external module */
    GpioPin csn; /* header pins held while `external` */
    GpioPin ce;
    FuriMutex* mutex;
} FuriHalNrf24;

static FuriHalNrf24 furi_hal_nrf24 = {
    .csn = {.port = NULL, .pin = UINT16_MAX},
    .ce = {.port = NULL, .pin = UINT16_MAX},
};
