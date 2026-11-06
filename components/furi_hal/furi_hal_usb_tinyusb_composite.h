#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool furi_hal_usb_composite_install(
    uint16_t vid,
    uint16_t pid,
    const char* manuf,
    const char* product);

bool furi_hal_usb_composite_is_installed(void);

bool furi_hal_usb_composite_uninstall(void);

void furi_hal_usb_composite_restore_serial_jtag(void);

size_t furi_hal_usb_serial_jtag_read(uint8_t* buf, size_t len);
size_t furi_hal_usb_serial_jtag_write(const uint8_t* buf, size_t len);

#ifdef __cplusplus
}
#endif
