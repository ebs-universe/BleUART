
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "esp_mac.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "driver/uart.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_att.h"
#include "host/ble_uuid.h"
#include "host/ble_hs_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define DEVICE_NAME       device_name

#define UART_PORT         UART_NUM_1
#define UART_TX_PIN       4
#define UART_RX_PIN       3
#define UART_BAUD         115200

#define STREAM_SIZE       8192
#define MAX_BLE_CHUNK     200

static const char *TAG = "G474-BLE";

static StreamBufferHandle_t uart_to_ble;

static volatile uint16_t connection =
    BLE_HS_CONN_HANDLE_NONE;

static volatile bool notifications_enabled = false;
static volatile uint32_t dropped_bytes = 0;

static uint8_t own_addr_type;
static uint16_t nus_tx_handle;

static char device_name[8];
static char device_id[5];
static uint8_t factory_mac[6];

static void init_identity(void)
{
    static const char alphabet[] =
        "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

    ESP_ERROR_CHECK(
        esp_efuse_mac_get_default(factory_mac));

    uint32_t value =
        ((uint32_t)(factory_mac[3] & 0x0F) << 16) |
        ((uint32_t)factory_mac[4] << 8) |
        factory_mac[5];

    for (int i = 3; i >= 0; i--) {
        device_id[i] = alphabet[value & 31];
        value >>= 5;
    }

    device_id[4] = '\0';

    snprintf(
        device_name,
        sizeof(device_name),
        "C3-%s",
        device_id);
}

/*
 * Nordic UART Service UUIDs.
 * BLE_UUID128_INIT uses little-endian byte order.
 */

static const ble_uuid128_t nus_service_uuid =
    BLE_UUID128_INIT(
        0x9e, 0xca, 0xdc, 0x24,
        0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5,
        0x01, 0x00, 0x40, 0x6e);

static const ble_uuid128_t nus_rx_uuid =
    BLE_UUID128_INIT(
        0x9e, 0xca, 0xdc, 0x24,
        0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5,
        0x02, 0x00, 0x40, 0x6e);

static const ble_uuid128_t nus_tx_uuid =
    BLE_UUID128_INIT(
        0x9e, 0xca, 0xdc, 0x24,
        0x0e, 0xe5, 0xa9, 0xe0,
        0x93, 0xf3, 0xa3, 0xb5,
        0x03, 0x00, 0x40, 0x6e);

/* ---------- UART initialization ---------- */

static void init_uart(void)
{
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    ESP_ERROR_CHECK(uart_driver_install(
        UART_PORT, 4096, 4096, 0, NULL, 0));

    ESP_ERROR_CHECK(
        uart_param_config(UART_PORT, &cfg));

    ESP_ERROR_CHECK(uart_set_pin(
        UART_PORT,
        UART_TX_PIN,
        UART_RX_PIN,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART1: 115200 8N1");
}

/* ---------- BLE RX -> UART TX ---------- */

static int nus_access(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    uint16_t offset = 0;

    uint8_t buf[128];

    while (offset < length) {
        uint16_t n = length - offset;

        if (n > sizeof(buf))
            n = sizeof(buf);

        if (os_mbuf_copydata(
                ctxt->om, offset, n, buf) != 0)
            return BLE_ATT_ERR_UNLIKELY;

        int written = uart_write_bytes(
            UART_PORT, (const char *)buf, n);

        if (written != n)
            return BLE_ATT_ERR_UNLIKELY;

        offset += n;
    }

    return 0;
}

/* ---------- GATT service ---------- */

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_service_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]) {
                {
                    /* Mac writes to this characteristic */
                    .uuid = &nus_rx_uuid.u,
                    .access_cb = nus_access,
                    .flags =
                        BLE_GATT_CHR_F_WRITE |
                        BLE_GATT_CHR_F_WRITE_NO_RSP
                },
                {
                    /* Mac subscribes to notifications */
                    .uuid = &nus_tx_uuid.u,
                    .access_cb = nus_access,
                    .val_handle = &nus_tx_handle,
                    .flags = BLE_GATT_CHR_F_NOTIFY
                },
                { 0 }
            }
    },
    { 0 }
};

/* ---------- BLE advertising ---------- */

static void start_advertising(void);

static int gap_event(
    struct ble_gap_event *event,
    void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            connection = event->connect.conn_handle;
            notifications_enabled = false;
            ESP_LOGI(TAG, "BLE connected");
        } else {
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        notifications_enabled = false;
        connection = BLE_HS_CONN_HANDLE_NONE;

        ESP_LOGI(TAG, "BLE disconnected");
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle ==
            nus_tx_handle) {

            notifications_enabled =
                event->subscribe.cur_notify != 0;

            ESP_LOGI(TAG, "Notifications: %s",
                notifications_enabled
                    ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU: %u",
                 event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};

    fields.flags =
        BLE_HS_ADV_F_DISC_GEN |
        BLE_HS_ADV_F_BREDR_UNSUP;

    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;

    fields.uuids128 =
        (ble_uuid128_t *)&nus_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertisement data: %d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(
        own_addr_type,
        NULL,
        BLE_HS_FOREVER,
        &params,
        gap_event,
        NULL);

    if (rc != 0)
        ESP_LOGE(TAG, "Advertising failed: %d", rc);
    else
        ESP_LOGI(TAG, "Advertising: %s", DEVICE_NAME);
}

static void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    assert(rc == 0);

    start_advertising();
}

static void ble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------- UART RX -> BLE queue ---------- */

static void uart_reader_task(void *arg)
{
    uint8_t buf[256];
    TickType_t last_warning = 0;

    while (true) {
        int n = uart_read_bytes(
            UART_PORT,
            buf,
            sizeof(buf),
            pdMS_TO_TICKS(20));

        if (n <= 0)
            continue;

        /* Drop data while no receiver is subscribed */
        if (!notifications_enabled)
            continue;

        size_t queued = xStreamBufferSend(
            uart_to_ble, buf, n, 0);

        if (queued < (size_t)n) {
            dropped_bytes += n - queued;

            TickType_t now = xTaskGetTickCount();

            if (now - last_warning >=
                pdMS_TO_TICKS(5000)) {

                ESP_LOGW(TAG,
                    "BLE buffer overflow, lost=%lu",
                    (unsigned long)dropped_bytes);

                last_warning = now;
            }
        }
    }
}

/* ---------- BLE queue -> Mac notifications ---------- */

static void ble_sender_task(void *arg)
{
    uint8_t buf[MAX_BLE_CHUNK];

    while (true) {

        if (!notifications_enabled ||
            connection == BLE_HS_CONN_HANDLE_NONE) {

            /* Drain obsolete data while disconnected */
            xStreamBufferReceive(
                uart_to_ble,
                buf,
                sizeof(buf),
                pdMS_TO_TICKS(20));

            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint16_t conn = connection;
        uint16_t mtu = ble_att_mtu(conn);

        size_t chunk = mtu > 3 ? mtu - 3 : 20;

        if (chunk > MAX_BLE_CHUNK)
            chunk = MAX_BLE_CHUNK;

        size_t n = xStreamBufferReceive(
            uart_to_ble,
            buf,
            chunk,
            pdMS_TO_TICKS(50));

        if (n == 0)
            continue;

        struct os_mbuf *om =
            ble_hs_mbuf_from_flat(buf, n);

        if (!om) {
            dropped_bytes += n;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int rc = ble_gatts_notify_custom(
            conn, nus_tx_handle, om);

        if (rc != 0) {
            dropped_bytes += n;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/* ---------- Application entry ---------- */

void app_main(void)
{
    init_identity();

    ESP_LOGI(TAG, "Device ID: %s", device_id);
    ESP_LOGI(TAG, "BLE name: %s", device_name);

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    uart_to_ble = xStreamBufferCreate(
        STREAM_SIZE, 1);
    assert(uart_to_ble != NULL);

    init_uart();

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = ble_on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_svc_gap_device_name_set(
        DEVICE_NAME);
    assert(rc == 0);

    rc = ble_gatts_count_cfg(services);
    assert(rc == 0);

    rc = ble_gatts_add_svcs(services);
    assert(rc == 0);

    xTaskCreate(
        uart_reader_task,
        "uart_reader",
        4096, NULL, 5, NULL);

    xTaskCreate(
        ble_sender_task,
        "ble_sender",
        4096, NULL, 5, NULL);

    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE UART bridge started");

    /* Periodic diagnostics */
    while (true) {
        ESP_LOGI(TAG,
            "IDENTITY ID=%s MAC=%02X%02X%02X%02X%02X%02X"
            " | BLE=%s | Notify=%s | Dropped=%lu",
            device_id,
            factory_mac[0], factory_mac[1],
            factory_mac[2], factory_mac[3],
            factory_mac[4], factory_mac[5],
            connection != BLE_HS_CONN_HANDLE_NONE
                ? "connected" : "disconnected",
            notifications_enabled ? "yes" : "no",
            (unsigned long)dropped_bytes);

        vTaskDelay(pdMS_TO_TICKS(2000));
    }

}
