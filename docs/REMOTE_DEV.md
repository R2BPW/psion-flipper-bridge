# Remote Development: Claude Code → Psion via IR Bridge

## Vision

Claude Code on macOS has access to EPOC SDK, Wine, cross-compilers.
The Psion MC218 is reachable via IR bridge.
Claude Code writes OPL code, compiles it, deploys to Psion, runs it,
reads output, iterates. Autonomous development loop.

```
┌─────────────────────────────────────────────────┐
│ macOS (Claude Code)                             │
│                                                 │
│  1. Write OPL source                            │
│  2. Compile via Wine/OPLTRAN → .opo             │
│  3. Send .opo to Psion via bridge               │
│  4. Tell Psion to run it                        │
│  5. Read output back                            │
│  6. Analyze, fix, repeat                        │
│                                                 │
│  Tools: OPLTRAN.EXE, Wine, EPOC SDK, bash       │
└──────────────┬──────────────────────────────────┘
               │ TCP :8080
               v
┌──────────────────────┐
│ ESP32 (WiFi bridge)  │
└──────────┬───────────┘
           │ UART 115200
           v
┌──────────────────────┐
│ Flipper Zero (IR)    │
└──────────┬───────────┘
           │ IR 36kHz
           v
┌──────────────────────┐
│ Psion MC218          │
│  Agent program (A)   │
│  - receives commands  │
│  - deploys files     │
│  - runs programs     │
│  - captures output   │
│  - sends output back │
└──────────────────────┘
```

## Architecture

### Program A: Psion Agent (`agent.opl`)

Always-running program on Psion. Loop:

```
1. Send READY signal to Flipper (via OPX IR)
2. Close OPX, open TTY:B, wait for command from Flipper
3. Parse command, execute, collect result
4. Reopen OPX, send result back to Flipper
5. Goto 1
```

Commands the agent understands:

| Command | Format | Action |
|---------|--------|--------|
| FPUT | `FPUT:name.ext:content` | Save file to D:\ (`\|` = newline) |
| RUN | `RUN:name.opo` | Run .opo, capture screen output |
| FGET | `FGET:name.ext` | Read file from D:\, send back |
| LIST | `LIST` | List D:\ directory |
| PING | `PING` | Reply PONG (health check) |

### The problem: .opo is binary

OPLTRAN compiles .opl (text) → .opo (binary). The IR bridge sends ASCII
text (printable bytes 0x20-0x7E). Binary .opo files contain bytes 0x00-0xFF.

#### Solution: hex encoding

Encode .opo as hex string. 1 binary byte = 2 hex chars.
A 1KB .opo becomes 2KB hex — within the ~450 byte single-TX limit
only for tiny programs (~225 bytes .opo).

For larger programs: chunked transfer.

#### Solution: send .opl source, compile on Psion

EPOC has a built-in OPL translator (`OPLTRAN` system service).
The agent could:
1. Receive .opl source text (ASCII, no binary issues)
2. Save to D:\temp.opl
3. Use EPOC's `OplTranslate` API or shell to compile
4. Run the resulting .opo

But: EPOC's built-in OPL translator may not be accessible from OPL.
The `OPLTRAN` on Psion is the OPL editor's compile function.
From OPL, we can call `CMD$` to launch programs — we could launch
the OPL editor to compile. But this is fragile.

#### Recommended solution: compile on Mac, send hex-encoded .opo

```
Mac: write .opl → OPLTRAN.EXE (Wine) → .opo → hex encode → send
Psion: receive hex → decode to binary → save .opo → RUN
```

For small test programs (<200 bytes .opo = <400 hex chars), fits in
one IR transmission. For larger programs, use chunked transfer.

### Chunked transfer protocol

For files > 450 bytes:

```
FPUT:name.ext:CHUNK:1/5:hex_data_part1
FPUT:name.ext:CHUNK:2/5:hex_data_part2
...
FPUT:name.ext:CHUNK:5/5:hex_data_part5
```

Agent appends each chunk to the file. After last chunk, file is complete.
Each chunk is a separate IR transmission (~400 hex chars = ~200 binary bytes).

Mac side sends chunks with pauses between them (agent needs time to
receive, save, signal READY, and listen again).

### Capturing program output

The agent runs a program with `CMD$()` or similar. But OPL programs
write to their own screen — the agent can't capture stdout.

#### Solution: output convention

Program B writes its output to a known file:

```opl
REM Program B — writes output to D:\OUT.TXT
PROC Main:
  LOCAL result$(255)
  result$ = "Hello World"
  REM ... do work ...
  LOPEN "D:\OUT.TXT"
  LPRINT result$
  LCLOSE
ENDP
```

After Program B exits, the agent reads D:\OUT.TXT and sends it back.

#### Alternative: shared global variables

Agent declares GLOBAL variables. Program B (loaded as an OPO library)
writes to them. But this requires tight coupling.

#### Recommended: file-based output (D:\OUT.TXT)

Simple, universal, works with any program.

### ESP32 changes

Add TCP server (port 8080) with bidirectional support:

**Computer → Psion (downlink):**
- Computer sends command via TCP
- ESP32 forwards as `R:<command>\n` to Flipper UART
- Flipper IR-transmits to Psion

**Psion → Computer (uplink):**
- Psion sends via OPX IR to Flipper
- Flipper sends `Q:<text>\n` to ESP32 UART
- ESP32 forwards to connected TCP client

The TCP connection stays open during a session.
ESP32 bridges: TCP socket ←→ Flipper UART, bidirectionally.

### Full development cycle

```
Claude Code:
  1. Writes hello.opl
  2. unix2dos + wine OPLTRAN → hello.opo (binary)
  3. Hex-encodes hello.opo → "4F504C..."
  4. Sends via TCP: FPUT:hello.opo:<hex>
  5. Sends via TCP: RUN:hello.opo
  6. Waits for response (agent runs program, reads D:\OUT.TXT)
  7. Receives output text
  8. Analyzes: did it work? errors?
  9. If error → fix .opl, goto 2
  10. If success → done
```

Time per iteration:
- Compile: ~1 sec (Wine OPLTRAN)
- Send .opo (~500 bytes hex): ~60 sec at current IR speed
- Run program: ~1-5 sec
- Receive output (~100 bytes): ~5 sec via OPX
- Total: ~70 sec per iteration

### Psion Agent state machine

```
          ┌──────────────┐
          │  IDLE/READY   │
          │  (OPX open)  │
          └──────┬───────┘
                 │ Send "READY" via IR
                 │ Close OPX
                 │ Open TTY:B
                 v
          ┌──────────────┐
          │  LISTENING    │
          │  (TTY:B open) │◄─────────────────┐
          └──────┬───────┘                   │
                 │ Receive command            │
                 v                            │
          ┌──────────────┐                   │
          │  EXECUTE      │                   │
          │  Close TTY:B  │                   │
          │  Run command  │                   │
          └──────┬───────┘                   │
                 │ Collect result             │
                 │ Open OPX                   │
                 │ Send result via IR         │
                 │ Close OPX                  │
                 └───────────────────────────┘
```

### Security considerations

- TCP port 8080 has no authentication (local network only)
- Agent executes arbitrary commands from IR — physical proximity required
- No code signing on Psion — any .opo runs with full privileges

### Implementation progress

**Done:**
1. ESP32 TCP bridge (WiFiServer :8080, bidirectional, LLM disabled in dev mode)
2. Psion agent (PING + FPUT + RUN via LOADM)
3. Mac send script (tools/psion_send.py)
4. PING round trip verified end-to-end
5. FPUT binary file writing verified (correct OPO icon)
6. LOADM + Task:(addr&) + POKE$ verified in emulator

**Remaining:**
7. Full FPUT + RUN test on real device
8. Chunked transfer for files > 450 bytes
9. Claude Code integration (bash tool wrappers)

### Hard-won knowledge

**OPL reserved words (cause compile errors):**
cmd$, hex$, menu — cannot use as variable or label names

**LOCAL declarations:** must be at TOP of PROC, before any code

**TRAP:** only works before keywords (DELETE, LOADM), NOT before proc calls

**GLOBAL variables:** NOT shared across LOADM boundaries. Use POKE$/PEEK$
with passed address for inter-module communication.

**Binary file writing:**
- IOOPEN(fh%, name$, 2) = create/replace binary stream
- IOW(fh%, 2, #buf&, len%) = write raw bytes (MUST use # before buf&)
- LOPEN/LPRINT creates TEXT files — wrong for .opo binary

**LOADM module convention:**
- Module has PROC Task:(buf&), no Main:
- buf& = ADDR(caller_string$) passed by caller
- Module writes result via POKE$ buf&, result$

**Agent IR quirks:**
- No READY signal (Flipper forwards to ESP32 as LLM query)
- Reply needs PAUSE 5 before IrSendMsg (port warmup)
- Send reply TWICE (Flipper may miss first)
- No IrFlash before IrClose (confuses Flipper decoder)
- OPX PETRAN UIDs: -uid1 0x10000079 -uid2 0x1000005d -uid3 0x101F9B01
