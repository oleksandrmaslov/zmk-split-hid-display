/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_split_peripheral_output_relay

#include <stddef.h>
#include <string.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zmk/split/output-relay/uuid.h>
#include <zmk/split/output-relay/event.h>

#ifdef CONFIG_RAW_HID_SPLIT_RELAY_CHANNEL
#include <raw_hid/events.h>
#include <raw_hid/split.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_OUTPUT_BEHAVIOR_LISTENER)
#include <zmk/output/output_generic.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY)

static struct zmk_split_bt_output_relay_event split_output_run_payload;

#ifdef CONFIG_RAW_HID_SPLIT_RELAY_CHANNEL
static uint8_t raw_hid_reassembly_buf[CONFIG_RAW_HID_REPORT_SIZE];
static uint8_t raw_hid_reassembly_sequence;
static uint8_t raw_hid_reassembly_total;
static uint8_t raw_hid_reassembly_received;
static bool raw_hid_reassembly_active;

static void reset_raw_hid_reassembly(void) {
    raw_hid_reassembly_sequence = 0;
    raw_hid_reassembly_total = 0;
    raw_hid_reassembly_received = 0;
    raw_hid_reassembly_active = false;
}

static ssize_t handle_raw_hid_relay_payload(const struct zmk_split_bt_output_relay_event *in_ev,
                                            uint8_t payload_size, uint16_t len) {
    if (in_ev->value != RAW_HID_SPLIT_CHUNK_VALUE) {
        if (payload_size == 0) {
            LOG_WRN("Raw HID relay received with empty payload");
            return len;
        }

        raise_raw_hid_received_event(
            (struct raw_hid_received_event){.data = in_ev->payload, .length = payload_size});
        return len;
    }

    if (payload_size <= RAW_HID_SPLIT_CHUNK_HEADER_SIZE) {
        LOG_WRN("Raw HID chunk too short: %u", payload_size);
        reset_raw_hid_reassembly();
        return len;
    }

    const uint8_t sequence = in_ev->payload[0];
    const uint8_t offset = in_ev->payload[1];
    const uint8_t total = in_ev->payload[2];
    const uint8_t chunk_len = payload_size - RAW_HID_SPLIT_CHUNK_HEADER_SIZE;

    if (total == 0 || total > CONFIG_RAW_HID_REPORT_SIZE || offset + chunk_len > total) {
        LOG_WRN("Invalid Raw HID chunk seq=%u offset=%u total=%u len=%u", sequence, offset,
                total, chunk_len);
        reset_raw_hid_reassembly();
        return len;
    }

    if (offset == 0) {
        reset_raw_hid_reassembly();
        raw_hid_reassembly_sequence = sequence;
        raw_hid_reassembly_total = total;
        raw_hid_reassembly_active = true;
    }

    if (!raw_hid_reassembly_active || sequence != raw_hid_reassembly_sequence ||
        total != raw_hid_reassembly_total || offset != raw_hid_reassembly_received) {
        LOG_WRN("Out-of-order Raw HID chunk seq=%u offset=%u total=%u expected=%u", sequence,
                offset, total, raw_hid_reassembly_received);
        reset_raw_hid_reassembly();
        return len;
    }

    memcpy(&raw_hid_reassembly_buf[offset],
           &in_ev->payload[RAW_HID_SPLIT_CHUNK_HEADER_SIZE], chunk_len);
    raw_hid_reassembly_received += chunk_len;

    if (raw_hid_reassembly_received == raw_hid_reassembly_total) {
        raise_raw_hid_received_event((struct raw_hid_received_event){
            .data = raw_hid_reassembly_buf, .length = raw_hid_reassembly_total});
        reset_raw_hid_reassembly();
    }

    return len;
}
#endif

K_MSGQ_DEFINE(peripheral_output_event_msgq, sizeof(struct zmk_split_output_event),
              CONFIG_ZMK_SPLIT_SPLT_PERIPHERAL_OUTPUT_QUEUE_SIZE, 4);

void peripheral_output_event_work_callback(struct k_work *work) {
    struct zmk_split_output_event ev;
    while (k_msgq_get(&peripheral_output_event_msgq, &ev, K_NO_WAIT) == 0) {
        LOG_DBG("Trigger output change: v-%d payload-%u", ev.value, ev.payload_size);

        const struct device *output_dev = ev.dev;
        if (!output_dev) {
            LOG_WRN("No output device assigned");
            continue;
        }

#if IS_ENABLED(CONFIG_ZMK_OUTPUT_BEHAVIOR_LISTENER)

        //** TODO: check if in_ev has payload bits
        //         call either api->set_value, or api->set_payload

        const struct output_generic_api *api = (const struct output_generic_api *)output_dev->api;
        if (api->set_value == NULL) {
            LOG_WRN("No enable() api assigned on device %s", output_dev->name);
            continue;
        }
        api->set_value(output_dev, ev.value);

#endif /* IS_ENABLED(CONFIG_ZMK_OUTPUT_BEHAVIOR_LISTENER) */

    }
}

K_WORK_DEFINE(peripheral_output_event_work, peripheral_output_event_work_callback);

const struct device* virtual_output_device_get_for_relay_channel(uint8_t relay_channel);

static ssize_t split_svc_update_output(struct bt_conn *conn, const struct bt_gatt_attr *attrs,
                                       const void *buf, uint16_t len, uint16_t offset,
                                       uint8_t flags) {
    uint8_t *data = attrs->user_data;

    LOG_DBG("offset %d len %d", offset, len);

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    if (len < offsetof(struct zmk_split_bt_output_relay_event, payload) ||
        len > sizeof(struct zmk_split_bt_output_relay_event)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    memcpy(data, buf, len);

    struct zmk_split_bt_output_relay_event *in_ev
            = (struct zmk_split_bt_output_relay_event *)data;
    const uint8_t payload_size =
        MIN(in_ev->payload_size,
            (uint8_t)MIN((uint16_t)ZMK_SPLIT_PERIPHERAL_OUTPUT_PAYLOAD_MAX,
                         (uint16_t)(len - offsetof(struct zmk_split_bt_output_relay_event,
                                                   payload))));

#ifdef CONFIG_RAW_HID_SPLIT_RELAY_CHANNEL
    if (in_ev->relay_channel == CONFIG_RAW_HID_SPLIT_RELAY_CHANNEL) {
        return handle_raw_hid_relay_payload(in_ev, payload_size, len);
    }
#endif

    const struct device *dev = virtual_output_device_get_for_relay_channel(in_ev->relay_channel);
    if (dev == NULL) {
        LOG_DBG("Unable to retrieve virtual device for channel: %d", in_ev->relay_channel);
        return len;
    }

    struct zmk_split_output_event ev = {0};
    ev.dev = dev;
    ev.value = in_ev->value;
    ev.payload_size = payload_size;
    memcpy(ev.payload, in_ev->payload, ev.payload_size);

    k_msgq_put(&peripheral_output_event_msgq, &ev, K_NO_WAIT);
    k_work_submit(&peripheral_output_event_work);

    return len;
}

#endif /* IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY) */

BT_GATT_SERVICE_DEFINE(
    or_split_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_OR_SERVICE_UUID)),
#if IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY)
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_OR_CHAR_OUTPUT_STATE_UUID),
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE_ENCRYPT, NULL,
                           split_svc_update_output, &split_output_run_payload),
#endif /* IS_ENABLED(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY) */
);


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

const struct device* virtual_output_device_get_for_relay_channel(uint8_t relay_channel) {
    #define OR_P_COND_CMP_RELAY_CHANNEL(n)                          \
        if (relay_channel == config_##n.relay_channel) {            \
            return config_##n.device;                               \
        }
    DT_INST_FOREACH_STATUS_OKAY(OR_P_COND_CMP_RELAY_CHANNEL)
    return NULL;
}

#else

const struct device* virtual_output_device_get_for_relay_channel(uint8_t relay_channel) {
    return NULL;
}

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
