# Large files sync status

GitHub MCP `push_files` often truncates above ~12–20 KB.

| Path | Remote status |
|------|---------------|
| `components/furi_hal/furi_hal_nrf24.c` | **Complete** (~17 KB full session API) |
| `components/btshim/btshim.c` | **Incomplete** — NimBLE prefix only; need full ~45 KB |
| `components/wifi/wlan_hal.c` | **Incomplete** — truncated; need full ~37 KB |

## Finish btshim + wlan_hal

From the agent download page while it is up:

```bash
git clone -b cursor/own-journey-66a5 https://github.com/meet114/Flipper-Zero-ESP32-Port.git
cd Flipper-Zero-ESP32-Port
curl -fsSL http://127.0.0.1:43145/apply-large-files.sh | bash -s -- .
# script also refreshes furi_hal_nrf24.c (already OK on remote)
git add components/btshim/btshim.c components/wifi/wlan_hal.c
git commit -m "journey: restore full NimBLE btshim + wlan_hal"
git push
```

Or import [own-journey-66a5.bundle](http://127.0.0.1:43145/own-journey-66a5.bundle) for the entire local tree.

Firmware built from the complete local tree: `furi_esp32-pr135-nrf24.bin`.
