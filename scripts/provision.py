#!/usr/bin/env python3
"""Flash and register one ESP32-C3, or identify an existing board without flashing.

Requires PlatformIO CLI and pyserial. Run with no other serial monitor on the USB port.
Generated inventory: fleet.csv; never assigns the MAC-derived sticker ID manually.
"""

import argparse
import csv
from datetime import datetime, timezone
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

try:
    import serial
    from serial import SerialException
except ImportError:
    sys.exit("Missing pyserial. Run: sh setup_mac.sh")

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
IDENTITY = re.compile(r"\bIDENTITY ID=([0-9A-HJKMNP-TV-Z]{4}) MAC=([0-9A-F]{12})\b")
FIELDS = ("id", "advertised_name", "factory_mac", "flashed_utc", "usb_port")


def calc_id(mac: str) -> str:
    value = int(mac, 16) & 0xFFFFF
    return "".join(ALPHABET[(value >> shift) & 31] for shift in (15, 10, 5, 0))


def locate_platformio():
    found = shutil.which("pio")
    if found:
        return found
    candidate = Path.home() / ".platformio/penv/bin/pio"
    if candidate.is_file():
        return str(candidate)
    raise RuntimeError("PlatformIO CLI not found (run from VS Code PlatformIO terminal)")


def read_identity(port: str, timeout: float):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            # Native USB reconnects following a flash can temporarily invalidate the port.
            with serial.Serial(port, baudrate=115200, timeout=0.5,
                               rtscts=False, dsrdtr=False) as ser:
                try:
                    ser.dtr = False
                    ser.rts = False
                except OSError:
                    pass
                buffer = bytearray()
                while time.monotonic() < end:
                    chunk = ser.read(512)
                    if not chunk:
                        continue
                    buffer.extend(chunk)
                    if len(buffer) > 8192:
                        del buffer[:-2048]
                    while b"\n" in buffer:
                        raw, _, remaining = buffer.partition(b"\n")
                        buffer = bytearray(remaining)
                        line = raw.decode("utf-8", errors="replace").strip()
                        match = IDENTITY.search(line)
                        if match:
                            print("Identity: " + line, flush=True)
                            return match.group(1), match.group(2)
        except (SerialException, OSError):
            time.sleep(0.5)
    raise TimeoutError(
        f"No IDENTITY message from {port} within {timeout:g}s. "
        "Close other monitors, confirm USB console, and check heartbeat logs."
    )


def register(inventory: Path, ident: str, mac: str, port: str):
    rows = []
    if inventory.exists():
        with inventory.open(newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            if reader.fieldnames != list(FIELDS):
                raise RuntimeError(f"Unexpected CSV format in {inventory}; preserve it and inspect manually")
            rows = list(reader)
    for row in rows:
        if row["id"] == ident and row["factory_mac"] != mac:
            raise RuntimeError(
                f"COLLISION: ID {ident} is already assigned to MAC {row['factory_mac']}, "
                f"but this chip is {mac}. Set this unit aside; do not label/distribute."
            )
        if row["factory_mac"] == mac:
            if row["id"] != ident:
                raise RuntimeError(f"MAC {mac} previously registered with ID {row['id']}; investigate")
            print(f"Previously registered {ident}; inventory unchanged.")
            return len(rows)

    rows.append({
        "id": ident,
        "advertised_name": f"C3-{ident}",
        "factory_mac": mac,
        "flashed_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "usb_port": port,
    })
    inventory.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(prefix=".fleet-", suffix=".csv", dir=inventory.parent)
    try:
        with os.fdopen(fd, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=FIELDS)
            writer.writeheader()
            writer.writerows(rows)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_name, inventory)
    finally:
        if os.path.exists(tmp_name):
            os.unlink(tmp_name)
    print(f"Inventory updated: {inventory}")
    return len(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True, help="ESP USB port e.g. /dev/cu.usbmodem101")
    ap.add_argument("--project", type=Path, default=Path(__file__).resolve().parent.parent,
                    help="PlatformIO project root (default: directory above scripts/)")
    ap.add_argument("--inventory", type=Path, default=None,
                    help="CSV inventory path (default: <project>/fleet.csv)")
    ap.add_argument("--timeout", type=float, default=35)
    ap.add_argument("--identify-only", action="store_true", help="do not reflash")
    args = ap.parse_args()
    if args.timeout <= 0:
        ap.error("--timeout must be positive")
    project = args.project.expanduser().resolve()
    inventory = (args.inventory or project / "fleet.csv").expanduser().resolve()
    if not args.identify_only:
        if not (project / "platformio.ini").exists():
            raise RuntimeError(f"Not a PlatformIO project: {project}")
        cmd = [locate_platformio(), "run", "-d", str(project), "-t", "upload",
               "--upload-port", args.port]
        print("Flashing: " + " ".join(cmd), flush=True)
        subprocess.run(cmd, check=True)
    print(f"Reading factory identity from {args.port}...", flush=True)
    ident, mac = read_identity(args.port, args.timeout)
    expected = calc_id(mac)
    if ident != expected:
        raise RuntimeError(f"Firmware ID {ident} doesn't match MAC-derived ID {expected}")
    count = register(inventory, ident, mac, args.port)
    print(f"\nSTICKER: {ident}     BLE NAME: C3-{ident}     REGISTERED: {count} board(s)\n")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, TimeoutError, subprocess.CalledProcessError, OSError) as exc:
        sys.exit(f"ERROR: {exc}")
