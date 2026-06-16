#include <nice_view_hid/hid.h>
#include <raw_hid/events.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(is_connected_notification);
ZMK_EVENT_IMPL(time_notification);
ZMK_EVENT_IMPL(volume_notification);
ZMK_EVENT_IMPL(media_artist_notification);
ZMK_EVENT_IMPL(media_title_notification);
#ifdef CONFIG_NICE_VIEW_HID_SHOW_LAYOUT
ZMK_EVENT_IMPL(layout_notification);
#endif

typedef enum {
    _TIME = 0xAA, // random value, must match companion app
    _VOLUME = 0xAB,
    _LAYOUT = 0xAC,
    _MEDIA_ARTIST = 0xAD,
    _MEDIA_TITLE = 0xAE,
} hid_data_type;

#define HID_MAX_TEXT_LEN NICE_VIEW_HID_TEXT_MAX_LEN

static bool is_connected = false;

static void on_disconnect_timer(struct k_timer *dummy) {
    LOG_INF("raise_connection_notification: false");
    is_connected = false;
    raise_is_connected_notification((struct is_connected_notification){.value = false});
}

K_TIMER_DEFINE(disconnect_timer, on_disconnect_timer, NULL);

static uint8_t last_hid_volume = 0;
static uint8_t last_raised_volume = 0;

static void on_volume_timer(struct k_timer *dummy) {
    // prevent raising event with the same value multiple times
    if (last_raised_volume != last_hid_volume) {
        last_raised_volume = last_hid_volume;
        LOG_INF("raise_volume_notification %i", last_hid_volume);
        raise_volume_notification((struct volume_notification){.value = last_hid_volume});
    }
}

K_TIMER_DEFINE(volume_timer, on_volume_timer, NULL);

static void handle_media_field(bool is_artist, uint8_t *data, uint8_t length) {
    if (length < 2) {
        LOG_WRN("Media packet too short: %u", length);
        return;
    }

    uint8_t declared_len = data[1];
    uint8_t available_len = length > 2 ? length - 2 : 0;
    uint8_t copy_len =
        MIN(declared_len, MIN(available_len, (uint8_t)HID_MAX_TEXT_LEN));

    if (copy_len == 0) {
        LOG_WRN("Received empty media value, declared=%u available=%u", declared_len,
                available_len);
        return;
    }

    if (declared_len > copy_len) {
        LOG_WRN("Truncating media value from %u to %u bytes", declared_len, copy_len);
    }

    if (is_artist) {
        struct media_artist_notification notification = {0};
        memcpy(notification.value, data + 2, copy_len);
        raise_media_artist_notification(notification);
    } else {
        struct media_title_notification notification = {0};
        memcpy(notification.value, data + 2, copy_len);
        raise_media_title_notification(notification);
    }
}

static void process_raw_hid_data(uint8_t *data, uint8_t length) {
    if (length == 0) {
        return;
    }

    LOG_INF("display_process_raw_hid_data - received data_type %u", data[0]);

    // raise disconnect notification after 65 seconds of inactivity
    k_timer_start(&disconnect_timer, K_SECONDS(65), K_NO_WAIT);
    if (!is_connected) {
        LOG_INF("raise_connection_notification: true");
        is_connected = true;
        raise_is_connected_notification((struct is_connected_notification){.value = true});
    }

    uint8_t data_type = data[0];
    switch (data_type) {
    case _TIME:
        if (length < 3) {
            LOG_WRN("Time payload too short (%u)", length);
            break;
        }
        raise_time_notification((struct time_notification){.hour = data[1], .minute = data[2]});
        break;

    case _VOLUME:
        if (length < 2) {
            LOG_WRN("Volume payload too short (%u)", length);
            break;
        }
        last_hid_volume = data[1];

        // debounce volume change events
        if (k_timer_status_get(&volume_timer) > 0 || k_timer_remaining_get(&volume_timer) == 0) {
            k_timer_start(&volume_timer, K_MSEC(150), K_NO_WAIT);
            on_volume_timer(&volume_timer);
        }

        break;

#ifdef CONFIG_NICE_VIEW_HID_SHOW_LAYOUT
    case _LAYOUT:
        if (length < 2) {
            LOG_WRN("Layout payload too short (%u)", length);
            break;
        }
        raise_layout_notification((struct layout_notification){.value = data[1]});
        break;
#endif
    case _MEDIA_ARTIST:
        handle_media_field(true, data, length);
        break;
    case _MEDIA_TITLE:
        handle_media_field(false, data, length);
        break;
    }
}

static int raw_hid_received_event_listener(const zmk_event_t *eh) {
    struct raw_hid_received_event *event = as_raw_hid_received_event(eh);
    if (event) {
        process_raw_hid_data(event->data, event->length);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(process_raw_hid_event, raw_hid_received_event_listener);
ZMK_SUBSCRIPTION(process_raw_hid_event, raw_hid_received_event);
