# ESP32-C3 fleet identity — one-time firmware patch

Apply to the **working** PlatformIO + ESP-IDF NimBLE/Nordic UART Service firmware. Keep its UART pin assignments and BLE forwarding logic unchanged. All devices will use the **same compiled image**.

## 1. Headers

Add to `src/main.c`:

```c
#include <stdio.h>
#include <stdint.h>
#include "esp_mac.h"
```

## 2. Replace the static device name

Replace `#define DEVICE_NAME "G474-BLE"` with the following block **at file scope**, before any use of `DEVICE_NAME`:

```c
#define DEVICE_NAME device_name

static char device_name[8];   // "C3-ABCD" plus NUL
static char device_id[5];     // "ABCD" plus NUL
static uint8_t factory_mac[6];

static void init_identity(void)
{
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

    ESP_ERROR_CHECK(esp_efuse_mac_get_default(factory_mac));

    // 20 least significant bits of the 48-bit factory base MAC.
    uint32_t value = ((uint32_t)(factory_mac[3] & 0x0f) << 16)
                   | ((uint32_t)factory_mac[4] << 8)
                   | (uint32_t)factory_mac[5];

    for (int i = 3; i >= 0; --i) {
        device_id[i] = alphabet[value & 31u];
        value >>= 5;
    }
    device_id[4] = '\0';
    snprintf(device_name, sizeof(device_name), "C3-%s", device_id);
}
```

This matches `provision.py`'s `calc_id()` exactly.

## 3. Call identity initialization before BLE setup

At the **very beginning** of `app_main()`:

```c
void app_main(void)
{
    init_identity();
    // ... keep existing NVS/UART/NimBLE initialization below ...
```

The existing code must continue to set the GAP device name with:

```c
ble_svc_gap_device_name_set(DEVICE_NAME);
```

In `start_advertising()` it must continue to use:

```c
fields.name = (uint8_t *)DEVICE_NAME;
fields.name_len = strlen(DEVICE_NAME);
fields.name_is_complete = 1;
```

Do **not** replace these with the old literal `"G474-BLE"`.

## 4. Emit a parseable identity for the faculty provisioning tool

Keep the existing heartbeat loop at the end of `app_main()`, but replace its `ESP_LOGI()` call with:

```c
ESP_LOGI(TAG,
    "IDENTITY ID=%s MAC=%02X%02X%02X%02X%02X%02X | BLE=%s | Notify=%s | Dropped=%lu",
    device_id,
    factory_mac[0], factory_mac[1], factory_mac[2],
    factory_mac[3], factory_mac[4], factory_mac[5],
    connection != BLE_HS_CONN_HANDLE_NONE ? "connected" : "disconnected",
    notifications_enabled ? "yes" : "no",
    (unsigned long)dropped_bytes);
```

Keep the existing `vTaskDelay(pdMS_TO_TICKS(2000));` in the loop. The script waits for this message after USB reconnects following flashing.

The prefix `G474-BLE:` in an ESP-IDF log is only a **logging tag**, not the advertised BLE name. There is no need to rename the tag.

## 5. Ensure CMake includes the MAC helper dependency

In `src/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_driver_uart esp_hw_support
)
```

## 6. Verify one device before production flashing

```sh
pio run
pio run -t upload
pio device monitor -b 115200
```

Look for `IDENTITY ID=XXXX MAC=XXXXXXXXXXXX` and `Advertising: C3-XXXX` on the ESP32 USB log. In a separate terminal, check the **advertised** local name using nRF Connect or a Bleak advertisement scan. On macOS, `ble-scan` may display the older cached CoreBluetooth name. That does *not* mean the ESP32 is still advertising the old name.

## Limits / policy

- Four Base32 characters encode 20 bits, so uniqueness is **not globally guaranteed**. `provision.py` detects duplicates against the current `fleet.csv` and refuses a collision; set such a unit aside or implement an explicit ID override.
- The MAC is factory-programmed and persistent across firmware flashes.
- The code does not alter BLE MAC addresses or bond/pair devices; identifiers are for **convenient discovery**, not authentication.
- Maintain the inventory CSV; do not delete it while provisioning more boards.
- The standard legacy BLE advertising packet fits flags (3 bytes) + NUS UUID (18 bytes) + `C3-XXXX` name (9 bytes) = 30 of 31 bytes.
