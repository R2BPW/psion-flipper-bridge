# IR Bridge: Ericsson MC218 <-> Flipper Zero <-> LLM

<img width="579" height="286" alt="image" src="https://github.com/user-attachments/assets/256f465e-d1be-4f1a-aeeb-36bec438ccfd" />

A working bridge that lets an **Ericsson MC218** (or Psion 5mx, a 1999 palmtop with EPOC R5) chat with a large language model via infrared. The Psion types a question, a native OPX encodes and transmits the entire message in a single IR burst (~100 ms for "HELLO"), a Flipper Zero decodes the raw signal and relays it over USB CDC to a Raspberry Pi, the RPi queries an LLM, and the response travels back the same path.

The project does **not** use the IrDA protocol. Instead, a custom physical-layer protocol re-purposes the Psion's IrDA transceiver hardware: driving the SIR modulator at 38400 baud produces pulses at ~38 kHz — exactly the carrier frequency the Flipper's TSOP receiver is tuned to. The result is reliable, high-speed IR communication using only the built-in hardware of both devices.

```
Ericsson MC218
  |  IR (38 kHz carrier, ternary encoding, single SIR frame)
  v
Flipper Zero  (IR Bridge FAP)
  |  USB-C CDC
  v
Raspberry Pi
  |  WiFi / local LLM
  v
LLM (OpenRouter / Ollama)
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
  irchat_opx.opl          Chat program (38 kHz protocol via OPX)
  opxtest.opl             OPX diagnostic tool
  irchat_sir.opl          Legacy: SIR burst-timing (binary PWM)
  irchat_ter.opl          Legacy: SIR burst-timing (ternary)

opx/
  irflash_sir.cpp         OPX source (38400 baud, mark/space encoding)
  irflash.h               OPX C++ header
  irflash.mmp             Symbian build file
  IRFLASH.OXH             OPL interface header

flipper/ir_bridge/
  ir_bridge.c             Flipper Zero FAP source (fast38 + legacy decoders)
  application.fam         Flipper app manifest

rpi/
  bridge.py               RPi LLM bridge (USB CDC, OpenRouter / Ollama)

release/
  irflash.opx             Compiled OPX for MC218
  irchat_opx.opo          Compiled OPL chat program
  ir_bridge.fap           Compiled Flipper FAP
  IRFLASH.OXH             OPX header (copy to MC218)

docs/
  PROTOCOL.md             Protocol specification
  HARDWARE.md             Hardware notes
  OPL_NOTES.md            EPOC OPL compilation gotchas
  PROGRESS.md             Development log
```

## Psion -> Flipper: 38 kHz carrier protocol

The entire message is transmitted in a **single SIR frame** at 38400 baud. At this rate, each bit period is ~26 µs — nearly identical to the 38 kHz period (26.3 µs) that the Flipper's TSOP receiver expects. A byte of `0x00` generates nine SIR pulses at ~38 kHz (carrier "on"); a byte of `0xFF` generates no pulses (carrier "off").

This is a **custom physical-layer protocol** (OSI Layer 1) that re-purposes the SIR pulse shaper as a 38 kHz carrier generator, with a minimal data-link framing (Layer 2) on top.

### Encoding

| Symbol | Mark (0x00 bytes) | Space (0xFF bytes) |
|--------|------------------|-------------------|
| Header | 32 (~8.3 ms) | 16 (~4.2 ms) |
| Digit 0 | 4 (~1.0 ms) | 4 (~1.0 ms) |
| Digit 1 | 4 (~1.0 ms) | 8 (~2.1 ms) |
| Digit 2 | 4 (~1.0 ms) | 12 (~3.1 ms) |
| Trail | 4 (~1.0 ms) | — |

Character code: A=0, B=1, ... Z=25, space=26. Each character is decomposed into three base-3 (ternary) digits.

Flipper thresholds: digit 1 >= 1.5 ms, digit 2 >= 2.5 ms, header >= 4 ms. One character ≈ 5 ms. "HELLO" transmits in ~100 ms including SIR frame overhead.

### Why 38400 baud?

The Flipper's TSOP infrared receiver has a bandpass filter tuned to 38 kHz. Ordinary IrDA at 4800 or 9600 baud produces SIR pulses at the wrong repetition rate — the TSOP detects them unreliably. At 38400 baud, the pulse repetition rate matches the TSOP's design frequency, so the receiver locks on cleanly. No IrDA protocol is involved: only the IrDA transceiver *hardware* (LED + SIR modulator) is used.

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

**End-of-message:** Flipper sends byte `0x04` (ASCII EOT). Max response: 200 characters.

## Flipper <-> RPi: USB CDC protocol

Newline-terminated ASCII over USB CDC (`/dev/ttyACM0`):

| Direction | Frame | Meaning |
|-----------|-------|---------|
| Flipper -> RPi | `Q:<text>\n` | LLM query |
| RPi -> Flipper | `R:<text>\n` | LLM response |
| RPi -> Flipper | `S:<status>\n` | Status |
| RPi -> Flipper | `E:<reason>\n` | Error |

The RPi bridge (`rpi/bridge.py`) supports OpenRouter (cloud) and Ollama (local) LLM backends, plus a file system browser (LS, CD, CAT, PWD, HOME, INFO).

## Hardware

| Component | Notes |
|-----------|-------|
| Ericsson MC218 | EPOC Release 5, OPL + OPX. Built-in IrDA port. |
| Flipper Zero | Stock firmware with IR subsystem |
| Raspberry Pi | Any model with USB. Connected to Flipper via USB-C CDC. |

No additional IR hardware needed. Point the Psion's IrDA window at the Flipper's IR transceiver, ~10-30 cm distance.

## Building

### Flipper app

```sh
cd flipper/ir_bridge
ufbt build
# Copy ir_bridge.fap to Flipper SD: /ext/apps/Infrared/
```

### RPi bridge

```sh
pip install pyserial
# Deploy rpi/bridge.py to RPi
python3 bridge.py
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
unix2dos psion/irchat_opx.opl
wine path/to/OPLTRAN.EXE "C:\\irchat_opx.opl"
# Copy irchat_opx.opo to MC218 via CF card
```

The OPX auto-loads via `DECLARE OPX` — no `LOADM` needed on EPOC R5.

## Usage

1. Flash **IR Bridge** FAP to Flipper, launch from Infrared category
2. Start `bridge.py` on RPi (connected via USB-C)
3. On Psion, run `irchat_opx.opo`, type a message (A-Z and spaces), press Enter
4. The OPX encodes and transmits the entire message in one IR burst (~100 ms)
5. Flipper decodes, dispatches to RPi -> LLM -> response arrives back on Psion
6. Psion prints response and prompts for next message

### Flipper controls

| Button | Action |
|--------|--------|
| OK | Send received message to LLM |
| Left | Clear display / reset state |
| Right | Toggle gap recording to SD |
| Back | Exit app |

## Known limitations

- Psion TX alphabet: A-Z + space only (no punctuation/numbers)
- LLM response capped at 200 chars (IR TX buffer constraint)
- Psion receives only first burst of multi-burst responses (blocking IOW in OPL)
- The OPX uses the IrDA UART hardware but does not implement IrDA protocol — once the OPX has opened the port, OPL serial I/O (`LPRINT`) will not function until reboot
- TTY:B may be busy after OPX releases the port; Psion retries up to 15 times

## Why

The MC218 uses a proprietary serial connector that's been out of production for 25 years. This project eliminates the need for any cable by re-purposing the built-in IrDA transceiver hardware for a custom 38 kHz IR protocol — turning a 1999 palmtop into a wireless LLM terminal.
