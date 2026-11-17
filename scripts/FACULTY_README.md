# C3 BLE fleet provisioning — Faculty guide

Use this to prepare a batch of 30 ESP32-C3 Super Mini boards with **one firmware image** and distinct 4-character sticker IDs derived from factory MAC addresses.

## Directory layout

Put these files in a `scripts/` directory inside your **existing working** PlatformIO firmware project:

```text
g474-ble-serial/
  platformio.ini
  src/main.c
  src/CMakeLists.txt
  sdkconfig.defaults
  scripts/
    provision.py
    connect.py
    setup_mac.sh
    STUDENT_README.md
    firmware_changes.md
  fleet.csv          <- generated, keep/backup
```

Alternatively use a separate scripts directory and supply `--project /path/to/g474-ble-serial` to `provision.py`.

## One-time preparation

1. Apply `firmware_changes.md` to the existing functional NUS firmware.
2. Build once: `pio run`.
3. Install Mac Python dependencies from the script directory: `sh scripts/setup_mac.sh`.
4. Keep PlatformIO's monitor closed during provisioning (`Ctrl+C`).
5. Find USB port: `pio device list` (typically `/dev/cu.usbmodem101`).

## Provision each board

Plug in **one** ESP32-C3 over USB. Run from the project root:

```sh
./scripts/.venv/bin/python scripts/provision.py --port /dev/cu.usbmodem101
```

The tool builds if required, flashes the selected board, listens for `IDENTITY ID=XXXX MAC=YYYYYYYYYYYY`, independently computes the same ID from the MAC, checks fleet duplicates, and writes a row to `<project>/fleet.csv`.

When the result says `STICKER: XXXX`, print/write exactly those four characters on the board. Unplug it and repeat. If the script reports a **collision**, set that board aside; **do not** hand out two adapters with the same sticker ID.

### Common commands

```sh
# Do not reflash; read and register a board that already has the identity firmware
./scripts/.venv/bin/python scripts/provision.py --port /dev/cu.usbmodem101 --identify-only

# Specify an alternative inventory path
./scripts/.venv/bin/python scripts/provision.py --port /dev/cu.usbmodem101 --inventory /path/to/fleet.csv

# If using a standalone scripts folder rather than <project>/scripts
python /path/to/provision.py --project /path/to/g474-ble-serial --port /dev/cu.usbmodem101
```

The board may reconnect with a different USB device number after upload; if that happens, re-run the script using its new port, or start with `--identify-only`.

Do not run two provisioning processes against the **same CSV inventory** concurrently. The CSV is atomically replaced after writing, but this version does not implement multi-writer fleet-inventory locking.

## Verify first before flashing the entire batch

Test two boards separately and simultaneously with two Macs. Confirm each advertises its own `C3-XXXX` **advertised local name**, not merely the CoreBluetooth `device.name` (which can be stale). Verify each student's connection to `/tmp/c3-ble`, UART loopback, and 30 seconds of sequence-numbered logging.

## Distribution to students

Provide only `connect.py`, `setup_mac.sh`, and `STUDENT_README.md` (not `fleet.csv` and not the firmware sources). Each student installs once with `sh setup_mac.sh`; each session they run `./.venv/bin/python connect.py` and enter their sticker.

## Production cautions

- Keep `fleet.csv` backed up; it is the source of truth for duplicates and device assignment.
- Factory MAC-based ID is deterministic, but only **20 bits** are represented by the four characters; collisions are possible and rejected when seen.
- Every adapter runs the same raw NUS firmware, UART fixed to 115200 8N1.
- Bluetooth service has no pairing/authentication in this prototype; avoid sensitive data.
- The PTY is not a native `/dev/cu.*` device. Student plotters must support a manually supplied serial path.
