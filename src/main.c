#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/pm/pm.h>
#include <hal/nrf_gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/settings/settings.h>

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

const struct device *sensor = DEVICE_DT_GET_ANY(st_lis2dh);

#define CUSTOM_MAC_ADDR "DE:AD:BE:AF:BA:11"

static int custom_id = BT_ID_DEFAULT;
struct bt_conn *current_conn = NULL;

// --- 1. GATT PAYLOAD & UUID DEFINITIONS ---
static uint8_t accel_payload[32] = {0};

#define BT_UUID_CUSTOM_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
#define BT_UUID_CUSTOM_CHAR_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1)

// --- 2. GATT SERVICE DEFINITION ---
static void accel_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

BT_GATT_SERVICE_DEFINE(accel_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(BT_UUID_CUSTOM_SERVICE_VAL)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(BT_UUID_CUSTOM_CHAR_VAL),
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_AUTHEN,
                           NULL, NULL, accel_payload),
    BT_GATT_CCC(accel_ccc_cfg_changed,
                BT_GATT_PERM_READ_AUTHEN | BT_GATT_PERM_WRITE_AUTHEN)
);

// --- 3. BOND CHECK (Pure C, no lambdas) ---
typedef struct {
    const bt_addr_le_t *target_addr;
    bool found;
} bond_check_t;

static void bond_check_cb(const struct bt_bond_info *info, void *user_data)
{
    bond_check_t *check = (bond_check_t *)user_data;
    if (bt_addr_le_eq(&info->addr, check->target_addr)) {
        check->found = true;
    }
}

static bool device_is_bonded(const bt_addr_le_t *addr)
{
    bond_check_t check = {
        .target_addr = addr,
        .found       = false,
    };
    bt_foreach_bond(custom_id, bond_check_cb, &check);
    return check.found;
}

// --- 4. CONNECTION & SECURITY MANAGEMENT ---
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }

    current_conn = bt_conn_ref(conn);
    const bt_addr_le_t *peer_addr = bt_conn_get_dst(conn);

    if (device_is_bonded(peer_addr)) {
        printk("Known bonded device reconnected. No passkey needed.\n");
    } else {
        printk("New device. Starting secure pairing...\n");
    }

    bt_conn_set_security(conn, BT_SECURITY_L3);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Client Disconnected (reason %u)\n", reason);
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = connected,
    .disconnected = disconnected,
};

// --- 5. AUTH CALLBACKS ---
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("\n========================================\n");
    printk(">>> PASSKEY FOR %s: %06u <<<\n", addr, passkey);
    printk("========================================\n");
}

static void auth_cancel(struct bt_conn *conn)
{
    printk("Pairing cancelled.\n");
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("Pairing complete with %s. Bonded: %s\n", addr, bonded ? "YES" : "NO");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("Pairing FAILED with %s (reason %d)\n", addr, reason);
}

static struct bt_conn_auth_cb auth_cb_display = {
    .passkey_display = auth_passkey_display,
    .passkey_entry   = NULL,
    .cancel          = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
    .pairing_complete = pairing_complete,
    .pairing_failed   = pairing_failed,
};

// --- 6. ADVERTISING DATA ---
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

// --- 7. ADDRESS INITIALIZATION ---
static void set_custom_mac_address(void)
{
    bt_addr_le_t addr;
    int err;

    err = bt_addr_le_from_str(CUSTOM_MAC_ADDR, "random", &addr);
    if (err) {
        printk("Invalid BT address (err %d)\n", err);
        return;
    }

    int id = bt_id_create(&addr, NULL);
    if (id < 0) {
        printk("Creating new ID failed (err %d)\n", id);
        return;
    }

    custom_id = id;
    printk("Custom MAC set: %s (id=%d)\n", CUSTOM_MAC_ADDR, custom_id);
}

// --- 8. SENSOR FETCH LOGIC ---
void fetch_acc_data(void)
{
    struct sensor_value accel[3];
    int err;

    nrf_gpio_cfg_output(DT_GPIO_PIN(DT_NODELABEL(led0), gpios));
    nrf_gpio_cfg_output(DT_GPIO_PIN(DT_NODELABEL(led1), gpios));
    nrf_gpio_cfg_output(DT_GPIO_PIN(DT_NODELABEL(led2), gpios));

    err = sensor_sample_fetch(sensor);
    if (err == 0) {
        sensor_channel_get(sensor, SENSOR_CHAN_ACCEL_XYZ, accel);
    } else {
        printk("ERROR: Accelerometer update failed: %d\n", err);
        return;
    }

    int8_t x_b = accel[0].val1;
    int8_t x_a = accel[0].val2 / 10000;
    int8_t y_b = accel[1].val1;
    int8_t y_a = accel[1].val2 / 10000;
    int8_t z_b = accel[2].val1;
    int8_t z_a = accel[2].val2 / 10000;

    if (x_b == 0 && y_b == 0 && (z_b >= 9 || z_b <= -9)) {
        nrf_gpio_pin_clear(DT_GPIO_PIN(DT_NODELABEL(led2), gpios));
        nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led0), gpios));
        nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led1), gpios));
    } else if (y_b == 0 && z_b == 0 && (x_b >= 9 || x_b <= -9)) {
        nrf_gpio_pin_clear(DT_GPIO_PIN(DT_NODELABEL(led0), gpios));
        nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led1), gpios));
        nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led2), gpios));
    } else if (x_b == 0 && z_b == 0 && (y_b >= 9 || y_b <= -9)) {
        nrf_gpio_pin_clear(DT_GPIO_PIN(DT_NODELABEL(led1), gpios));
        nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led2), gpios));
        nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led0), gpios));
    } else {
        nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led0), gpios));
        nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led1), gpios));
        nrf_gpio_pin_set(DT_GPIO_PIN(DT_NODELABEL(led2), gpios));
    }

    snprintf(accel_payload, sizeof(accel_payload),
             "X:%d.%d Y:%d.%d Z:%d.%d",
             x_b, x_a, y_b, y_a, z_b, z_a);

    if (current_conn) {
        bt_gatt_notify(current_conn, &accel_svc.attrs[1],
                       accel_payload, strlen(accel_payload));
    }
}

// --- 9. MAIN ---
int main(void)
{
    int err;

    printk("Starting LIS2DH Secure GATT Node\n");

    if (!device_is_ready(sensor)) {
        printk("Sensor not ready\n");
        return -1;
    }

    err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return -1;
    }

    printk("Bluetooth initialized\n");

    settings_load();
    printk("Settings loaded (bonds restored)\n");

    bt_conn_auth_cb_register(&auth_cb_display);
    bt_conn_auth_info_cb_register(&auth_info_cb);

    set_custom_mac_address();

    struct bt_le_adv_param my_adv_param = {
        .id                 = custom_id,
        .sid                = 0,
        .secondary_max_skip = 0,
        .options            = BT_LE_ADV_OPT_CONN,
        .interval_min       = BT_GAP_ADV_FAST_INT_MIN_2,
        .interval_max       = BT_GAP_ADV_FAST_INT_MAX_2,
        .peer               = NULL,
    };

    err = bt_le_adv_start(&my_adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        printk("Advertising start failed (err %d)\n", err);
        return -1;
    }

    printk("Advertising started\n");

    while (1) {
        fetch_acc_data();
        k_msleep(100);
    }

    return 0;
}