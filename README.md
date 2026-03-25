# IR Bridge: Ericsson MC218 <-> Flipper Zero <-> LLM

<img width="579" height="286" alt="image" src="https://github.com/user-attachments/assets/256f465e-d1be-4f1a-aeeb-36bec438ccfd" />

A working bridge that lets an **Ericsson MC218** (or Psion 5mx, a 1999 palmtop with EPOC R5) chat with a large language model via infrared. The Psion types a question, a native OPX encodes and transmits the entire message in a single IR burst (~100 ms for "HELLO"), a Flipper Zero decodes the raw signal and relays it over UART to an ESP32 WiFi devboard, which queries an LLM API, and the response travels back the same path.

The project does **not** use the IrDA protocol. Instead, a custom physical-layer protocol re-purposes the Psion's IrDA transceiver hardware: driving the SIR modulator at 38400 baud produces pulses at ~38 kHz — exactly the carrier frequency the Flipper's TSOP receiver is tuned to. The result is reliable, high-speed IR communication using only the built-in hardware of both devices.

```
Ericsson MC218
  |  IR (38 kHz carrier, ternary encoding, single SIR frame)
  v
Flipper Zero  (IR Bridge FAP)
  |  UART (115200 baud, GPIO header)
  v
ESP32-S2 WiFi Devboard  (plugs into Flipper)
  |  WiFi / TLS
  v
LLM (OpenRouter API)
  |
  v  (response returns same path)
Flipper Zero  (pulse-width IR TX)
  |  IR -> TTY:B SIR decoder, 4800 baud
  v
Ericsson MC218
```

## Repository layout

```
psion/
  chat_a.opl              Chat program (production: OPX TX + TTY:B RX)
  irchat_opx.opl          Legacy chat program (original version)

opx/
  irflash_sir.cpp         OPX source (38400 baud, mark/space encoding)
  irflash.h               OPX C++ header
  irflash.mmp             Symbian build file
  IRFLASH.OXH             OPL interface header

flipper/ir_bridge/
  ir_bridge.c             Flipper Zero FAP source (fast38 + legacy decoders)
  application.fam         Flipper app manifest

esp32/
  simple_bridge/
    simple_bridge.ino     ESP32 LLM bridge (WiFi, OpenRouter API)
  llm_bridge/
    llm_bridge.ino        ESP32 LLM bridge with RPC opcodes

release/
  irflash.opx             Compiled OPX for MC218
  ir_bridge.fap           Compiled Flipper FAP
  IRFLASH.OXH             OPX header (copy to MC218)

docs/
  PROTOCOL.md             Protocol specification
  HARDWARE.md             Hardware notes
  OPL_NOTES.md            EPOC OPL compilation gotchas
  SPEEDUP.org             Speed optimization notes
```

## Psion -> Flipper: 38 kHz carrier protocol

The entire message is transmitted in a **single SIR frame** at 38400 baud. At this rate, each bit period is ~26 us — nearly identical to the 38 kHz period (26.3 us) that the Flipper's TSOP receiver is tuned to. A byte of `0x00` generates nine SIR pulses at ~38 kHz (carrier "on"); a byte of `0xFF` generates no pulses (carrier "off").

### Encoding

| Symbol | Mark (0x00 bytes) | Space (0xFF bytes) |
|--------|------------------|-------------------|
| Header | 32 (~8.3 ms) | 16 (~4.2 ms) |
| Digit 0 | 4 (~1.0 ms) | 4 (~1.0 ms) |
| Digit 1 | 4 (~1.0 ms) | 8 (~2.1 ms) |
| Digit 2 | 4 (~1.0 ms) | 12 (~3.1 ms) |
| Trail | 4 (~1.0 ms) | — |

Character code: A=0, B=1, ... Z=25, space=26. Each character is decomposed into three base-3 (ternary) digits. One character ~ 5 ms. "HELLO" transmits in ~100 ms.

## Flipper -> Psion: pulse-width IR

The Flipper encodes each ASCII byte as 8 mark/space pairs at 36 kHz carrier:

| Symbol | Mark duration |
|--------|--------------|
| Start | 50 ms |
| Bit 0 | 8 ms |
| Bit 1 | 16 ms |
| End | 75 ms |
| Space between marks | 3 ms |

The Psion reads TTY:B (IrDA serial port) at 4800 baud. SIR decoder interprets marks as byte values: >= `$E0` = bit 1, <= `$90` = bit 0, between = marker.

**End-of-message:** Flipper sends byte `0x04` (ASCII EOT). Max response: 200 characters. ~120 ms per byte, ~24 seconds for 200 characters.

## Flipper <-> ESP32: UART protocol

115200 baud, 8N1, newline-terminated ASCII over Flipper GPIO header:

| Direction | Frame | Meaning |
|-----------|-------|---------|
| Flipper -> ESP32 | `Q:<text>\n` | LLM query |
| ESP32 -> Flipper | `R:<text>\n` | LLM response |
| ESP32 -> Flipper | `S:<status>\n` | Status |
| ESP32 -> Flipper | `E:<reason>\n` | Error |

## Hardware

| Component | Notes |
|-----------|-------|
| Ericsson MC218 | EPOC Release 5, OPL + OPX. Built-in IrDA port. |
| Flipper Zero | Stock firmware with IR subsystem |
| ESP32-S2 WiFi Devboard | Official Flipper devboard, plugs into GPIO header. No wires. |

No additional IR hardware needed. The ESP32 devboard plugs directly into the Flipper — no soldering, no cables. Point the Psion's IrDA window at the Flipper's IR transceiver, ~10-30 cm distance.

## Building

### Flipper app

```sh
cd flipper/ir_bridge
ufbt build
# Copy ir_bridge.fap to Flipper SD: /ext/apps/Infrared/
```

### ESP32 firmware

Set your WiFi credentials and OpenRouter API key in `esp32/simple_bridge/simple_bridge.ino`, then:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s2 esp32/simple_bridge/simple_bridge.ino
arduino-cli upload --fqbn esp32:esp32:esp32s2 --port /dev/cu.usbmodem01 esp32/simple_bridge/simple_bridge.ino
```

### Psion OPX (native C++ DLL)

Built with EPOC Release 5 C++ SDK under Wine (GCC ARM cross-compiler + PETRAN):

```sh
# See opx/ directory for source
# Pre-built binary: release/irflash.opx
# Copy to MC218: D:\System\Opx\IRFLASH.OPX
```

### Psion OPL

Requires EPOC SDK with OPLTRAN.EXE (runs under Wine on macOS/Linux):

```sh
unix2dos psion/chat_a.opl
wine path/to/OPLTRAN.EXE "C:\\chat_a.opl"
# Copy chat_a.opo to MC218 via CF card
```

## Usage

1. Plug ESP32 WiFi devboard into Flipper GPIO header
2. Flash **IR Bridge** FAP to Flipper, launch from Infrared category
3. On Psion, run `chat_a.opo`, type a message (A-Z and spaces), press Enter
4. The OPX encodes and transmits the entire message in one IR burst (~100 ms)
5. Flipper decodes, forwards to ESP32 -> WiFi -> LLM -> response arrives back on Psion

### Flipper controls

| Button | Action |
|--------|--------|
| Up | Send "ABC" to Psion (test) |
| Down | Send "Hello" to Psion (test) |
| OK | Send received message to LLM |
| Left | Clear display / reset state |
| Back | Exit app |

## Known limitations

- Psion TX alphabet: A-Z + space only (no punctuation/numbers)
- LLM response capped at 200 chars (IR TX buffer constraint)
- Flipper -> Psion speed: ~24 seconds for 200 characters (4800 baud SIR limit)
- The OPX uses the IrDA UART hardware but does not implement IrDA protocol
- Port contention: OPX must be closed before TTY:B can receive (chat_a handles this automatically)

## Why

The MC218 uses a proprietary serial connector that's been out of production for 25 years. This project eliminates the need for any cable by re-purposing the built-in IrDA transceiver hardware for a custom 38 kHz IR protocol — turning a 1999 palmtop into a wireless LLM terminal. The ESP32 WiFi devboard plugs directly into the Flipper, making the entire setup cable-free.
