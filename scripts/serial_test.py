
import serial
import time

PORT = "/tmp/g474-ble"
PAYLOAD = bytes(range(256))


def drain_rx(ser, quiet_time=0.3, max_wait=3.0):
    """Discard incoming bytes until RX is quiet."""

    ser.reset_input_buffer()

    original_timeout = ser.timeout
    ser.timeout = 0.05

    discarded = 0
    start = time.monotonic()
    last_rx = start

    while time.monotonic() - start < max_wait:
        data = ser.read(ser.in_waiting or 1)

        if data:
            discarded += len(data)
            last_rx = time.monotonic()
        elif time.monotonic() - last_rx >= quiet_time:
            break

    ser.timeout = original_timeout
    return discarded


with serial.Serial(
    PORT,
    baudrate=115200,
    timeout=5,
    write_timeout=5,
) as ser:

    # Allow any pending BLE data to arrive
    time.sleep(0.5)

    discarded = drain_rx(ser)
    print(f"Drained {discarded} stale RX bytes")

    # Transmit test sequence
    ser.write(PAYLOAD)
    ser.flush()

    # Receive echoed bytes
    received = ser.read(len(PAYLOAD))

    print(f"Sent:     {len(PAYLOAD)} bytes")
    print(f"Received: {len(received)} bytes")
    print(f"Match:    {received == PAYLOAD}")

    if received != PAYLOAD:
        print("Sent:    ", PAYLOAD.hex(" "))
        print("Received:", received.hex(" "))

