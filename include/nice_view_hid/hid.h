#pragma once

#include <zmk/event_manager.h>

#ifdef CONFIG_RAW_HID

#define NICE_VIEW_HID_TEXT_MAX_LEN 64

struct is_connected_notification {
    bool value;
};

ZMK_EVENT_DECLARE(is_connected_notification);

struct time_notification {
    uint8_t hour;
    uint8_t minute;
};

ZMK_EVENT_DECLARE(time_notification);

struct volume_notification {
    uint8_t value;
};

ZMK_EVENT_DECLARE(volume_notification);

struct media_artist_notification {
    char value[NICE_VIEW_HID_TEXT_MAX_LEN + 1];
};

ZMK_EVENT_DECLARE(media_artist_notification);

struct media_title_notification {
    char value[NICE_VIEW_HID_TEXT_MAX_LEN + 1];
};

ZMK_EVENT_DECLARE(media_title_notification);

#ifdef CONFIG_NICE_VIEW_HID_SHOW_LAYOUT
struct layout_notification {
    uint8_t value;
};

ZMK_EVENT_DECLARE(layout_notification);
#endif
#endif
