#!/usr/bin/env python3
"""
RPi Bridge for Psion-Flipper-Bridge.
Replaces ESP32 WiFi devboard. Connects to Flipper Zero via USB CDC.

Usage:
    pip install pyserial
    export OPENROUTER_API_KEY="sk-or-v1-..."
    python3 bridge.py

Environment variables:
    SERIAL_PORT      - serial device (default: /dev/ttyACM0)
    LLM_BACKEND      - "openrouter" or "ollama" (default: openrouter)
    OPENROUTER_API_KEY - API key for OpenRouter
    LLM_MODEL        - model name (default: google/gemini-2.0-flash-001)
    OLLAMA_URL       - Ollama base URL (default: http://localhost:11434)
    OLLAMA_MODEL     - Ollama model (default: llama3.2)
    MAX_RESPONSE     - max response chars sent to Flipper (default: 200)
"""

import json
import os
import stat
import sys
import time
import urllib.request
import urllib.error

import serial

SERIAL_PORT = os.environ.get("SERIAL_PORT", "/dev/ttyACM0")
BAUD_RATE = 115200

LLM_BACKEND = os.environ.get("LLM_BACKEND", "openrouter")
OPENROUTER_API_KEY = os.environ.get("OPENROUTER_API_KEY", "")
LLM_MODEL = os.environ.get("LLM_MODEL", "google/gemini-2.0-flash-001")
MAX_TOKENS = 150
MAX_RESPONSE = int(os.environ.get("MAX_RESPONSE", "200"))

OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://localhost:11434")
OLLAMA_MODEL = os.environ.get("OLLAMA_MODEL", "llama3.2")

SYSTEM_PROMPT = (
    "You are a helpful assistant responding to queries from a 1999 Ericsson MC218 "
    "palmtop via infrared link. Keep responses concise (under 180 characters). "
    "Use plain ASCII only, no markdown or special characters."
)


FS_ROOT = os.environ.get("FS_ROOT", os.path.expanduser("~"))
ITEMS_PER_PAGE = 6


class FileBrowser:
    """Stateful file browser for MC218. Navigated via letter indices."""

    def __init__(self, root):
        self.root = os.path.realpath(root)
        self.cwd = self.root
        self.listing = []  # cached sorted entries for current dir

    def _refresh(self):
        """Reload directory listing."""
        try:
            entries = sorted(os.listdir(self.cwd))
        except PermissionError:
            entries = []
        self.listing = entries

    def _short_path(self):
        """Abbreviated current path for display."""
        path = self.cwd
        home = os.path.expanduser("~")
        if path.startswith(home):
            path = "~" + path[len(home):]
        if len(path) > 30:
            path = "..." + path[-27:]
        return path

    def _format_entry(self, idx, name):
        """Format one entry: 'A name/' or 'A name 1K'."""
        letter = chr(65 + idx)  # A, B, C...
        path = os.path.join(self.cwd, name)
        try:
            st = os.stat(path)
        except OSError:
            return f"{letter} {name}?"

        if stat.S_ISDIR(st.st_mode):
            return f"{letter} {name}/"
        else:
            size = st.st_size
            if size < 1024:
                sz = f"{size}B"
            elif size < 1024 * 1024:
                sz = f"{size // 1024}K"
            else:
                sz = f"{size // (1024 * 1024)}M"
            return f"{letter} {name} {sz}"

    def ls(self, page=0):
        """List current directory. Returns response string."""
        self._refresh()
        if not self.listing:
            return f"{self._short_path()}: (empty)"

        start = page * ITEMS_PER_PAGE
        if start >= len(self.listing):
            return f"{self._short_path()}: no page {page + 1}"

        end = min(start + ITEMS_PER_PAGE, len(self.listing))
        parts = [self._short_path() + ":"]
        for i, name in enumerate(self.listing[start:end]):
            parts.append(self._format_entry(i, name))

        remaining = len(self.listing) - end
        if remaining > 0:
            parts.append(f"[+{remaining}]")

        result = " ".join(parts)
        if len(result) > MAX_RESPONSE:
            result = result[:MAX_RESPONSE - 3] + "..."
        return result

    def cd(self, letter):
        """Enter directory by letter index. Returns new listing."""
        idx = ord(letter.upper()) - 65
        if idx < 0 or idx >= min(len(self.listing), ITEMS_PER_PAGE):
            return "E:BAD INDEX"

        name = self.listing[idx]
        path = os.path.realpath(os.path.join(self.cwd, name))
        if not os.path.isdir(path):
            return f"NOT A DIR: {name}"
        self.cwd = path
        return self.ls()

    def up(self):
        """Go to parent directory. Returns new listing."""
        parent = os.path.dirname(self.cwd)
        if os.path.realpath(parent) == os.path.realpath(self.cwd):
            return "ALREADY AT ROOT"
        self.cwd = parent
        return self.ls()

    def cat(self, letter, page=0):
        """Read file content by letter index."""
        idx = ord(letter.upper()) - 65
        if idx < 0 or idx >= min(len(self.listing), ITEMS_PER_PAGE):
            return "E:BAD INDEX"

        name = self.listing[idx]
        path = os.path.join(self.cwd, name)
        if os.path.isdir(path):
            return f"IS A DIR: {name}"

        try:
            offset = page * (MAX_RESPONSE - 20)
            with open(path, "r", errors="replace") as f:
                f.seek(offset)
                content = f.read(MAX_RESPONSE - 20)
            if not content:
                return f"{name}: (end)"
            content = content.replace("\n", " ").replace("\r", "")
            has_more = len(content) == MAX_RESPONSE - 20
            prefix = f"{name}: " if page == 0 else f"{name} p{page + 1}: "
            result = prefix + content
            if has_more:
                result = result[:MAX_RESPONSE - 5] + " [...]"
            return result[:MAX_RESPONSE]
        except Exception as e:
            return f"E:{str(e)[:60]}"

    def pwd(self):
        return self.cwd

    def home(self):
        """Return to root directory."""
        self.cwd = self.root
        return self.ls()

    def info(self, letter):
        """Show file/dir info by letter index."""
        idx = ord(letter.upper()) - 65
        if idx < 0 or idx >= min(len(self.listing), ITEMS_PER_PAGE):
            return "E:BAD INDEX"

        name = self.listing[idx]
        path = os.path.join(self.cwd, name)
        try:
            st = os.stat(path)
            kind = "DIR" if stat.S_ISDIR(st.st_mode) else "FILE"
            size = st.st_size
            mtime = time.strftime("%Y-%m-%d %H:%M", time.localtime(st.st_mtime))
            perms = oct(st.st_mode)[-3:]
            return f"{name}: {kind} {size}B {mtime} {perms}"
        except OSError as e:
            return f"E:{e}"


# Global file browser instance (session state)
browser = None


def get_browser():
    global browser
    if browser is None:
        browser = FileBrowser(FS_ROOT)
    return browser


def handle_fs_command(text):
    """Try to handle text as a file system command.
    Returns response string, or None if not a FS command."""
    parts = text.strip().split()
    if not parts:
        return None

    cmd = parts[0].upper()
    arg = parts[1].upper() if len(parts) > 1 else ""
    fb = get_browser()

    if cmd == "LS":
        page = ord(arg) - 65 if arg and arg.isalpha() else 0
        return fb.ls(page)
    elif cmd == "CD" and arg:
        return fb.cd(arg)
    elif cmd == "UP":
        return fb.up()
    elif cmd == "CAT" and arg:
        page = 0
        if len(parts) > 2 and parts[2].isalpha():
            page = ord(parts[2].upper()) - 65
        return fb.cat(arg, page)
    elif cmd == "PWD":
        return fb.pwd()
    elif cmd == "HOME":
        return fb.home()
    elif cmd == "INFO" and arg:
        return fb.info(arg)

    return None


def query_openrouter(text):
    """Query LLM via OpenRouter API."""
    if not OPENROUTER_API_KEY:
        return "E:NO_API_KEY"

    body = json.dumps({
        "model": LLM_MODEL,
        "max_tokens": MAX_TOKENS,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": text},
        ],
    }).encode()

    req = urllib.request.Request(
        "https://openrouter.ai/api/v1/chat/completions",
        data=body,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {OPENROUTER_API_KEY}",
        },
    )

    with urllib.request.urlopen(req, timeout=30) as resp:
        data = json.loads(resp.read().decode())

    content = data["choices"][0]["message"]["content"]
    # Strip markdown artifacts
    content = content.replace("**", "").replace("`", "'")
    return content


def query_ollama(text):
    """Query local Ollama instance."""
    body = json.dumps({
        "model": OLLAMA_MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": text},
        ],
        "stream": False,
        "options": {"num_predict": MAX_TOKENS},
    }).encode()

    req = urllib.request.Request(
        f"{OLLAMA_URL}/api/chat",
        data=body,
        headers={"Content-Type": "application/json"},
    )

    with urllib.request.urlopen(req, timeout=60) as resp:
        data = json.loads(resp.read().decode())

    return data["message"]["content"]


def query_llm(text):
    if LLM_BACKEND == "ollama":
        return query_ollama(text)
    return query_openrouter(text)


def send(ser, line):
    """Send a line to Flipper via CDC."""
    msg = (line + "\n").encode()
    ser.write(msg)
    ser.flush()
    print(f"  > {line}")


def handle_query(ser, text):
    """Handle Q: LLM query."""
    send(ser, "S:ASKING_LLM")
    try:
        response = query_llm(text)
        response = response.replace("\n", " ").replace("\r", "")
        if len(response) > MAX_RESPONSE:
            response = response[:MAX_RESPONSE]
        send(ser, f"R:{response}")
    except Exception as e:
        err = str(e)[:60].replace("\n", " ")
        print(f"  ! LLM error: {e}")
        send(ser, f"E:{err}")


def handle_opcode(ser, opcode_hex, params):
    """Handle OP:HH:params RPC opcode."""
    # For now, forward all opcodes to LLM with context
    try:
        opcode = int(opcode_hex, 16)
    except ValueError:
        send(ser, "E:BAD_OPCODE")
        return

    # Opcode dispatch table (extend as needed)
    if opcode == 0x80:  # GEN_LISP (0x10 + 0x70)
        handle_query(ser, f"Generate a Lisp expression for: {params}")
    elif opcode == 0x81:  # EXPL_ERR
        handle_query(ser, f"Explain this error concisely: {params}")
    elif opcode == 0x82:  # COMPL_COD
        handle_query(ser, f"Complete this code: {params}")
    elif opcode == 0x83:  # SUMM_TXT
        handle_query(ser, f"Summarize in one sentence: {params}")
    elif opcode == 0x84:  # TRANSLATE
        handle_query(ser, f"Translate to English: {params}")
    elif opcode == 0x85:  # REWRITE
        handle_query(ser, f"Rewrite more clearly: {params}")
    elif opcode == 0x86:  # ORG_DECOMP
        handle_query(ser, f"Break down into steps: {params}")
    else:
        send(ser, f"E:UNKNOWN_OP:{opcode_hex}")


def handle_line(ser, line):
    """Dispatch a line received from Flipper."""
    if line.startswith("Q:"):
        text = line[2:]
        # Try file system command first
        fs_resp = handle_fs_command(text)
        if fs_resp is not None:
            send(ser, f"R:{fs_resp}")
            return
        handle_query(ser, text)
    elif line == "PING":
        send(ser, "S:PONG")
    elif line == "STATUS":
        send(ser, "S:RPI_OK")
    elif line.startswith("OP:"):
        # Format: OP:HH:params
        parts = line.split(":", 2)
        if len(parts) >= 3:
            handle_opcode(ser, parts[1], parts[2])
        elif len(parts) == 2:
            handle_opcode(ser, parts[1], "")
        else:
            send(ser, "E:BAD_OP_FORMAT")
    else:
        print(f"  ? Unknown: {line}")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else SERIAL_PORT
    print(f"RPi Bridge v1.1")
    print(f"  Port:    {port}")
    print(f"  Backend: {LLM_BACKEND}")
    if LLM_BACKEND == "ollama":
        print(f"  Model:   {OLLAMA_MODEL} @ {OLLAMA_URL}")
    else:
        print(f"  Model:   {LLM_MODEL}")
        if not OPENROUTER_API_KEY:
            print("  WARNING: OPENROUTER_API_KEY not set!")

    while True:
        try:
            print(f"\nConnecting to {port}...")
            ser = serial.Serial(port, BAUD_RATE, timeout=1)
            print("Connected.")
            time.sleep(0.5)
            send(ser, "S:RPI_OK")

            while True:
                raw = ser.readline()
                if not raw:
                    continue
                hexd = raw.hex()
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                print(f"  < {line}  [{hexd}]")
                handle_line(ser, line)

        except serial.SerialException as e:
            print(f"Serial error: {e}")
            print("Reconnecting in 3s...")
            time.sleep(3)
        except KeyboardInterrupt:
            print("\nExiting.")
            break


if __name__ == "__main__":
    main()
