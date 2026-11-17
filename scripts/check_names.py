
import asyncio
from bleak import BleakScanner

seen = set()

def on_device(device, adv):
    if not (
        device.name
        or adv.local_name
    ):
        return

    key = (
        device.address,
        device.name,
        adv.local_name
    )

    if key in seen:
        return

    seen.add(key)

    print(f"\nDevice UUID:    {device.address}")
    print(f"CoreBluetooth:  {device.name}")
    print(f"Advertised:     {adv.local_name}")

async def main():
    async with BleakScanner(
        detection_callback=on_device
    ):
        await asyncio.sleep(15)

asyncio.run(main())

