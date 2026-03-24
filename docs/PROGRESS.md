# Progress Log

## 2026-03-24: USB CDC bridge to RPi

### What changed

Replaced ESP32 WiFi devboard with Raspberry Pi connected via USB-C.

**Previous architecture:**
```
MC218 --IR--> Flipper Zero --UART/GPIO--> ESP32 --WiFi--> OpenRouter LLM
```

**New architecture:**
```
MC218 --IR--> Flipper Zero --USB-C/CDC--> Raspberry Pi --WiFi/local--> LLM
```

### Files modified

- **`flipper/ir_bridge/ir_bridge.c`** — v12 -> v13 [USB]
  - Replaced `furi_hal_serial_*` (hardware UART) with `furi_hal_cdc_*` (USB CDC)
  - Added `furi_hal_usb.h`, `furi_hal_usb_cdc.h`
  - Struct: `FuriHalSerialHandle* esp_serial` -> `FuriHalUsbInterface* usb_if_prev`
  - RX callback: byte-by-byte UART -> 64-byte CDC chunks via `furi_hal_cdc_receive()`
  - TX: `furi_hal_serial_tx()` -> `furi_hal_cdc_send(0, ...)`
  - Init: UART acquire -> `furi_hal_usb_set_config(&usb_cdc_single)` + CDC callbacks
  - Cleanup: UART release -> `furi_hal_usb_set_config(prev)` to restore USB
  - Display: "WiFi"/"noWF" -> "RPi"/"noRPi"

### Files added

- **`rpi/bridge.py`** — RPi-side bridge (replaces `esp32/simple_bridge.ino`)
  - Serial CDC listener on `/dev/ttyACM0`
  - LLM backends: OpenRouter (cloud) and Ollama (local)
  - File system browser (LS, CD, UP, CAT, PWD, HOME, INFO)
  - Auto-reconnect on USB disconnect
  - Zero deps beyond `pyserial`

### Files rebuilt

- **`release/ir_bridge.fap`** — rebuilt with USB CDC, API 87.1

### Files unchanged

- `psion/irchat.opl` — no changes needed, protocol is transparent
- `release/irchat.opo` — same binary
- `esp32/` — kept for reference, no longer required

### Deployment (darkstar.local)

- `bridge.py` deployed to `tema@darkstar.local:~/bridge.py`
- `python3-serial` installed via apt
- User `tema` already in `dialout` group
- Tested and working: PING, Q:LS, file browsing confirmed over USB CDC

### File system commands (from MC218)

| Command   | Action                          |
|-----------|---------------------------------|
| LS        | List current directory          |
| LS B      | List page 2 (B=2, C=3...)      |
| CD A      | Enter directory at index A      |
| UP        | Go to parent directory          |
| CAT A     | Read file at index A            |
| CAT A B   | Read file A, page 2            |
| INFO A    | Show file info (size, date)     |
| PWD       | Show current path               |
| HOME      | Return to home directory        |

### Known state

- LLM not tested yet (OPENROUTER_API_KEY not set on RPi)
- Ollama not installed on RPi yet
- ESP32 devboard no longer needed in the hardware chain
- `docs/HARDWARE.md` not yet updated for USB-C setup
