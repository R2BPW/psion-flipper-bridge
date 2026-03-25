# Hardware Setup

## Components

| Device | Model | Role |
|--------|-------|------|
| Psion MC218 | EPOC Release 5, ARM710T | User interface, IR TX/RX |
| Flipper Zero | Stock firmware | IR relay, protocol bridge |
| WiFi Devboard | Official Flipper ESP32-S2-WROVER | WiFi + LLM API access |

## Connections

### Flipper <-> ESP32

The official WiFi devboard plugs directly into the Flipper's GPIO header. No soldering, no wires.

- UART TX (ESP32 GPIO43) -> Flipper USART RX (header pin 14)
- UART RX (ESP32 GPIO44) -> Flipper USART TX (header pin 13)
- Power: supplied by Flipper via header
- Baud rate: 115200

### IR alignment

Point the Psion's IrDA window at the Flipper's IR port. Distance: 10-30 cm.

```
  [Psion MC218]          [Flipper Zero + ESP32]
  +--------------+        +----------+--------+
  |   IrDA       |--IR-->|  TSOP    | ESP32  |
  |   window     |<--IR--|  IR LED  | WiFi   |
  +--------------+  ~20cm +----------+--------+
```

The Psion's IrDA port is on the left side of the device. The Flipper's IR receiver (TSOP) is on top, IR LED is also on top. The ESP32 devboard sits on the back of the Flipper.

## Psion IrDA details

- The MC218 IrDA port operates in SIR (Serial Infrared) mode
- TTY:B is the serial device name for the IR port in EPOC OPL
- OPX opens port via RBusDevComm at 38400 baud for TX (ternary encoding)
- OPL opens TTY:B at 4800 baud for RX (pulse-width decoding)
- Port contention: OPX must close before TTY:B can open for RX

## Flipper IR details

- InfraredWorker in raw mode (signal decoding disabled)
- Raw signal timeout: 150ms (`INFRARED_RAW_RX_TIMING_DELAY_US`)
- TX uses `infrared_send_raw_ext` with 36kHz carrier, 33% duty
- Optimized timings: bit0=8ms, bit1=16ms, space=3ms
- Max 1024 timings per `infrared_send_raw_ext` call

## ESP32 details

- ESP32-S2-WROVER (official Flipper WiFi devboard)
- Arduino framework
- WiFi with WPA2, TLS to OpenRouter API
- `client.setInsecure()` -- no certificate pinning (simplicity over security)
- HardwareSerial(1) on GPIO43/44 for Flipper communication
- Firmware: `esp32/simple_bridge/simple_bridge.ino`

### Flashing ESP32

Connect ESP32 directly to computer via USB (not through Flipper):

```sh
# Hold BOOT, press RESET, release BOOT
arduino-cli compile --fqbn esp32:esp32:esp32s2 esp32/simple_bridge/simple_bridge.ino
arduino-cli upload --fqbn esp32:esp32:esp32s2 --port /dev/cu.usbmodem01 esp32/simple_bridge/simple_bridge.ino
# Press RESET after flashing
```
