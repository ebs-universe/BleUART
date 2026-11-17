#!/usr/bin/env python3
"""ESP32 GPIO TX↔RX jumper loopback test via the /tmp/c3-ble virtual port."""
import time
import serial

PORT = "/tmp/c3-ble"
PAYLOAD = bytes(range(256))

with serial.Serial(PORT, 115200, timeout=8, write_timeout=8) as port:
    time.sleep(0.4)
    port.reset_input_buffer()
    port.timeout = 0.05
    idle_since = time.monotonic()
    deadline = idle_since + 3
    discarded = 0
    while time.monotonic() < deadline:
        data = port.read(port.in_waiting or 1)
        if data:
            discarded += len(data)
            idle_since = time.monotonic()
        elif time.monotonic() - idle_since >= 0.3:
            break
    port.timeout = 8
    port.write(PAYLOAD)
    port.flush()
    response = port.read(len(PAYLOAD))
    print(f"Drained: {discarded} stale bytes")
    print(f"Sent: {len(PAYLOAD)}; received: {len(response)}")
    print(f"Match: {response == PAYLOAD}")
