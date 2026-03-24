#!/usr/bin/env python3
"""
Upload a file to Flipper Zero SD card via CLI serial port.

Usage:
    1. Exit any running FAP on Flipper (Back button)
    2. Run: python3 flipper_upload.py ir_bridge.fap /ext/apps/Infrared/ir_bridge.fap

The Flipper must be in normal mode (not running a custom USB CDC app).
"""

import serial
import sys
import time

SERIAL_PORT = "/dev/ttyACM0"
CHUNK_SIZE = 512


def wait_prompt(ser, timeout=3):
    """Read until we see '>:' prompt or timeout."""
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        if ser.in_waiting:
            buf += ser.read(ser.in_waiting)
            if b">:" in buf:
                return buf.decode(errors="replace")
        time.sleep(0.05)
    return buf.decode(errors="replace")


def upload(port, local_path, remote_path):
    with open(local_path, "rb") as f:
        data = f.read()

    size = len(data)
    print(f"File: {local_path} ({size} bytes)")
    print(f"Dest: {remote_path}")
    print(f"Port: {port}")

    ser = serial.Serial(port, 115200, timeout=2)
    time.sleep(0.3)

    # Clear buffer, send empty line to get prompt
    ser.read(ser.in_waiting)
    ser.write(b"\r\n")
    resp = wait_prompt(ser)

    if ">:" not in resp:
        print(f"WARNING: No Flipper CLI prompt detected.")
        print(f"  Got: {resp[:100]}")
        print(f"  Is the Flipper in normal mode (no FAP running)?")
        ser.close()
        return False

    print("Flipper CLI detected.")

    # Ensure target directory exists
    dir_path = "/".join(remote_path.split("/")[:-1])
    ser.write(f"storage mkdir {dir_path}\r\n".encode())
    time.sleep(0.3)
    ser.read(ser.in_waiting)

    # Remove existing file (ignore error if not exists)
    ser.write(f"storage remove {remote_path}\r\n".encode())
    time.sleep(0.3)
    ser.read(ser.in_waiting)

    # Write file using storage write with explicit size
    cmd = f"storage write_chunk {remote_path} {size}\r\n"
    print(f"Sending: {cmd.strip()}")
    ser.write(cmd.encode())
    time.sleep(0.3)

    resp = ser.read(ser.in_waiting).decode(errors="replace")
    if "error" in resp.lower() or "usage" in resp.lower():
        # write_chunk might not be available, fall back to raw write
        print("write_chunk not available, trying raw write...")
        ser.write(f"storage write {remote_path}\r\n".encode())
        time.sleep(0.3)
        ser.read(ser.in_waiting)

        # Send data
        sent = 0
        while sent < size:
            chunk = data[sent:sent + CHUNK_SIZE]
            ser.write(chunk)
            sent += len(chunk)
            pct = sent * 100 // size
            print(f"\r  Uploading: {pct}% ({sent}/{size})", end="", flush=True)
            time.sleep(0.01)

        # End with Ctrl+C
        time.sleep(0.2)
        ser.write(b"\x03")
    else:
        # write_chunk: send data
        sent = 0
        while sent < size:
            chunk = data[sent:sent + CHUNK_SIZE]
            ser.write(chunk)
            sent += len(chunk)
            pct = sent * 100 // size
            print(f"\r  Uploading: {pct}% ({sent}/{size})", end="", flush=True)
            time.sleep(0.01)

    print()
    time.sleep(0.5)
    resp = wait_prompt(ser)
    print(f"Response: {resp.strip()}")

    # Verify file exists
    ser.write(f"storage stat {remote_path}\r\n".encode())
    time.sleep(0.5)
    resp = wait_prompt(ser)
    if "File" in resp or str(size) in resp:
        print(f"OK: {remote_path} uploaded successfully.")
    else:
        print(f"Verify: {resp.strip()}")

    ser.close()
    return True


def main():
    if len(sys.argv) < 3:
        print("Usage: python3 flipper_upload.py <local_file> <flipper_path>")
        print()
        print("Examples:")
        print("  python3 flipper_upload.py ir_bridge.fap /ext/apps/Infrared/ir_bridge.fap")
        print("  python3 flipper_upload.py sirtest.opo /ext/apps/sirtest.opo")
        return

    local = sys.argv[1]
    remote = sys.argv[2]
    port = sys.argv[3] if len(sys.argv) > 3 else SERIAL_PORT

    upload(port, local, remote)


if __name__ == "__main__":
    main()
