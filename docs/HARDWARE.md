# Hardware Setup

## Components

| Device | Model | Role |
|--------|-------|------|
| Psion MC218 | EPOC Release 5, ARM710T | User interface, IR TX/RX |
| Flipper Zero | Stock firmware | IR relay, protocol bridge |
| WiFi Devboard | Official Flipper ESP32-S2-WROVER | WiFi + LLM API access |

## Wiring

### Flipper <-> ESP32

The official WiFi devboard plugs directly into the Flipper's GPIO header. No soldering.

- UART TX (ESP32 GPIO43) -> Flipper USART RX (header pin 14)
- UART RX (ESP32 GPIO44) -> Flipper USART TX (header pin 13)
- Power: supplied by Flipper via header

### IR alignment

Point the Psion's IrDA window at the Flipper's IR port. Distance: 10-30 cm.

```
  [Psion MC218]          [Flipper Zero]
  ┌─────────────┐        ┌──────────┐
  │   IrDA       ├──IR──>│  TSOP    │
  │   window    │<──IR──┤  IR LED  │
  └─────────────┘  ~20cm └──────────┘
```

The Psion's IrDA port is on the left side of the device. The Flipper's IR receiver (TSOP) is on top, IR LED is also on top.

## Psion IrDA details

- The MC218 IrDA port operates in SIR (Serial Infrared) mode
- TTY:B is the serial device name for the IR port in EPOC OPL
- 4800 baud is used for receiving (configured via IOW function 7)
- IrDA connect/disconnect attempts (`IrDAConnectToSend&:`) generate IR bursts detectable by the Flipper's TSOP receiver
- After IrDA TX, `IrDADisconnect:` must be called to release TTY:B for RX
- TTY:B may be busy for several seconds after IrDA operations; retry with delays

## Flipper IR details

- InfraredWorker in raw mode (signal decoding disabled)
- Raw signal timeout: 150ms (`INFRARED_RAW_RX_TIMING_DELAY_US`)
- All Psion TX gaps must exceed 150ms for separate callback detection
- TX uses `infrared_send_raw_ext` with 36kHz carrier, 33% duty
- Max 1024 timings per `infrared_send_raw_ext` call (MAX_TIMINGS_AMOUNT)

## ESP32 details

- ESP32-S2-WROVER (official Flipper WiFi devboard)
- Arduino framework
- WiFi with WPA2, TLS to OpenRouter API
- `client.setInsecure()` — no certificate pinning (simplicity over security)
- HardwareSerial(1) on GPIO43/44 for Flipper communication
