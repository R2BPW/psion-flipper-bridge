# IR Bridge: Psion MC218 → Flipper Zero → LLM

A working bridge that lets a **Psion MC218** (1999 palmtop) chat with a large language model via infrared. The Psion types a question, sends it over IrDA, a Flipper Zero relays it to an ESP32 over UART, the ESP32 queries an LLM over WiFi, and the response travels back the same path to the Psion's screen.

```
Psion MC218
  │  IrDA (burst-count encoding)
  ▼
Flipper Zero  (IR Bridge app)
  │  UART 115200 baud
  ▼
ESP32-S2 WiFi Devboard
  │  HTTPS / OpenRouter API
  ▼
Gemini 2.0 Flash
  │  (response returns same path in reverse)
  ▼
Flipper Zero  (pulse-width IR encoding)
  │  IrDA → TTY:B 4800 baud
  ▼
Psion MC218
```

## Repository layout

```
ir_bridge/
  ir_bridge.c        Flipper Zero app (IR Bridge v6)
  application.fam    Flipper app manifest

esp32_bridge/
  esp32_bridge.ino   Arduino sketch for ESP32-S2 WiFi Devboard (LLM Bridge v3)

irchat.opl           Psion OPL program — send + receive (main program)
irsend3.opl          Psion OPL program — send only
irrecv2.opl          Psion OPL program — receive only
```

## How it works

### Psion → Flipper: burst-count encoding

The Psion has no easy way to send arbitrary serial data over IrDA to a non-EPOC device.
The solution: encode each letter as a **count of IrDA connection bursts**.

- `A` = 1 burst, `B` = 2 bursts, … `Z` = 26 bursts, space = 27 bursts
- 300 ms gap between bursts within one letter
- 4 s gap between letters
- Message ends with `N` (= 14 bursts) to trigger the LLM query

The Flipper's `InfraredWorker` counts incoming signals. After a 3-second silence it finalizes the letter, appends it to the message buffer, and sends `Q:<message>\n` over UART when it sees a trailing `N`.

### Flipper → ESP32: UART

- **Flipper pins:** USART header (FuriHalSerialIdUsart)
- **ESP32 pins:** HardwareSerial(1) on GPIO 43 (TX) / GPIO 44 (RX)
- **Baud rate:** 115200

Protocol (newline-terminated ASCII):

| Direction | Frame | Meaning |
|---|---|---|
| Flipper → ESP32 | `Q:<text>\n` | LLM query |
| Flipper → ESP32 | `PING\n` | Connectivity check |
| Flipper → ESP32 | `STATUS\n` | Request WiFi status |
| ESP32 → Flipper | `R:<text>\n` | LLM response |
| ESP32 → Flipper | `S:<status>\n` | Status (WIFI_OK, ASKING_LLM, …) |
| ESP32 → Flipper | `E:<reason>\n` | Error |

### ESP32 → LLM: HTTPS

The ESP32-S2 connects to OpenRouter (`openrouter.ai`) and calls `google/gemini-2.0-flash-001`.
Uses `HTTP/1.0` with `WiFiClientSecure` (certificate verification disabled) to avoid chunked transfer encoding complications. Response is read byte-by-byte until the connection closes, then the JSON `content` field is extracted.

### Flipper → Psion: pulse-width IR encoding

The Psion reads TTY:B (serial port via IR hardware) at **4800 baud**.
The Flipper encodes each character as a sequence of IR pulses at 36 kHz / 33% duty:

| Symbol | Mark duration |
|---|---|
| Start | 50 ms |
| Bit 0 | 10 ms |
| Bit 1 | 26 ms |
| End | 75 ms |
| Space between marks | 5 ms |

The Psion OPL program reads raw bytes from TTY:B with `IOW(-1, 1, …)` and decodes them: values ≥ `$E0` = bit 1, values ≤ `$90` = bit 0, values in between = end marker.

**Key quirk:** After IrDA burst-count transmission the Psion must call `IrDADisconnect:` to release TTY:B before it can open the port for reading. Without the disconnect TTY:B stays permanently busy.

## Hardware

| Component | Notes |
|---|---|
| Psion MC218 | EPOC Release 5, OPL 5 |
| Flipper Zero | Firmware with `infrared_worker` and `infrared_transmit` APIs |
| Flipper WiFi Devboard | Official ESP32-S2-WROVER devboard |

No additional IR hardware is needed — the Psion's built-in IrDA port and the Flipper's built-in IR receiver/LED are used directly.

## Building

### Flipper app

```sh
cd ir_bridge
python3 -m ufbt build
# Install via ufbt or copy .fap to SD card /apps/Infrared/
```

### ESP32 sketch

```sh
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s2:CDCOnBoot=cdc" \
  esp32_bridge/ \
  --output-dir esp32_bridge/build
```

Flash: hold BOOT, press RESET, then run esptool, then power-cycle.

**Before flashing:** edit `esp32_bridge.ino` and set your own WiFi credentials and OpenRouter API key:

```cpp
#define WIFI_SSID  "your_network"
#define WIFI_PASS  "your_password"
#define API_KEY    "sk-or-v1-..."   // https://openrouter.ai/
```

### Psion OPL

```sh
cd /path/to/opl/files
unix2dos irchat.opl
wine Epoc32/Release/Winc/Deb/OPLTRAN.EXE irchat.opl
# Copy the resulting .opo to Documents on the Psion
```

Requires the EPOC SDK with `SYSTEM.OXH` (System OPX, provides `IrDAConnectToSend&:`, `IrDADisconnect:`, etc.)

## Usage

1. Flash the Flipper app and launch **IR Bridge** from the Infrared category.
2. Power on the ESP32 devboard — it will connect to WiFi and send `S:WIFI_OK` to the Flipper.
3. On the Psion, run `irchat.opo`, type a message (A–Z and spaces only), end with `N`, press Enter.
4. Watch bursts being counted on the Flipper display, then `ESP: asking LLM...`
5. The LLM response appears on the Flipper screen and is transmitted back to the Psion over IR.

### Flipper button shortcuts (for testing)

| Button | Action |
|---|---|
| OK | Send `"Hi"` to Psion via IR |
| Up | Send `"ABC"` to Psion |
| Down | Send `"Hello"` to Psion |
| Left | Clear display / reset state |
| Right | Ping ESP32 / request status |
| Back | Exit |

## Known limitations

- Message alphabet is A–Z + space only (no punctuation or numbers from Psion side)
- LLM response is truncated to ~30 characters due to IR timing constraints
- The burst-count scheme is slow: about 5–10 seconds per character at full speed
- `infrared_send_raw_ext` timing on Flipper is approximate; very long IR transmissions may drift

## Why

Because a 25-year-old palmtop asking a 2025 AI model questions via infrared is delightful.
