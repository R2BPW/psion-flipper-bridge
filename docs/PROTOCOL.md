# IR Protocol Specification

## Psion -> Flipper: 5-bit PWM encoding

### Frame structure

Each character = 7 IrDA connect bursts:

```
[Start flash] --(start_space)--> [Bit4 flash] --(b4_space)--> [Bit3 flash] --> ... --> [Parity flash] --(parity_space)--> [next char's Start flash or End marker]
```

End of message = one extra flash + 2s silence.

### Timing

The Psion calls `IrDAConnectToSend&:(KIrTinyTP$, 1)` which generates one IR burst. Each burst has ~955ms overhead (IrDA stack connect/disconnect attempt). The space between bursts encodes data:

| Space type | OPL code | OPL duration | Total gap (OPL + overhead) |
|------------|----------|-------------|---------------------------|
| Start space | `PAUSE 20` | 1000ms | ~1955ms |
| Bit 0 | `PAUSE 5` | 250ms | ~1205ms |
| Bit 1 | `PAUSE 11` | 550ms | ~1505ms |

### Character encoding

- 5 data bits, MSB first: A=0 (`00000`), B=1 (`00001`), ... Z=25 (`11001`), space=26 (`11010`)
- 6th bit: even parity = XOR of 5 data bits
- Total: 7 bursts per character (start + 5 data + parity)

### Flipper decoder thresholds

```
gap < 500ms          -> noise (ignored)
500ms <= gap < 1350ms -> bit 0
1350ms <= gap < 1730ms -> bit 1
gap >= 1730ms         -> start space (new character)
gap >= 4000ms         -> message timeout (dispatch)
```

### State machine

```
PwmIdle ──[any burst]──> PwmWaitConfirm
PwmWaitConfirm ──[gap >= START_MIN]──> PwmData (start confirmed)
PwmWaitConfirm ──[gap < START_MIN]──> PwmIdle (bad start)
PwmData ──[gap < BIT1_MIN]──> decode bit 0, bit_count++
PwmData ──[BIT1_MIN <= gap < START_MIN]──> decode bit 1, bit_count++
PwmData ──[bit_count == 5]──> verify parity, emit char, -> PwmWaitConfirm
PwmData ──[gap >= START_MIN]──> framing error, -> PwmWaitConfirm
```

## Flipper -> Psion: pulse-width byte encoding

### IR parameters

- Carrier: 36 kHz, 33% duty cycle
- Mark durations encode data; spaces are fixed 5ms

### Byte encoding

Each byte is 8 mark/space pairs (MSB first):

| Mark type | Duration |
|-----------|----------|
| Start mark | 50ms (50000 us) |
| Bit 0 mark | 10ms (10000 us) |
| Bit 1 mark | 26ms (26000 us) |
| End mark | 75ms (75000 us) |
| Space (all) | 5ms (5000 us) |

### Frame structure

```
[START mark + space] [byte0: 8x mark+space] [byte1: 8x mark+space] ... [EOT byte: 0x04] [END mark + space]
```

Data is sent in chunks of 63 bytes via `infrared_send_raw_ext` (limit: MAX_TIMINGS_AMOUNT = 1024). Back-to-back calls with ~100us gap (invisible to SIR decoder).

### Psion SIR decoding

The Psion opens `TTY:B` at 4800 baud and reads bytes. The SIR hardware interprets pulse-width marks as:

- Byte value >= `$E0` -> bit 1
- Byte value <= `$90` -> bit 0
- Byte value `$90` < v < `$E0` -> marker (start/end)

8 bits = 1 decoded byte. If decoded byte = `0x04` (EOT), message is complete.

## Flipper <-> ESP32: UART protocol

115200 baud, 8N1, newline-terminated ASCII strings.

### Commands (Flipper -> ESP32)

| Prefix | Example | Meaning |
|--------|---------|---------|
| `Q:` | `Q:WHAT IS LISP\n` | Generic LLM query |
| `OP:` | `OP:80:describe a loop\n` | RPC opcode + params |
| `PING` | `PING\n` | Heartbeat check |
| `STATUS` | `STATUS\n` | Request status |

### Responses (ESP32 -> Flipper)

| Prefix | Example | Meaning |
|--------|---------|---------|
| `R:` | `R:Lisp is a language...\n` | LLM response text |
| `S:` | `S:WIFI_OK\n` | Status update |
| `S:TOKENS:` | `S:TOKENS:150\n` | Token usage |
| `E:` | `E:NO_WIFI\n` | Error |
