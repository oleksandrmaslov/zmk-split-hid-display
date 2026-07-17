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
    PERIPHERAL_SLOT_STATE_RELEASING,
};

enum or_peripheral_discovery_stage {
    OR_PERIPHERAL_DISCOVER_SERVICE,
    OR_PERIPHERAL_DISCOVER_CHARACTERISTIC,
    OR_PERIPHERAL_DISCOVER_READY,
};

struct or_peripheral_slot {
    enum or_peripheral_slot_state state;
    struct bt_conn *conn;
    bool discovering;
    enum or_peripheral_discovery_stage discovery_stage;
    uint8_t discovery_attempts;
    int64_t discovery_retry_at;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    struct bt_gatt_discover_params discover_params;
    uint16_t update_output_handler;
};

static struct or_peripheral_slot peripherals[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];
static K_MUTEX_DEFINE(split_central_state_mutex);

static const struct bt_uuid_128 split_or_service_uuid = BT_UUID_INIT_128(
    ZMK_SPLIT_BT_OR_SERVICE_UUID);

/* ZMK starts its mandatory split-service discovery as soon as a peripheral
 * connects. The relay is optional, so start it later and retry if the ATT
 * request queue is temporarily full. */
#define SPLIT_OR_DISCOVER_RETRY_MS 500
#define SPLIT_OR_DISCOVER_MAX_ATTEMPTS 3

static void split_central_discover_work_callback(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(split_central_discover_work,
                               split_central_discover_work_callback);

/* The following slot helpers require split_central_state_mutex to be held. */
static int or_peripheral_slot_index_for_conn_locked(struct bt_conn *conn) {
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].conn == conn) {
            return i;
        }
    }
    return -EINVAL;
}

static struct or_peripheral_slot *or_peripheral_slot_for_conn_locked(struct bt_conn *conn) {
    int idx = or_peripheral_slot_index_for_conn_locked(conn);
    if (idx < 0) {
        return NULL;
    }
    return &peripherals[idx];
}

/* The caller owns the returned connection reference and must unref it after
 * dropping the state lock. This must only run after a discovery callback has
 * stopped, because Zephyr retains discover_params until then. */
static struct bt_conn *release_or_peripheral_slot_locked(int index) {
    if (index < 0 || index >= ZMK_SPLIT_BLE_PERIPHERAL_COUNT) {
        return NULL;
    }

    struct or_peripheral_slot *slot = &peripherals[index];

    if (slot->state == PERIPHERAL_SLOT_STATE_OPEN || slot->discovering) {
        return NULL;
    }

    LOG_DBG("Releasing peripheral slot at %d", index);

    struct bt_conn *conn = slot->conn;
    slot->conn = NULL;
    slot->state = PERIPHERAL_SLOT_STATE_OPEN;
    slot->discovering = false;
    slot->discovery_stage = OR_PERIPHERAL_DISCOVER_SERVICE;
    slot->discovery_attempts = 0;
    slot->discovery_retry_at = 0;
    slot->service_start_handle = 0;
    slot->service_end_handle = 0;
    slot->update_output_handler = 0;
    memset(&slot->discover_params, 0, sizeof(slot->discover_params));

    return conn;
}

static void reserve_or_peripheral_slot(struct or_peripheral_slot *slot, struct bt_conn *conn) {
    memset(&slot->discover_params, 0, sizeof(slot->discover_params));
    slot->conn = bt_conn_ref(conn);
    slot->state = PERIPHERAL_SLOT_STATE_CONNECTED;
    slot->discovering = false;
    slot->discovery_stage = OR_PERIPHERAL_DISCOVER_SERVICE;
    slot->discovery_attempts = 0;
    slot->discovery_retry_at = k_uptime_get() + SPLIT_OR_DISCOVER_RETRY_MS;
    slot->service_start_handle = 0;
    slot->service_end_handle = 0;
    slot->update_output_handler = 0;
}

static int reserve_or_peripheral_slot_for_conn(struct bt_conn *conn) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_PREF_WEAK_BOND)
    k_mutex_lock(&split_central_state_mutex, K_FOREVER);
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].state == PERIPHERAL_SLOT_STATE_OPEN) {
            reserve_or_peripheral_slot(&peripherals[i], conn);
            k_mutex_unlock(&split_central_state_mutex);
            return i;
        }
    }
    k_mutex_unlock(&split_central_state_mutex);
#else
    int i = zmk_ble_put_peripheral_addr(bt_conn_get_dst(conn));
    if (i >= 0) {
        k_mutex_lock(&split_central_state_mutex, K_FOREVER);
        if (peripherals[i].state == PERIPHERAL_SLOT_STATE_OPEN) {
            reserve_or_peripheral_slot(&peripherals[i], conn);
            k_mutex_unlock(&split_central_state_mutex);
            return i;
        }
        k_mutex_unlock(&split_central_state_mutex);
    }
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_PREF_WEAK_BOND)

    return -ENOMEM;
}

/* This runs with split_central_state_mutex held. */
static bool split_central_retry_discovery_locked(struct or_peripheral_slot *slot) {
    slot->discovering = false;
    slot->discovery_retry_at = k_uptime_get() + SPLIT_OR_DISCOVER_RETRY_MS;
    slot->service_start_handle = 0;
    slot->service_end_handle = 0;

    if (++slot->discovery_attempts >= SPLIT_OR_DISCOVER_MAX_ATTEMPTS) {
        slot->discovery_stage = OR_PERIPHERAL_DISCOVER_READY;
        return false;
    }

    slot->discovery_stage = OR_PERIPHERAL_DISCOVER_SERVICE;
    return true;
}

static uint8_t split_central_chrc_discovery_func(struct bt_conn *conn,
                                                 const struct bt_gatt_attr *attr,
                                                 struct bt_gatt_discover_params *params) {
    ARG_UNUSED(params);

    k_mutex_lock(&split_central_state_mutex, K_FOREVER);
    struct or_peripheral_slot *slot = or_peripheral_slot_for_conn_locked(conn);
    if (slot == NULL) {
        k_mutex_unlock(&split_central_state_mutex);
        LOG_ERR("No peripheral state found for connection");
        return BT_GATT_ITER_STOP;
    }

    if (slot->state == PERIPHERAL_SLOT_STATE_RELEASING) {
        slot->discovering = false;
        k_mutex_unlock(&split_central_state_mutex);
        return BT_GATT_ITER_STOP;
    }

    if (!attr || !attr->user_data) {
        bool retry = split_central_retry_discovery_locked(slot);
        if (retry) {
            LOG_DBG("Split output relay characteristic discovery incomplete; retrying");
        } else {
            LOG_WRN("Split output relay characteristic unavailable after %u attempts",
                    (unsigned int)SPLIT_OR_DISCOVER_MAX_ATTEMPTS);
        }
        k_mutex_unlock(&split_central_state_mutex);
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[ATTRIBUTE] handle %u", attr->handle);
    const struct bt_uuid *chrc_uuid = ((struct bt_gatt_chrc *)attr->user_data)->uuid;

#if IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY)
    if (bt_uuid_cmp(chrc_uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_OR_CHAR_OUTPUT_STATE_UUID)) == 0) {
        LOG_DBG("Found update output handle");
        slot->update_output_handler = bt_gatt_attr_value_handle(attr);
        slot->discovering = false;
        slot->discovery_stage = OR_PERIPHERAL_DISCOVER_READY;
        slot->discovery_retry_at = k_uptime_get() + SPLIT_OR_DISCOVER_RETRY_MS;
        k_mutex_unlock(&split_central_state_mutex);
        return BT_GATT_ITER_STOP;
    }
#endif /* IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY) */

    k_mutex_unlock(&split_central_state_mutex);
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t split_central_service_discovery_func(struct bt_conn *conn,
                                                    const struct bt_gatt_attr *attr,
                                                    struct bt_gatt_discover_params *params) {
    ARG_UNUSED(params);

    bool schedule_next_stage = false;

    k_mutex_lock(&split_central_state_mutex, K_FOREVER);
    struct or_peripheral_slot *slot = or_peripheral_slot_for_conn_locked(conn);
    if (slot == NULL) {
        k_mutex_unlock(&split_central_state_mutex);
        LOG_ERR("No peripheral state found for connection");
        return BT_GATT_ITER_STOP;
    }

    if (slot->state == PERIPHERAL_SLOT_STATE_RELEASING) {
        slot->discovering = false;
        k_mutex_unlock(&split_central_state_mutex);
        return BT_GATT_ITER_STOP;
    }

    if (!attr || !attr->user_data) {
        bool retry = split_central_retry_discovery_locked(slot);
        if (retry) {
            LOG_DBG("Split output relay service discovery incomplete; retrying");
        } else {
            LOG_WRN("Split output relay service unavailable after %u attempts",
                    (unsigned int)SPLIT_OR_DISCOVER_MAX_ATTEMPTS);
        }
        k_mutex_unlock(&split_central_state_mutex);
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_service_val *service = attr->user_data;
    if (service->end_handle <= attr->handle) {
        bool retry = split_central_retry_discovery_locked(slot);
        LOG_WRN("Invalid split output relay service range%s", retry ? "; retrying" : "");
        k_mutex_unlock(&split_central_state_mutex);
        return BT_GATT_ITER_STOP;
    }

    /* Keep discover_params intact until this callback returns. The next
     * discovery stage is started by delayed work, never from this callback. */
    slot->service_start_handle = attr->handle + 1;
    slot->service_end_handle = service->end_handle;
    slot->discovering = false;
    slot->discovery_stage = OR_PERIPHERAL_DISCOVER_CHARACTERISTIC;
    slot->discovery_retry_at = k_uptime_get() + SPLIT_OR_DISCOVER_RETRY_MS;
    schedule_next_stage = true;
    k_mutex_unlock(&split_central_state_mutex);

    if (schedule_next_stage) {
        k_work_reschedule(&split_central_discover_work,
                          K_MSEC(SPLIT_OR_DISCOVER_RETRY_MS));
    }

    return BT_GATT_ITER_STOP;
}

/* This runs with split_central_state_mutex held. */
static void split_central_begin_discovery_locked(struct or_peripheral_slot *slot) {
    memset(&slot->discover_params, 0, sizeof(slot->discover_params));

    if (slot->discovery_stage == OR_PERIPHERAL_DISCOVER_SERVICE) {
        slot->discover_params.uuid = &split_or_service_uuid.uuid;
        slot->discover_params.func = split_central_service_discovery_func;
        slot->discover_params.start_handle = 0x0001;
        slot->discover_params.end_handle = 0xffff;
        slot->discover_params.type = BT_GATT_DISCOVER_PRIMARY;
    } else if (slot->discovery_stage == OR_PERIPHERAL_DISCOVER_CHARACTERISTIC &&
               slot->service_start_handle != 0 && slot->service_end_handle != 0) {
        slot->discover_params.uuid = NULL;
        slot->discover_params.func = split_central_chrc_discovery_func;
        slot->discover_params.start_handle = slot->service_start_handle;
        slot->discover_params.end_handle = slot->service_end_handle;
        slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    } else {
        LOG_WRN("Invalid split output relay discovery state");
        slot->discovery_stage = OR_PERIPHERAL_DISCOVER_READY;
        return;
    }

    slot->discovering = true;
    int err = bt_gatt_discover(slot->conn, &slot->discover_params);
    if (err) {
        /* An immediate error (normally a full ATT request queue) has no
         * terminal callback. Treat it as transient rather than consuming one
         * of the bounded completed-discovery attempts. */
        slot->discovering = false;
        memset(&slot->discover_params, 0, sizeof(slot->discover_params));
        slot->discovery_retry_at = k_uptime_get() + SPLIT_OR_DISCOVER_RETRY_MS;
        LOG_DBG("Split output relay discovery failed (%d); will retry", err);
    }
}

/* This runs with split_central_state_mutex held. */
static bool split_central_discovery_pending_locked(void) {
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].state == PERIPHERAL_SLOT_STATE_CONNECTED &&
            peripherals[i].conn != NULL &&
            peripherals[i].discovery_stage != OR_PERIPHERAL_DISCOVER_READY) {
            return true;
        }
    }

    return false;
}

static void split_central_discover_work_callback(struct k_work *work) {
    ARG_UNUSED(work);

    int64_t now = k_uptime_get();

    k_mutex_lock(&split_central_state_mutex, K_FOREVER);
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        struct or_peripheral_slot *slot = &peripherals[i];

        if (slot->state != PERIPHERAL_SLOT_STATE_CONNECTED || slot->conn == NULL ||
            slot->discovery_stage == OR_PERIPHERAL_DISCOVER_READY || slot->discovering) {
            continue;
        }

        /* The GATT callback has not necessarily returned at the instant it
         * marks a stage complete. Leave its params untouched until the next
         * delayed pass. */
        if (now < slot->discovery_retry_at) {
            continue;
        }

        if (bt_conn_get_security(slot->conn) < BT_SECURITY_L2) {
            continue;
        }

        split_central_begin_discovery_locked(slot);
    }

    bool pending = split_central_discovery_pending_locked();
    k_mutex_unlock(&split_central_state_mutex);

    if (pending) {
        k_work_reschedule(&split_central_discover_work, K_MSEC(SPLIT_OR_DISCOVER_RETRY_MS));
    }
}

static void split_central_connected(struct bt_conn *conn, uint8_t conn_err) {
    char addr_str[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));

    int info_err = bt_conn_get_info(conn, &info);
    if (conn_err || info_err) {
        LOG_ERR("Failed to connect to %s (conn err %u, info err %d)", addr_str, conn_err,
                info_err);
        return;
    }

    if (info.role != BT_CONN_ROLE_CENTRAL) {
        LOG_DBG("SKIPPING FOR ROLE %d", info.role);
        return;
    }

    LOG_DBG("Connected: %s", addr_str);

    int slot_idx = reserve_or_peripheral_slot_for_conn(conn);

    if (slot_idx < 0) {
        LOG_ERR("Unable to reserve peripheral slot for connection (err %d)", slot_idx);
        return;
    }

    /* Let ZMK finish its mandatory split-service discovery first. */
    k_work_reschedule(&split_central_discover_work, K_MSEC(SPLIT_OR_DISCOVER_RETRY_MS));
}

static void split_central_security_changed(struct bt_conn *conn, bt_security_t level,
                                           enum bt_security_err err) {
    ARG_UNUSED(level);
    ARG_UNUSED(err);

    k_mutex_lock(&split_central_state_mutex, K_FOREVER);
    struct or_peripheral_slot *slot = or_peripheral_slot_for_conn_locked(conn);
    bool schedule_discovery = slot != NULL && slot->state == PERIPHERAL_SLOT_STATE_CONNECTED &&
                              slot->discovery_stage != OR_PERIPHERAL_DISCOVER_READY;
    k_mutex_unlock(&split_central_state_mutex);

    if (schedule_discovery) {
        k_work_reschedule(&split_central_discover_work, K_MSEC(50));
    }
}

static void split_central_disconnected(struct bt_conn *conn, uint8_t reason) {
    char addr_str[BT_ADDR_LE_STR_LEN];
    struct bt_conn *cancel_conn = NULL;
    struct bt_conn *release_conn = NULL;
    struct or_peripheral_slot *slot = NULL;
    int slot_idx;
    bool pending;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));

    LOG_DBG("Disconnected: %s (reason %d)", addr_str, reason);

    k_mutex_lock(&split_central_state_mutex, K_FOREVER);
    slot_idx = or_peripheral_slot_index_for_conn_locked(conn);
    if (slot_idx < 0) {
        k_mutex_unlock(&split_central_state_mutex);
        return;
    }

    slot = &peripherals[slot_idx];
    slot->state = PERIPHERAL_SLOT_STATE_RELEASING;
    if (slot->discovering && slot->conn != NULL) {
        cancel_conn = bt_conn_ref(slot->conn);
        pending = false;
    } else {
        release_conn = release_or_peripheral_slot_locked(slot_idx);
        pending = split_central_discovery_pending_locked();
    }
    k_mutex_unlock(&split_central_state_mutex);

    if (cancel_conn != NULL) {
        /* bt_gatt_cancel() completes the terminal callback synchronously. Do
         * not reset discover_params or release the slot before it returns. */
        bt_gatt_cancel(cancel_conn, &slot->discover_params);
        bt_conn_unref(cancel_conn);

        k_mutex_lock(&split_central_state_mutex, K_FOREVER);
        if (slot->state == PERIPHERAL_SLOT_STATE_RELEASING) {
            /* A concurrent ATT teardown may have removed the request before
             * bt_gatt_cancel() found it. In either case it no longer retains
             * discover_params once bt_gatt_cancel() returns. */
            slot->discovering = false;
            release_conn = release_or_peripheral_slot_locked(slot_idx);
        }
        pending = split_central_discovery_pending_locked();
        k_mutex_unlock(&split_central_state_mutex);
    }

    if (release_conn != NULL) {
        bt_conn_unref(release_conn);
    }

    if (pending) {
        k_work_reschedule(&split_central_discover_work, K_MSEC(SPLIT_OR_DISCOVER_RETRY_MS));
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
            struct bt_conn *conn = NULL;
            uint16_t update_output_handler = 0;

            k_mutex_lock(&split_central_state_mutex, K_FOREVER);
            if (peripherals[i].state == PERIPHERAL_SLOT_STATE_CONNECTED &&
                peripherals[i].conn != NULL && peripherals[i].update_output_handler != 0) {
                conn = bt_conn_ref(peripherals[i].conn);
                update_output_handler = peripherals[i].update_output_handler;
            }
            k_mutex_unlock(&split_central_state_mutex);

            if (conn == NULL) {
                // It appears that sometimes the peripheral is considered connected
                // before the GATT characteristics have been discovered. If this is
                // the case, the update_output_handler handle will not yet be set.
                continue;
            }

            if (event.payload_size != payload_size) {
                LOG_WRN("Truncating payload from %u to %zu bytes", event.payload_size,
                        payload_size);
            }

            int err = bt_gatt_write_without_response(conn, update_output_handler,
                                                      &event, msg_len, true);

            if (err) {
                LOG_ERR("Failed to write split output characteristic (err %d)", err);
            }

            bt_conn_unref(conn);
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
