#include <stddef.h>
#include <errno.h>
#include <string.h>

#include <raw_hid/events.h>
#include <raw_hid/split.h>
#include <zmk/event_manager.h>
#include <zmk/split/output-relay/event.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum {
    _TIME = 0xAA,
    _VOLUME = 0xAB,
    _LAYOUT = 0xAC,
    _MEDIA_ARTIST = 0xAD,
    _MEDIA_TITLE = 0xAE,
};

static uint8_t next_sequence;

static uint8_t raw_hid_logical_length(const struct raw_hid_received_event *event) {
    if (event->length == 0) {
        return 0;
    }

    switch (event->data[0]) {
    case _TIME:
        return MIN(event->length, 3);
    case _VOLUME:
    case _LAYOUT:
        return MIN(event->length, 2);
    case _MEDIA_ARTIST:
    case _MEDIA_TITLE:
        if (event->length < 2) {
            return event->length;
        }
        return MIN((uint16_t)event->length, (uint16_t)event->data[1] + 2);
    default:
        return event->length;
    }
}

static uint8_t raw_hid_split_payload_cap(void) {
    /* Default ATT MTU is 23 unless negotiated; assume 23 to keep writes safe. */
#ifdef BT_ATT_DEFAULT_LE_MTU
    const uint8_t att_payload_max = BT_ATT_DEFAULT_LE_MTU - 3; /* ATT header is 3 bytes */
#else
    const uint8_t att_payload_max = 20; /* 23 - 3 */
#endif
    const uint8_t frame_overhead = offsetof(struct zmk_split_bt_output_relay_event, payload);
    const uint8_t att_value_budget =
        att_payload_max > frame_overhead ? att_payload_max - frame_overhead : 0;

    return MIN((uint8_t)ZMK_SPLIT_PERIPHERAL_OUTPUT_PAYLOAD_MAX, att_value_budget);
}

static int forward_raw_hid_payload(const uint8_t *payload, uint8_t payload_size) {
    return zmk_split_bt_invoke_output_channel(CONFIG_RAW_HID_SPLIT_RELAY_CHANNEL, payload[0],
                                              payload, payload_size);
}

static int forward_raw_hid_chunks(const uint8_t *payload, uint8_t payload_size,
                                  uint8_t safe_payload_cap) {
    if (safe_payload_cap <= RAW_HID_SPLIT_CHUNK_HEADER_SIZE) {
        LOG_ERR("Raw HID split payload cap too small for chunk header (%u)", safe_payload_cap);
        return -EMSGSIZE;
    }

    const uint8_t chunk_data_cap = safe_payload_cap - RAW_HID_SPLIT_CHUNK_HEADER_SIZE;
    const uint8_t sequence = next_sequence++;

    for (uint8_t offset = 0; offset < payload_size; offset += chunk_data_cap) {
        uint8_t chunk_buf[ZMK_SPLIT_PERIPHERAL_OUTPUT_PAYLOAD_MAX] = {0};
        uint8_t chunk_len = MIN(chunk_data_cap, (uint8_t)(payload_size - offset));

        chunk_buf[0] = sequence;
        chunk_buf[1] = offset;
        chunk_buf[2] = payload_size;
        memcpy(&chunk_buf[RAW_HID_SPLIT_CHUNK_HEADER_SIZE], &payload[offset], chunk_len);

        int err = zmk_split_bt_invoke_output_channel(
            CONFIG_RAW_HID_SPLIT_RELAY_CHANNEL, RAW_HID_SPLIT_CHUNK_VALUE, chunk_buf,
            RAW_HID_SPLIT_CHUNK_HEADER_SIZE + chunk_len);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int raw_hid_split_forward_listener(const zmk_event_t *eh) {
    const struct raw_hid_received_event *event = as_raw_hid_received_event(eh);
    if (event == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t payload_size = raw_hid_logical_length(event);
    if (payload_size == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t safe_payload_cap = raw_hid_split_payload_cap();

    int err = payload_size <= safe_payload_cap
                  ? forward_raw_hid_payload(event->data, payload_size)
                  : forward_raw_hid_chunks(event->data, payload_size, safe_payload_cap);
    if (err) {
        LOG_ERR("Failed to forward Raw HID payload to peripheral (err %d)", err);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(raw_hid_split_forward, raw_hid_split_forward_listener);
ZMK_SUBSCRIPTION(raw_hid_split_forward, raw_hid_received_event);
