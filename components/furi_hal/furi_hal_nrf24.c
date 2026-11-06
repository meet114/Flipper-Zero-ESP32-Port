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

#define NRF24_CMD_W_CONFIG  0x20u
#define NRF24_CMD_W_REGISTER 0x20u
#define NRF24_CMD_R_REGISTER 0x00u
#define NRF24_REG_RF_CH      0x05u
#define NRF24_CONFIG_PWR_UP  0x02u

typedef struct {
    FuriHalNrf24ExtConfig config;
    bool config_loaded;
    volatile uint8_t refcount;
    bool external;
    GpioPin csn;
    GpioPin ce;
    FuriMutex* mutex;
} FuriHalNrf24;

static FuriHalNrf24 furi_hal_nrf24 = {
    .csn = {.port = NULL, .pin = UINT16_MAX},
    .ce = {.port = NULL, .pin = UINT16_MAX},
};

#if FURI_HAL_NRF24_HAS_EXT
static const char furi_hal_nrf24_owner[] = "NRF24 ext";
#endif

#ifdef BOARD_PIN_NRF24_CE
static const GpioPin furi_hal_nrf24_board_ce = {.port = NULL, .pin = BOARD_PIN_NRF24_CE};
#else
static const GpioPin furi_hal_nrf24_board_ce = {.port = NULL, .pin = UINT16_MAX};
#endif

/* Full implementation lives in the local journey tree / bundle.
 * This commit replaces a broken "$file:..." placeholder so the tree builds.
 * Prefer applying own-journey-66a5.bundle for the complete 17KB source. */

static FuriMutex* furi_hal_nrf24_lock(void) {
    static portMUX_TYPE create_lock = portMUX_INITIALIZER_UNLOCKED;
    FuriMutex* mutex = furi_hal_nrf24.mutex;
    if(!mutex) {
        FuriMutex* candidate = furi_mutex_alloc(FuriMutexTypeRecursive);
        furi_check(candidate);
        portENTER_CRITICAL(&create_lock);
        if(!furi_hal_nrf24.mutex) furi_hal_nrf24.mutex = candidate;
        mutex = furi_hal_nrf24.mutex;
        portEXIT_CRITICAL(&create_lock);
        if(mutex != candidate) furi_mutex_free(candidate);
    }
    furi_check(furi_mutex_acquire(mutex, FuriWaitForever) == FuriStatusOk);
    return mutex;
}

static void furi_hal_nrf24_unlock(FuriMutex* mutex) {
    furi_check(furi_mutex_release(mutex) == FuriStatusOk);
}

bool furi_hal_nrf24_ext_supported(void) {
    return FURI_HAL_NRF24_HAS_EXT;
}

bool furi_hal_nrf24_ext_pin_allowed(uint8_t pin) {
#if FURI_HAL_NRF24_HAS_EXT
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(gpio_pins[i].pin && gpio_pins[i].pin->pin == pin) return true;
    }
#else
    (void)pin;
#endif
    return false;
}

static bool furi_hal_nrf24_config_valid(const FuriHalNrf24ExtConfig* config) {
    return config->csn_pin != config->ce_pin && furi_hal_nrf24_ext_pin_allowed(config->csn_pin) &&
           furi_hal_nrf24_ext_pin_allowed(config->ce_pin);
}

static void furi_hal_nrf24_config_defaults(FuriHalNrf24ExtConfig* config) {
    config->use_external = false;
#if FURI_HAL_NRF24_HAS_EXT
    config->csn_pin = BOARD_PIN_EXT_GPIO_4;
    config->ce_pin = BOARD_PIN_EXT_GPIO_5;
#else
    config->csn_pin = UINT8_MAX;
    config->ce_pin = UINT8_MAX;
#endif
}

static void furi_hal_nrf24_config_load_locked(void) {
    if(furi_hal_nrf24.config_loaded) return;
    FuriHalNrf24ExtConfig config;
    furi_hal_nrf24_config_defaults(&config);
#if FURI_HAL_NRF24_HAS_EXT
    uint8_t blob[FURI_HAL_NRF24_BLOB_SIZE] = {0};
    if(furi_hal_nvs_blob_read(
           FURI_HAL_NRF24_NVS_NAMESPACE, FURI_HAL_NRF24_NVS_KEY, blob, sizeof(blob)) &&
       blob[0] == FURI_HAL_NRF24_BLOB_MAGIC) {
        const FuriHalNrf24ExtConfig stored = {
            .use_external = blob[1] == 1,
            .csn_pin = blob[2],
            .ce_pin = blob[3],
        };
        if(furi_hal_nrf24_config_valid(&stored)) config = stored;
    }
#endif
    furi_hal_nrf24.config = config;
    furi_hal_nrf24.config_loaded = true;
}

void furi_hal_nrf24_ext_get_config(FuriHalNrf24ExtConfig* config) {
    furi_check(config);
    FuriMutex* mutex = furi_hal_nrf24_lock();
    furi_hal_nrf24_config_load_locked();
    *config = furi_hal_nrf24.config;
    furi_hal_nrf24_unlock(mutex);
}

bool furi_hal_nrf24_ext_uses_pin(uint8_t pin) {
    if(!FURI_HAL_NRF24_HAS_EXT) return false;
    FuriMutex* mutex = furi_hal_nrf24_lock();
    furi_hal_nrf24_config_load_locked();
    const FuriHalNrf24ExtConfig* c = &furi_hal_nrf24.config;
    const bool used = c->use_external && (c->csn_pin == pin || c->ce_pin == pin);
    furi_hal_nrf24_unlock(mutex);
    return used;
}

bool furi_hal_nrf24_ext_set_config(const FuriHalNrf24ExtConfig* config) {
    furi_check(config);
    if(!FURI_HAL_NRF24_HAS_EXT || !furi_hal_nrf24_config_valid(config)) return false;
    if(config->use_external &&
       (furi_hal_subghz_ext_uses_pin(config->csn_pin) ||
        furi_hal_subghz_ext_uses_pin(config->ce_pin))) {
        return false;
    }
    FuriMutex* mutex = furi_hal_nrf24_lock();
    furi_hal_nrf24_config_load_locked();
    furi_hal_nrf24.config = *config;
#if FURI_HAL_NRF24_HAS_EXT
    const uint8_t blob[FURI_HAL_NRF24_BLOB_SIZE] = {
        FURI_HAL_NRF24_BLOB_MAGIC,
        config->use_external ? 1 : 0,
        config->csn_pin,
        config->ce_pin,
    };
    (void)furi_hal_nvs_blob_write(
        FURI_HAL_NRF24_NVS_NAMESPACE, FURI_HAL_NRF24_NVS_KEY, blob, sizeof(blob));
#endif
    furi_hal_nrf24_unlock(mutex);
    return true;
}

const char* furi_hal_nrf24_ext_result_text(FuriHalNrf24ExtResult result) {
    switch(result) {
    case FuriHalNrf24ExtOk: return "OK";
    case FuriHalNrf24ExtUnsupported: return "Not supported on this board";
    case FuriHalNrf24ExtBadConfig: return "CSN and CE must be two different header pins";
    case FuriHalNrf24ExtBusy: return "NRF24 bus is busy";
    case FuriHalNrf24ExtPinInUse: return "A chosen pin is used by another feature";
    case FuriHalNrf24ExtNotDetected: return "No NRF24 detected on the chosen pins";
    default: return "Unknown error";
    }
}

bool furi_hal_nrf24_session_is_active(void) {
    return furi_hal_nrf24.refcount > 0;
}

bool furi_hal_nrf24_session_is_external(void) {
    return furi_hal_nrf24.refcount > 0 && furi_hal_nrf24.external;
}

const GpioPin* furi_hal_nrf24_get_ce_pin(void) {
    return (furi_hal_nrf24.refcount > 0 && furi_hal_nrf24.external) ? &furi_hal_nrf24.ce :
                                                                      &furi_hal_nrf24_board_ce;
}

FuriHalNrf24ExtResult furi_hal_nrf24_session_begin(void) {
    FuriMutex* mutex = furi_hal_nrf24_lock();
    furi_hal_nrf24_config_load_locked();
    if(furi_hal_nrf24.refcount == 0) {
        furi_hal_nrf24.external = false;
        furi_hal_gpio_init(
            &furi_hal_nrf24_board_ce, GpioModeOutputPushPull, GpioPullUp, GpioSpeedVeryHigh);
        furi_hal_gpio_write(&furi_hal_nrf24_board_ce, false);
        furi_hal_nrf24.refcount = 1;
        furi_hal_power_insomnia_enter();
    } else {
        furi_hal_nrf24.refcount++;
        furi_hal_power_insomnia_enter();
    }
    furi_hal_nrf24_unlock(mutex);
    return FuriHalNrf24ExtOk;
}

void furi_hal_nrf24_session_end(void) {
    FuriMutex* mutex = furi_hal_nrf24_lock();
    if(furi_hal_nrf24.refcount > 1) {
        furi_hal_nrf24.refcount--;
        furi_hal_power_insomnia_exit();
    } else if(furi_hal_nrf24.refcount == 1) {
        furi_hal_gpio_write(&furi_hal_nrf24_board_ce, false);
        furi_hal_gpio_init_simple(&furi_hal_nrf24_board_ce, GpioModeAnalog);
        furi_hal_nrf24.refcount = 0;
        furi_hal_power_insomnia_exit();
    }
    furi_hal_nrf24_unlock(mutex);
}
