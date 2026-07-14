/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_split_peripheral_output_relay

#include <errno.h>
#include <stddef.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/ble.h>

#include <zmk/split/output-relay/uuid.h>
#include <zmk/split/output-relay/event.h>

enum or_peripheral_slot_state {
    PERIPHERAL_SLOT_STATE_OPEN,
    PERIPHERAL_SLOT_STATE_CONNECTING,
    PERIPHERAL_SLOT_STATE_CONNECTED,
};

struct or_peripheral_slot {
    enum or_peripheral_slot_state state;
    struct bt_conn *conn;
    bool discovering;
    struct bt_gatt_discover_params discover_params;
    struct bt_gatt_subscribe_params subscribe_params;
    uint16_t update_output_handler;
};

static struct or_peripheral_slot peripherals[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];

static const struct bt_uuid_128 split_or_service_uuid = BT_UUID_INIT_128(
    ZMK_SPLIT_BT_OR_SERVICE_UUID);

/* ZMK starts its mandatory split GATT discovery as soon as a peripheral
 * connects. Keep this optional display-relay discovery out of that startup
 * window, then retry any transient ATT allocation/connection failure instead
 * of leaving the relay handle unset for the lifetime of the connection. */
#define SPLIT_OR_DISCOVER_RETRY_MS 500

static void split_central_discover_work_callback(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(split_central_discover_work,
                               split_central_discover_work_callback);

static int or_peripheral_slot_index_for_conn(struct bt_conn *conn) {
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].conn == conn) {
            return i;
        }
    }
    return -EINVAL;
}

static struct or_peripheral_slot *or_peripheral_slot_for_conn(struct bt_conn *conn) {
    int idx = or_peripheral_slot_index_for_conn(conn);
    if (idx < 0) {
        return NULL;
    }
    return &peripherals[idx];
}

static int release_or_peripheral_slot(int index) {
    if (index < 0 || index >= ZMK_SPLIT_BLE_PERIPHERAL_COUNT) {
        return -EINVAL;
    }

    struct or_peripheral_slot *slot = &peripherals[index];

    if (slot->state == PERIPHERAL_SLOT_STATE_OPEN) {
        return -EINVAL;
    }

    LOG_DBG("Releasing peripheral slot at %d", index);

    if (slot->conn != NULL) {
        slot->conn = NULL;
    }
    slot->state = PERIPHERAL_SLOT_STATE_OPEN;
    slot->discovering = false;

    // Clean up previously discovered handles;
    slot->subscribe_params.value_handle = 0;
    slot->update_output_handler = 0;

    return 0;
}

static int reserve_or_peripheral_slot_for_conn(struct bt_conn *conn) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_PREF_WEAK_BOND)
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].state == PERIPHERAL_SLOT_STATE_OPEN) {
            // Be sure the slot is fully reinitialized.
            peripherals[i].discovering = false;
            peripherals[i].subscribe_params.value_handle = 0;
            peripherals[i].update_output_handler = 0;
            peripherals[i].conn = conn;
            peripherals[i].state = PERIPHERAL_SLOT_STATE_CONNECTED;
            return i;
        }
    }
#else
    int i = zmk_ble_put_peripheral_addr(bt_conn_get_dst(conn));
    if (i >= 0) {
        if (peripherals[i].state == PERIPHERAL_SLOT_STATE_OPEN) {
            // Be sure the slot is fully reinitialized.
            peripherals[i].discovering = false;
            peripherals[i].subscribe_params.value_handle = 0;
            peripherals[i].update_output_handler = 0;
            peripherals[i].conn = conn;
            peripherals[i].state = PERIPHERAL_SLOT_STATE_CONNECTED;
            return i;
        }
    }
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_PREF_WEAK_BOND)

    return -ENOMEM;
}

static int release_or_peripheral_slot_for_conn(struct bt_conn *conn) {
    int idx = or_peripheral_slot_index_for_conn(conn);
    if (idx < 0) {
        return idx;
    }

    return release_or_peripheral_slot(idx);
}

static uint8_t split_central_chrc_discovery_func(struct bt_conn *conn,
                                                 const struct bt_gatt_attr *attr,
                                                 struct bt_gatt_discover_params *params) {
    ARG_UNUSED(params);

    struct or_peripheral_slot *slot = or_peripheral_slot_for_conn(conn);
    if (slot == NULL) {
        LOG_ERR("No peripheral state found for connection");
        return BT_GATT_ITER_STOP;
    }

    if (!attr) {
        LOG_DBG("Discover complete");
        slot->discovering = false;
        return BT_GATT_ITER_STOP;
    }

    if (!attr->user_data) {
        LOG_ERR("Required user data not passed to discovery");
        slot->discovering = false;
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[ATTRIBUTE] handle %u", attr->handle);
    const struct bt_uuid *chrc_uuid = ((struct bt_gatt_chrc *)attr->user_data)->uuid;

#if IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY)
    if (bt_uuid_cmp(chrc_uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_OR_CHAR_OUTPUT_STATE_UUID)) == 0) {
        LOG_DBG("Found update output handle");
        slot->update_output_handler = bt_gatt_attr_value_handle(attr);
        slot->discovering = false;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY) */

    bool subscribed = true;
#if IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY)
    subscribed = subscribed && slot->update_output_handler;
#endif /* IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY) */

    return subscribed ? BT_GATT_ITER_STOP : BT_GATT_ITER_CONTINUE;
}

static uint8_t split_central_service_discovery_func(struct bt_conn *conn,
                                                    const struct bt_gatt_attr *attr,
                                                    struct bt_gatt_discover_params *params) {
    struct or_peripheral_slot *slot = or_peripheral_slot_for_conn(conn);
    if (slot == NULL) {
        LOG_ERR("No peripheral state found for connection");
        return BT_GATT_ITER_STOP;
    }

    if (!attr) {
        LOG_DBG("Discover complete");
        slot->discovering = false;
        (void)memset(params, 0, sizeof(*params));
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[ATTRIBUTE] handle %u", attr->handle);

    if (!attr->user_data) {
        LOG_ERR("Required user data not passed to service discovery");
        slot->discovering = false;
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("Found split service");
    const struct bt_gatt_service_val *service = attr->user_data;
    slot->discover_params.uuid = NULL;
    slot->discover_params.func = split_central_chrc_discovery_func;
    slot->discover_params.start_handle = attr->handle + 1;
    slot->discover_params.end_handle = service->end_handle;
    slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(conn, &slot->discover_params);
    if (err) {
        LOG_ERR("Failed to start discovering split service characteristics (err %d)", err);
        slot->discovering = false;
    }
    return BT_GATT_ITER_STOP;
}

static void split_central_begin_discovery(struct or_peripheral_slot *slot) {
    slot->discover_params.uuid = &split_or_service_uuid.uuid;
    slot->discover_params.func = split_central_service_discovery_func;
    slot->discover_params.start_handle = 0x0001;
    slot->discover_params.end_handle = 0xffff;
    slot->discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    int err = bt_gatt_discover(slot->conn, &slot->discover_params);
    if (err) {
        LOG_DBG("Split output relay discovery failed (err %d), will retry", err);
        return;
    }

    slot->discovering = true;
}

static bool split_central_discovery_pending(void) {
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].state == PERIPHERAL_SLOT_STATE_CONNECTED &&
            peripherals[i].conn != NULL && peripherals[i].update_output_handler == 0) {
            return true;
        }
    }

    return false;
}

static void split_central_discover_work_callback(struct k_work *work) {
    ARG_UNUSED(work);

    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        struct or_peripheral_slot *slot = &peripherals[i];

        if (slot->state != PERIPHERAL_SLOT_STATE_CONNECTED || slot->conn == NULL ||
            slot->update_output_handler != 0 || slot->discovering) {
            continue;
        }

        if (bt_conn_get_security(slot->conn) < BT_SECURITY_L2) {
            continue;
        }

        split_central_begin_discovery(slot);
    }

    if (split_central_discovery_pending()) {
        k_work_reschedule(&split_central_discover_work,
                          K_MSEC(SPLIT_OR_DISCOVER_RETRY_MS));
    }
}

static void split_central_connected(struct bt_conn *conn, uint8_t conn_err) {
    char addr_str[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));

    bt_conn_get_info(conn, &info);

    if (info.role != BT_CONN_ROLE_CENTRAL) {
        LOG_DBG("SKIPPING FOR ROLE %d", info.role);
        return;
    }

    if (conn_err) {
        LOG_ERR("Failed to connect to %s (%u)", addr_str, conn_err);
        release_or_peripheral_slot_for_conn(conn);
        return;
    }

    LOG_DBG("Connected: %s", addr_str);

    int slot_idx = reserve_or_peripheral_slot_for_conn(conn);
    if (slot_idx < 0) {
        LOG_ERR("Unable to reserve peripheral slot for connection (err %d)", slot_idx);
        return;
    }

    /* Let ZMK complete its mandatory split-service discovery first. */
    k_work_reschedule(&split_central_discover_work,
                      K_MSEC(SPLIT_OR_DISCOVER_RETRY_MS));
}

static void split_central_security_changed(struct bt_conn *conn, bt_security_t level,
                                           enum bt_security_err err) {
    ARG_UNUSED(level);
    ARG_UNUSED(err);

    if (or_peripheral_slot_for_conn(conn) != NULL) {
        /* The worker performs the actual security-level check. */
        k_work_reschedule(&split_central_discover_work, K_MSEC(50));
    }
}

static void split_central_disconnected(struct bt_conn *conn, uint8_t reason) {
    char addr_str[BT_ADDR_LE_STR_LEN];
    int err;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));

    LOG_DBG("Disconnected: %s (reason %d)", addr_str, reason);

    err = release_or_peripheral_slot_for_conn(conn);

    if (err < 0) {
        return;
    }

    if (split_central_discovery_pending()) {
        k_work_reschedule(&split_central_discover_work,
                          K_MSEC(SPLIT_OR_DISCOVER_RETRY_MS));
    } else {
        (void)k_work_cancel_delayable(&split_central_discover_work);
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected = split_central_connected,
    .disconnected = split_central_disconnected,
    .security_changed = split_central_security_changed,
};

K_THREAD_STACK_DEFINE(split_central_split_or_run_q_stack,
                      CONFIG_ZMK_SPLIT_BLE_CENTRAL_SPLIT_RUN_STACK_SIZE);

struct k_work_q split_central_split_or_run_q;

#if IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY)

K_MSGQ_DEFINE(zmk_split_central_split_or_run_msgq,
              sizeof(struct zmk_split_bt_output_relay_event),
              CONFIG_ZMK_SPLIT_BLE_CENTRAL_SPLIT_RUN_QUEUE_SIZE, 4);

void split_central_split_or_run_callback(struct k_work *work) {
    struct zmk_split_bt_output_relay_event event;

    LOG_DBG("");

    while (k_msgq_get(&zmk_split_central_split_or_run_msgq, &event, K_NO_WAIT) == 0) {
        size_t payload_size =
            MIN(event.payload_size, ZMK_SPLIT_PERIPHERAL_OUTPUT_PAYLOAD_MAX);
        size_t msg_len =
            offsetof(struct zmk_split_bt_output_relay_event, payload) + payload_size;

        for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
            if (peripherals[i].state != PERIPHERAL_SLOT_STATE_CONNECTED) {
                continue;
            }

            if (peripherals[i].update_output_handler == 0) {
                // It appears that sometimes the peripheral is considered connected
                // before the GATT characteristics have been discovered. If this is
                // the case, the update_output_handler handle will not yet be set.
                continue;
            }

            if (event.payload_size != payload_size) {
                LOG_WRN("Truncating payload from %u to %zu bytes", event.payload_size,
                        payload_size);
            }

            int err = bt_gatt_write_without_response(peripherals[i].conn,
                                                     peripherals[i].update_output_handler,
                                                     &event, msg_len, true);

            if (err) {
                LOG_ERR("Failed to write split output characteristic (err %d)", err);
            }
        }

    }
}

K_WORK_DEFINE(split_central_split_or_run_work, split_central_split_or_run_callback);

static int split_bt_invoke_output(struct zmk_split_bt_output_relay_event event) {
    LOG_DBG("");

    int err = k_msgq_put(&zmk_split_central_split_or_run_msgq, &event, K_MSEC(100));
    if (err) {
        switch (err) {
        case -EAGAIN: {
            LOG_WRN("Consumer message queue full, dropping oldest message");
            struct zmk_split_bt_output_relay_event discarded_report;
            k_msgq_get(&zmk_split_central_split_or_run_msgq, &discarded_report, K_NO_WAIT);
            err = k_msgq_put(&zmk_split_central_split_or_run_msgq, &event, K_NO_WAIT);
            if (err) {
                LOG_WRN("Failed to queue split output after dropping oldest message (%d)", err);
                return err;
            }
            break;
        }
        default:
            LOG_WRN("Failed to queue behavior to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&split_central_split_or_run_q, &split_central_split_or_run_work);

    return 0;
};

int relay_channel_get_for_device(const struct device *device);

int zmk_split_bt_invoke_output(const struct device *dev,
                               struct zmk_split_bt_output_relay_event event) {

    int relay_channel = relay_channel_get_for_device(dev);
    if (relay_channel < 0) {
        LOG_DBG("Unable to retrieve relay channel for device");
        return relay_channel;
    }

    struct zmk_split_bt_output_relay_event ev = {0};
    ev.relay_channel = (uint8_t)relay_channel;
    ev.value = event.value;
    ev.payload_size =
        MIN(event.payload_size, (uint8_t)ZMK_SPLIT_PERIPHERAL_OUTPUT_PAYLOAD_MAX);
    memcpy(ev.payload, event.payload, ev.payload_size);

    LOG_DBG("Send output: rc-%d v-%d payload-%u", ev.relay_channel, ev.value,
            ev.payload_size);

    return split_bt_invoke_output(ev);
}

int zmk_split_bt_invoke_output_channel(uint8_t relay_channel, uint8_t value,
                                       const uint8_t *payload, uint8_t payload_size) {
    struct zmk_split_bt_output_relay_event ev = {0};
    ev.relay_channel = relay_channel;
    ev.value = value;
    ev.payload_size = MIN(payload_size, (uint8_t)ZMK_SPLIT_PERIPHERAL_OUTPUT_PAYLOAD_MAX);
    if (payload != NULL && ev.payload_size > 0) {
        memcpy(ev.payload, payload, ev.payload_size);
    }

    LOG_DBG("Send output (direct): rc-%d v-%d payload-%u", ev.relay_channel, ev.value,
            ev.payload_size);

    return split_bt_invoke_output(ev);
}

#endif // IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY)

static int zmk_split_bt_central_init(void) {
    k_work_queue_start(&split_central_split_or_run_q, split_central_split_or_run_q_stack,
                       K_THREAD_STACK_SIZEOF(split_central_split_or_run_q_stack),
                       CONFIG_ZMK_BLE_THREAD_PRIORITY, NULL);
    bt_conn_cb_register(&conn_callbacks);
    return 0;
}

SYS_INIT(zmk_split_bt_central_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);


#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct split_peripheral_output_relay_config {
    uint8_t relay_channel;
    const struct device *device;
};

#define OUTPUT_RELY_CFG_DEFINE(n)                                               \
    static const struct split_peripheral_output_relay_config config_##n = {     \
        .relay_channel = DT_PROP(DT_DRV_INST(n), relay_channel),                \
        .device = DEVICE_DT_GET(DT_INST_PHANDLE(n, device)),                    \
    };

DT_INST_FOREACH_STATUS_OKAY(OUTPUT_RELY_CFG_DEFINE)

int relay_channel_get_for_device(const struct device *device) {
    #define OR_C_COND_CMP_RELAY_CHANNEL(n)            \
        if (device == config_##n.device) {            \
            return config_##n.relay_channel;          \
        }
    DT_INST_FOREACH_STATUS_OKAY(OR_C_COND_CMP_RELAY_CHANNEL)
    return -ENODEV;
}

#else

int relay_channel_get_for_device(const struct device *device) {
    return -ENODEV;
}

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
