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
#define HID_DISCONNECT_TIMEOUT_MS 65000
#define HID_VOLUME_WINDOW_MS 150

static struct k_spinlock hid_state_lock;
static bool is_connected = false;
static bool raised_is_connected = false;
static int64_t disconnect_deadline_ms;

static struct time_notification last_time;
static struct media_artist_notification last_media_artist;
static struct media_title_notification last_media_title;
#ifdef CONFIG_NICE_VIEW_HID_SHOW_LAYOUT
static struct layout_notification last_layout;
#endif

static void on_disconnect_work(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(disconnect_work, on_disconnect_work);

static void on_disconnect_work(struct k_work *work) {
    ARG_UNUSED(work);

    const int64_t now = k_uptime_get();
    int64_t remaining_ms = 0;
    bool notify;
    bool value;

    k_spinlock_key_t key = k_spin_lock(&hid_state_lock);
    if (is_connected && now >= disconnect_deadline_ms) {
        is_connected = false;
    }
    value = is_connected;
    notify = value != raised_is_connected;
    if (notify) {
        raised_is_connected = value;
    }
    if (value) {
        remaining_ms = MAX(disconnect_deadline_ms - now, 0);
    }
    k_spin_unlock(&hid_state_lock, key);

    if (remaining_ms > 0) {
        (void)k_work_reschedule(&disconnect_work, K_MSEC((uint32_t)remaining_ms));
    }

    if (notify) {
        LOG_INF("raise_connection_notification: %s", value ? "true" : "false");
        raise_is_connected_notification((struct is_connected_notification){.value = value});
    }
}

static uint8_t last_hid_volume = 0;
static uint8_t last_raised_volume = 0;
static bool volume_window_active;
static int64_t volume_deadline_ms;

static void on_volume_work(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(volume_work, on_volume_work);

static void on_volume_work(struct k_work *work) {
    ARG_UNUSED(work);

    const int64_t now = k_uptime_get();
    int64_t remaining_ms;
    uint8_t value;
    bool notify;

    k_spinlock_key_t key = k_spin_lock(&hid_state_lock);
    if (!volume_window_active) {
        k_spin_unlock(&hid_state_lock, key);
        return;
    }

    value = last_hid_volume;
    notify = value != last_raised_volume;
    if (notify) {
        last_raised_volume = value;
    }
    remaining_ms = MAX(volume_deadline_ms - now, 0);
    if (remaining_ms == 0) {
        volume_window_active = false;
    }
    k_spin_unlock(&hid_state_lock, key);

    if (remaining_ms > 0) {
        (void)k_work_reschedule(&volume_work, K_MSEC((uint32_t)remaining_ms));
    }

    if (notify) {
        LOG_INF("raise_volume_notification %u", value);
        raise_volume_notification((struct volume_notification){.value = value});
    }
}

static void handle_media_field(bool is_artist, const uint8_t *data, uint8_t length) {
    if (length < 2) {
        LOG_WRN("Media packet too short: %u", length);
        return;
    }

    uint8_t declared_len = data[1];
    uint8_t available_len = length > 2 ? length - 2 : 0;
    uint8_t copy_len =
        MIN(declared_len, MIN(available_len, (uint8_t)HID_MAX_TEXT_LEN));

    if (declared_len != 0 && copy_len == 0) {
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
        k_spinlock_key_t key = k_spin_lock(&hid_state_lock);
        last_media_artist = notification;
        k_spin_unlock(&hid_state_lock, key);
        raise_media_artist_notification(notification);
    } else {
        struct media_title_notification notification = {0};
        memcpy(notification.value, data + 2, copy_len);
        k_spinlock_key_t key = k_spin_lock(&hid_state_lock);
        last_media_title = notification;
        k_spin_unlock(&hid_state_lock, key);
        raise_media_title_notification(notification);
    }
}

static void process_raw_hid_data(const uint8_t *data, uint8_t length) {
    if (data == NULL || length == 0) {
        return;
    }

    LOG_INF("display_process_raw_hid_data - received data_type %u", data[0]);

    /* Serialize connection edges on the system workqueue. A report racing the
     * timeout updates this deadline before the queued work decides what to
     * publish, so it cannot leave the display falsely disconnected. */
    const int64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&hid_state_lock);
    is_connected = true;
    disconnect_deadline_ms = now + HID_DISCONNECT_TIMEOUT_MS;
    bool connection_edge_pending = !raised_is_connected;
    k_spin_unlock(&hid_state_lock, key);
    (void)k_work_reschedule(&disconnect_work,
                            connection_edge_pending ? K_NO_WAIT
                                                    : K_MSEC(HID_DISCONNECT_TIMEOUT_MS));

    uint8_t data_type = data[0];
    switch (data_type) {
    case _TIME: {
        if (length < 3) {
            LOG_WRN("Time payload too short (%u)", length);
            break;
        }
        struct time_notification time = {.hour = data[1], .minute = data[2]};
        key = k_spin_lock(&hid_state_lock);
        last_time = time;
        k_spin_unlock(&hid_state_lock, key);
        raise_time_notification(time);
        break;
    }

    case _VOLUME: {
        if (length < 2) {
            LOG_WRN("Volume payload too short (%u)", length);
            break;
        }
        key = k_spin_lock(&hid_state_lock);
        last_hid_volume = data[1];
        bool start_volume_window = !volume_window_active;
        if (start_volume_window) {
            volume_window_active = true;
            volume_deadline_ms = now + HID_VOLUME_WINDOW_MS;
        }
        k_spin_unlock(&hid_state_lock, key);

        if (start_volume_window) {
            (void)k_work_reschedule(&volume_work, K_NO_WAIT);
        }

        break;
    }

#ifdef CONFIG_NICE_VIEW_HID_SHOW_LAYOUT
    case _LAYOUT: {
        if (length < 2) {
            LOG_WRN("Layout payload too short (%u)", length);
            break;
        }
        struct layout_notification layout = {.value = data[1]};
        key = k_spin_lock(&hid_state_lock);
        last_layout = layout;
        k_spin_unlock(&hid_state_lock, key);
        raise_layout_notification(layout);
        break;
    }
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

/* Live-state getters used by the widgets to seed their initial state. */
bool nice_view_hid_is_connected(void) {
    k_spinlock_key_t key = k_spin_lock(&hid_state_lock);
    bool value = is_connected;
    k_spin_unlock(&hid_state_lock, key);
    return value;
}

struct time_notification nice_view_hid_last_time(void) {
    k_spinlock_key_t key = k_spin_lock(&hid_state_lock);
    struct time_notification value = last_time;
    k_spin_unlock(&hid_state_lock, key);
    return value;
}

struct volume_notification nice_view_hid_last_volume(void) {
    k_spinlock_key_t key = k_spin_lock(&hid_state_lock);
    struct volume_notification value = {.value = last_hid_volume};
    k_spin_unlock(&hid_state_lock, key);
    return value;
}

struct media_artist_notification nice_view_hid_last_media_artist(void) {
    k_spinlock_key_t key = k_spin_lock(&hid_state_lock);
    struct media_artist_notification value = last_media_artist;
    k_spin_unlock(&hid_state_lock, key);
    return value;
}

struct media_title_notification nice_view_hid_last_media_title(void) {
    k_spinlock_key_t key = k_spin_lock(&hid_state_lock);
    struct media_title_notification value = last_media_title;
    k_spin_unlock(&hid_state_lock, key);
    return value;
}

#ifdef CONFIG_NICE_VIEW_HID_SHOW_LAYOUT
struct layout_notification nice_view_hid_last_layout(void) {
    k_spinlock_key_t key = k_spin_lock(&hid_state_lock);
    struct layout_notification value = last_layout;
    k_spin_unlock(&hid_state_lock, key);
    return value;
}
#endif

ZMK_LISTENER(process_raw_hid_event, raw_hid_received_event_listener);
ZMK_SUBSCRIPTION(process_raw_hid_event, raw_hid_received_event);
