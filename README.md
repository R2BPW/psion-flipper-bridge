# IR Bridge: Ericsson MC218 <-> Flipper Zero <-> LLM

<img width="579" height="286" alt="image" src="https://github.com/user-attachments/assets/256f465e-d1be-4f1a-aeeb-36bec438ccfd" />

A working bridge that lets an **Ericsson MC218** (or a Psion 5mx, a 1999 palmtop with EPOC R5) chat with a large language model via infrared. The Psion types a question, sends it over IR using a 5-bit PWM protocol, a Flipper Zero decodes it and relays to an ESP32 over UART, the ESP32 queries an LLM over WiFi, and the response travels back the same path.


```
Ericsson MC218
  |  IR (5-bit PWM, IrDA connect bursts)
  v
Flipper Zero  (IR Bridge FAP)
  |  UART 115200 baud
  v
ESP32-S2 WiFi Devboard
  |  HTTPS / OpenRouter API
  v
LLM (OpenRouter)
  |
  v  (response returns same path)
Flipper Zero  (pulse-width IR TX + EOT byte)
  |  IR -> TTY:B SIR decoder, 4800 baud
  v
Ericsson MC218
```

## Repository layout

```
psion/
  irchat.opl            Psion OPL chat program (TX: 5-bit PWM, RX: pulse-width + EOT)

flipper/ir_bridge/
  ir_bridge.c           Flipper Zero FAP source (PWM RX decoder, pulse-width TX, ESP32 UART)
  application.fam       Flipper app manifest

esp32/
  llm_bridge/
    llm_bridge.ino      Advanced ESP32 bridge with RPC opcode table
  simple_bridge/
    simple_bridge.ino   Minimal ESP32 bridge (single Q: query protocol)

docs/
  PROTOCOL.md           Detailed protocol specification
  HARDWARE.md           Wiring and hardware notes
  OPL_NOTES.md          EPOC OPL compilation gotchas
```

## Psion -> Flipper: 5-bit PWM protocol

Each character is encoded as 7 IrDA connection attempt bursts with timing that encodes bits:

| Component | OPL PAUSE | Actual gap (incl. ~955ms IrDA overhead) |
|-----------|-----------|----------------------------------------|
| Start space | PAUSE 20 (1000ms) | ~1955ms |
| Bit 0 space | PAUSE 5 (250ms) | ~1205ms |
| Bit 1 space | PAUSE 11 (550ms) | ~1505ms |
| Parity space | PAUSE 5 or 11 | ~1205 or ~1505ms |

- Character code: A=0, B=1, ... Z=25, space=26 (5 bits, MSB first)
- 6th bit: even parity (XOR of 5 data bits)
- End of message: extra IrFlash + PAUSE 40 (2s silence)

Flipper thresholds: bit1 >= 1350ms, start >= 1730ms, timeout = 4000ms.

**Why IrDA connect attempts?** The Psion's TSOP-like IR receiver can't decode SIR protocol from non-EPOC devices, but it CAN detect IrDA connection attempt bursts as IR activity. Each `IrDAConnectToSend&:` call produces a detectable flash.

## Flipper -> Psion: pulse-width IR + EOT

The Flipper encodes each ASCII byte as 8 mark/space pairs at 36 kHz carrier:

| Symbol | Mark duration |
|--------|--------------|
| Start | 50 ms |
| Bit 0 | 10 ms |
| Bit 1 | 26 ms |
| End | 75 ms |
| Space between marks | 5 ms |

The Psion reads TTY:B (IrDA serial port) at 4800 baud. SIR decoder interprets marks as byte values: >= `$E0` = bit 1, <= `$90` = bit 0, between = marker.

**End-of-message:** Flipper sends byte `0x04` (ASCII EOT) after all data. Psion detects `byte% = 4` and exits receive loop, immediately allowing next message input.

Data is sent in chunks of 63 bytes back-to-back (gap between `infrared_send_raw_ext` calls is ~100us, invisible to SIR). Max response: 200 characters.

## Flipper <-> ESP32: UART protocol

115200 baud, newline-terminated ASCII:

| Direction | Frame | Meaning |
|-----------|-------|---------|
| Flipper -> ESP32 | `Q:<text>\n` | LLM query |
| Flipper -> ESP32 | `OP:HH:params\n` | RPC opcode (advanced bridge) |
| Flipper -> ESP32 | `PING\n` | Heartbeat |
| ESP32 -> Flipper | `R:<text>\n` | LLM response |
| ESP32 -> Flipper | `S:<status>\n` | Status (WIFI_OK, ASKING_LLM, ...) |
| ESP32 -> Flipper | `S:TOKENS:nnnn\n` | Token usage |
| ESP32 -> Flipper | `E:<reason>\n` | Error |

## Hardware

| Component | Notes |
|-----------|-------|
| Ericsson MC218 | EPOC Release 5, OPL. Built-in IrDA port. |
| Flipper Zero | Stock firmware with IR subsystem |
| Flipper WiFi Devboard | Official ESP32-S2-WROVER, plugs into Flipper header |

No additional IR hardware needed. The Psion's IrDA port and Flipper's built-in TSOP receiver + IR LED are used directly. Point them at each other, ~10-30 cm distance.

## Building

### Flipper app

```sh
cd flipper/ir_bridge
ufbt          # or: python3 -m ufbt build
# Copy dist/ir_bridge.fap to SD: /ext/apps/Infrared/
```

### ESP32 sketch

Edit `esp32/simple_bridge/simple_bridge.ino` and set your credentials:

```cpp
#define WIFI_SSID  "your_network"
#define WIFI_PASS  "your_password"
#define API_KEY    "sk-or-v1-..."   // from https://openrouter.ai/
```

```sh
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s2:CDCOnBoot=cdc" \
  esp32/simple_bridge/ \
  --output-dir build
```

Flash: hold BOOT on devboard, press RESET, flash with esptool, power-cycle.

### Psion OPL

Requires EPOC SDK with OPLTRAN.EXE (runs under Wine on macOS/Linux):

```sh
unix2dos psion/irchat.opl
wine path/to/OPLTRAN.EXE "C:\\irchat.opl"
# Copy irchat.opo to Psion via IR, cable, or CF card
```

The OPL source uses `INCLUDE "SYSTEM.OXH"` for IrDA functions.

## Usage

1. Flash **IR Bridge** FAP to Flipper, launch from Infrared category
2. Power on ESP32 devboard — it connects to WiFi and reports `S:WIFI_OK`
3. On Psion, run `irchat.opo`, type a message (A-Z and spaces), press Enter
4. Psion sends via IR (watch callback counter on Flipper), then enters Listening mode
5. Flipper dispatches to ESP32 -> LLM -> response arrives back on Psion
6. Psion prints response and prompts for next message

### Flipper controls

| Button | Action |
|--------|--------|
| OK | Send received message to LLM (manual trigger) |
| Left | Clear display / reset state |
| Right | Ping ESP32 |
| Back | Exit app |

## Known limitations

- Psion TX alphabet: A-Z + space only (no punctuation/numbers)
- IrDA connect overhead ~955ms per burst -> ~3.5s per character
- LLM response capped at 200 chars (IR TX buffer constraint: MAX_TIMINGS_AMOUNT=1024)
- Psion receives only first burst of multi-burst responses (blocking IOW, no async timeout in OPL)
- `infrared_send_raw_ext` timing is approximate; long transmissions may drift
- TTY:B may be busy after IrDA TX; Psion retries up to 15 times (75s)

## Why

The MC218 uses a proprietary serial connector that's been out of production for 25 years. This project eliminates the need for any cable by using the built-in IrDA port for bidirectional communication with modern LLMs.
