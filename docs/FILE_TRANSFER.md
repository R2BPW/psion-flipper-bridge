# File Transfer: Computer -> Psion via WiFi+IR

## Overview

Send files from a computer to the Psion MC218 wirelessly:

```
Computer                ESP32              Flipper          Psion MC218
   |                      |                  |                 |
   |-- TCP :8080 -------->|                  |                 |
   |   F:name:content     |-- UART R:F:.. -->|                 |
   |                      |                  |-- IR PW ------->|
   |                      |                  |                 |-- save D:\name
```

## Protocol

### Wire format

```
F:<filename>:<content>
```

- `F:` prefix identifies file transfer
- `<filename>` — target filename on Psion (e.g. `hello.opl`)
- `<content>` — file body, `|` encodes newline (`\n`)
- Max total length: ~450 bytes (IR TX limit minus protocol overhead)

### Example

OPL hello world:

```
F:hello.opl:PROC Main:|  PRINT "Hello World"|  GET|ENDP
```

Psion saves as `D:\hello.opl`:
```
PROC Main:
  PRINT "Hello World"
  GET
ENDP
```

## Components

### 1. ESP32 — TCP server (simple_bridge.ino)

- Listen on TCP port 8080 after WiFi connects
- Accept connection, read one line (up to `\n` or connection close)
- If line starts with `F:`, forward to Flipper UART as `R:<line>\n`
- Close connection
- No authentication (local network only)

### 2. Flipper — no changes

Existing `process_esp_response()` already handles `R:` from UART:
- Receives `R:F:hello.opl:content`
- Strips `R:` prefix → `F:hello.opl:content`
- Calls `send_to_psion()` which IR-transmits the text
- Psion must be listening (RECV mode) before computer sends

### 3. Psion — RECV command in psmacs.opl

When user types `RECV` at the prompt:
1. Close OPX (release port)
2. Open TTY:B @4800, wait for IR data (blocks until data arrives)
3. Decode pulse-width bytes (same as normal receive)
4. If decoded text starts with `F:`:
   - Extract filename: text between first and second `:`
   - Extract content: everything after second `:`
   - Replace all `|` with CHR$(13)+CHR$(10) (CRLF)
   - Save to `D:\<filename>`
   - Print confirmation
5. If no `F:` prefix, display as normal text
6. Reopen OPX

### 4. Computer — send script

Minimal: netcat one-liner:

```sh
echo 'F:hello.opl:PROC Main:|  PRINT "Hello"|ENDP' | nc <esp32-ip> 8080
```

Or Python helper for multi-line files:

```python
import socket, sys

def send_file(host, filename, content):
    # Replace newlines with |
    payload = "F:" + filename + ":" + content.replace("\n", "|")
    s = socket.socket()
    s.connect((host, 8080))
    s.send(payload.encode() + b"\n")
    s.close()
    print(f"Sent {len(payload)} bytes")

# Usage: python send.py 192.168.1.x hello.opl < hello.opl
host = sys.argv[1]
filename = sys.argv[2]
content = sys.stdin.read()
send_file(host, filename, content)
```

```sh
python send.py 192.168.1.42 hello.opl < hello.opl
```

## Timing / user flow

1. User runs `psmacs` on Psion
2. Types `RECV` at prompt — Psion starts listening
3. User runs send script on computer
4. ESP32 receives, forwards to Flipper, Flipper IR-transmits
5. Psion receives, saves file, shows confirmation
6. Psion returns to prompt

## Limits

- Max file size: ~450 bytes (single IR transmission)
- Character set: printable ASCII (0x20-0x7E) + `|` for newlines
- Filename: 8.3 format recommended for EPOC compatibility
- No binary files (ASCII text only)
- Psion must be in RECV mode before computer sends
- Local network only (no auth on TCP port)

## Future extensions

- Chunked transfer for files > 450 bytes
- Binary transfer (base64 or hex encoding)
- Bidirectional: Psion → Computer file transfer
- Directory listing from Psion
