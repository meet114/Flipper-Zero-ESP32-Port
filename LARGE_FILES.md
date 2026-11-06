# Large files not fully synced via API

GitHub MCP `push_files` truncates files above ~12–20 KB. On this branch:

| Path | Status |
|------|--------|
| `components/btshim/btshim.c` | Still Bluedroid — need NimBLE (~45 KB) |
| `components/wifi/wlan_hal.c` | Stub only — need full PR135 HAL (~37 KB) |
| `components/furi_hal/furi_hal_nrf24.c` | Simplified stub — need full (~17 KB) |

**Fix:** apply overlays from the agent download server (`large-files/` or `apply-large-files.sh`), or import `own-journey-66a5.bundle`. See `PUSH-TO-YOUR-FORK.md` in the kit repo / download page.

Firmware built from the complete local tree is already available as `furi_esp32-pr135-nrf24.bin`.
