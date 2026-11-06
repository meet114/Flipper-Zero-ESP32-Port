# Large files — restored

These sources now match the local PR135+nRF24 journey tree on `cursor/own-journey-66a5`:

| Path | Status |
|------|--------|
| `components/btshim/btshim.c` | **Complete** (NimBLE, `ble_core_stack_down`) |
| `components/wifi/wlan_hal.c` | **Complete** (survey / deauth / beacon spam) |
| `components/furi_hal/furi_hal_nrf24.c` | **Complete** |

Temporary `.journey-tmp/*.b64` chunks and `journey-restore-large-files.yml` were used to assemble them past MCP size limits; they can be removed once you no longer need the restore workflow.

Firmware built from this tree: `furi_esp32-pr135-nrf24.bin`.
