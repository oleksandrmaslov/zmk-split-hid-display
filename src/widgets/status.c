/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>

#include "status.h"

#ifdef CONFIG_RAW_HID
#include <nice_view_hid/hid.h>
#endif

/*
 * Layout coordinates are in the 68 × 160 portrait design space.
 * They map 1:1 to references/Connected.svg / references/No RAWHID.svg, and
 * the final 160 × 68 image is produced by rotate_portrait_canvas().
 */

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    uint8_t active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
    bool profile_connected[NICE_VIEW_HID_PROFILE_COUNT];
    bool profile_bonded[NICE_VIEW_HID_PROFILE_COUNT];
};

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

#define OUTPUT_ICON_X 52
#define OUTPUT_ICON_Y 3
#define OUTPUT_PROFILE_X 36
#define OUTPUT_PROFILE_Y 4
#define OUTPUT_PROFILE_WIDTH 14

static void fill_canvas(lv_obj_t *canvas) {
    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);
}

static void draw_profile_dot(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, uint8_t index,
                             const struct status_state *state) {
    bool is_active = (index == state->active_profile_index);

    if (is_active && state->active_profile_connected) {
        draw_profile_selected(canvas, x, y);
    } else if (is_active && !state->active_profile_bonded) {
        /* user is on this profile slot but it has no bonded peer yet — pairing mode */
        draw_profile_selected_free(canvas, x, y);
    } else if (state->profile_bonded[index]) {
        draw_profile_bonded(canvas, x, y);
    } else {
        draw_profile_free(canvas, x, y);
    }
}

/* BT / USB indicator with the BLE profile number aligned left of the BT icon. */
static void draw_output_indicator(lv_obj_t *canvas, const struct status_state *state) {
    if (state->selected_endpoint.transport == ZMK_TRANSPORT_USB) {
        draw_elemental_usb_logo(canvas, 46, 7);
        return;
    }

    if (state->active_profile_bonded) {
        if (state->active_profile_connected) {
            draw_elemental_bluetooth_logo(canvas, OUTPUT_ICON_X, OUTPUT_ICON_Y);
        } else {
            draw_elemental_bluetooth_logo_outlined(canvas, OUTPUT_ICON_X, OUTPUT_ICON_Y);
        }
    } else {
        draw_elemental_bluetooth_searching(canvas, OUTPUT_ICON_X, OUTPUT_ICON_Y);
    }

    /* Profile number (1..5), right-aligned in the slot before the BT icon. */
    lv_draw_label_dsc_t num_dsc;
    init_label_dsc(&num_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);
    char digit[2] = {(char)('1' + MIN(state->active_profile_index,
                                      (uint8_t)(NICE_VIEW_HID_PROFILE_COUNT - 1))),
                     '\0'};
    canvas_draw_text(canvas, OUTPUT_PROFILE_X, OUTPUT_PROFILE_Y, OUTPUT_PROFILE_WIDTH, &num_dsc,
                     digit);
}

/* ---------- Layout name (from comma-separated CONFIG_NICE_VIEW_HID_LAYOUTS) ---------- */

static void get_layout_text(uint8_t layout_index, char *out, size_t out_size) {
#if IS_ENABLED(CONFIG_NICE_VIEW_HID_SHOW_LAYOUT)
    char buf[sizeof(CONFIG_NICE_VIEW_HID_LAYOUTS)];
    strncpy(buf, CONFIG_NICE_VIEW_HID_LAYOUTS, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';

    char *layout = strtok(buf, ",");
    size_t i = 0;
    while (layout != NULL && i < layout_index) {
        i++;
        layout = strtok(NULL, ",");
    }

    if (layout != NULL) {
        snprintf(out, out_size, "%s", layout);
    } else {
        snprintf(out, out_size, "%u", layout_index);
    }

    /* upper-case for display */
    for (size_t j = 0; out[j] != '\0' && j + 1 < out_size; j++) {
        if (out[j] >= 'a' && out[j] <= 'z') {
            out[j] = out[j] - 'a' + 'A';
        }
    }
#else
    ARG_UNUSED(layout_index);
    if (out_size > 0) out[0] = '\0';
#endif
}

/* ---------- Top-level draw ---------- */

static void draw_status(struct zmk_widget_status *widget) {
    lv_obj_t *canvas = widget->portrait_canvas;
    const struct status_state *state = &widget->state;

    fill_canvas(canvas);

    /* Battery is drawn horizontally in the portrait layout at the top-left. */
    draw_battery(canvas, state);
    /* BT/USB and profile digit share the top-right header slot. */
    draw_output_indicator(canvas, state);

    /* middle band — y ≈ 28..92 */
    lv_draw_label_dsc_t label_18;
    init_label_dsc(&label_18, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t label_16_left;
    init_label_dsc(&label_16_left, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t label_14_left;
    init_label_dsc(&label_14_left, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);

#if IS_ENABLED(CONFIG_RAW_HID)
    if (state->is_connected) {
        /* time @ (4, 31) Montserrat-18 */
        char time[8] = {};
        snprintf(time, sizeof(time), "%02u:%02u", state->hour, state->minute);
        canvas_draw_text(canvas, 4, 31, 60, &label_18, time);

        /* language: globe icon @ (4, 60) + layout text @ (18, 60) */
        char layout[10] = {};
        get_layout_text(state->layout, layout, sizeof(layout));
        draw_language_icon(canvas, 4, 62);
        canvas_draw_text(canvas, 18, 60, 46, &label_14_left, layout);

        /* volume: composite speaker icon @ (4, 78) + value @ (22, 76) */
        char volume[5] = {};
        snprintf(volume, sizeof(volume), "%u%%", state->volume);
        draw_volume_icon(canvas, 4, 80, state->volume);
        canvas_draw_text(canvas, 18, 76, 42, &label_14_left, volume);
    } else {
        /* left-aligned "Connect / RAW HID" prompt — replaces middle band */
        canvas_draw_text(canvas, 4, 30, 68, &label_14_left, "Connect");
        canvas_draw_text(canvas, 4, 50, 60, &label_16_left, "RAW HID");
    }
#else
    ARG_UNUSED(label_18);
    ARG_UNUSED(label_16_left);
    ARG_UNUSED(label_14_left);
#endif

    /* profile section */
    canvas_draw_text(canvas, 4, 98, 60, &label_14_left, "Profile");
    static const lv_coord_t profile_x[NICE_VIEW_HID_PROFILE_COUNT] = {4, 16, 28, 40, 52};
    for (uint8_t i = 0; i < NICE_VIEW_HID_PROFILE_COUNT; i++) {
        draw_profile_dot(canvas, profile_x[i], 115, i, state);
    }

    /* layer section */
    canvas_draw_text(canvas, 4, 127, 60, &label_14_left, "Layer");
    if (state->layer_label == NULL || strlen(state->layer_label) == 0) {
        char text[12] = {};
        snprintf(text, sizeof(text), "Base %u", state->layer_index);
        canvas_draw_text(canvas, 4, 143, 60, &label_16_left, text);
    } else {
        canvas_draw_text(canvas, 4, 143, 60, &label_16_left, state->layer_label);
    }

    rotate_portrait_canvas(widget->portrait_cbuf, widget->screen_cbuf);
    lv_obj_invalidate(widget->screen_canvas);
}

/* ---------- ZMK event listeners ---------- */

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif
    widget->state.battery = state.level;
    draw_status(widget);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif

static void set_output_status(struct zmk_widget_status *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;
    memcpy(widget->state.profile_connected, state->profile_connected,
           sizeof(widget->state.profile_connected));
    memcpy(widget->state.profile_bonded, state->profile_bonded,
           sizeof(widget->state.profile_bonded));
    draw_status(widget);
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    struct output_status_state state = {
        .selected_endpoint = zmk_endpoint_get_selected(),
    };

#if defined(CONFIG_ZMK_BLE)
    state.active_profile_index = zmk_ble_active_profile_index();
    state.active_profile_connected = zmk_ble_active_profile_is_connected();
    state.active_profile_bonded = !zmk_ble_active_profile_is_open();

    for (uint8_t i = 0; i < NICE_VIEW_HID_PROFILE_COUNT; i++) {
        state.profile_connected[i] = zmk_ble_profile_is_connected(i);
        state.profile_bonded[i] = !zmk_ble_profile_is_open(i);
    }
#endif

    return state;
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

static void set_layer_status(struct zmk_widget_status *widget, struct layer_status_state state) {
    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;
    draw_status(widget);
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index, .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index))};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)

ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

#ifdef CONFIG_RAW_HID

static void copy_text_field(char *dst, const char *src) {
    strncpy(dst, src, NICE_VIEW_HID_TEXT_MAX_LEN);
    dst[NICE_VIEW_HID_TEXT_MAX_LEN] = '\0';
}

static struct is_connected_notification get_is_hid_connected(const zmk_event_t *eh) {
    struct is_connected_notification *n = as_is_connected_notification(eh);
    return n ? *n : (struct is_connected_notification){.value = false};
}

static void is_hid_connected_update_cb(struct is_connected_notification is_connected) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.is_connected = is_connected.value;
        if (!is_connected.value) {
            widget->state.media_artist[0] = '\0';
            widget->state.media_title[0] = '\0';
        }
        draw_status(widget);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_is_connected, struct is_connected_notification,
                            is_hid_connected_update_cb, get_is_hid_connected)
ZMK_SUBSCRIPTION(widget_is_connected, is_connected_notification);

static struct time_notification get_time(const zmk_event_t *eh) {
    struct time_notification *n = as_time_notification(eh);
    return n ? *n : (struct time_notification){.hour = 0, .minute = 0};
}

static void time_update_cb(struct time_notification time) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.hour = time.hour;
        widget->state.minute = time.minute;
        draw_status(widget);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_time, struct time_notification, time_update_cb, get_time)
ZMK_SUBSCRIPTION(widget_time, time_notification);

static struct volume_notification get_volume(const zmk_event_t *eh) {
    struct volume_notification *n = as_volume_notification(eh);
    return n ? *n : (struct volume_notification){.value = 0};
}

static void volume_update_cb(struct volume_notification volume) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.volume = volume.value;
        draw_status(widget);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_volume, struct volume_notification, volume_update_cb, get_volume)
ZMK_SUBSCRIPTION(widget_volume, volume_notification);

#ifdef CONFIG_NICE_VIEW_HID_SHOW_LAYOUT
static struct layout_notification get_layout(const zmk_event_t *eh) {
    struct layout_notification *n = as_layout_notification(eh);
    return n ? *n : (struct layout_notification){.value = 0};
}

static void layout_update_cb(struct layout_notification layout) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.layout = layout.value;
        draw_status(widget);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layout, struct layout_notification, layout_update_cb, get_layout)
ZMK_SUBSCRIPTION(widget_layout, layout_notification);
#endif

static struct media_title_notification get_media_title(const zmk_event_t *eh) {
    struct media_title_notification *n = as_media_title_notification(eh);
    return n ? *n : (struct media_title_notification){0};
}

static void media_title_update_cb(struct media_title_notification title) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        copy_text_field(widget->state.media_title, title.value);
        /* central does not draw media — just keep state in sync. */
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_media_title, struct media_title_notification,
                            media_title_update_cb, get_media_title)
ZMK_SUBSCRIPTION(widget_media_title, media_title_notification);

static struct media_artist_notification get_media_artist(const zmk_event_t *eh) {
    struct media_artist_notification *n = as_media_artist_notification(eh);
    return n ? *n : (struct media_artist_notification){0};
}

static void media_artist_update_cb(struct media_artist_notification artist) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        copy_text_field(widget->state.media_artist, artist.value);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_media_artist, struct media_artist_notification,
                            media_artist_update_cb, get_media_artist)
ZMK_SUBSCRIPTION(widget_media_artist, media_artist_notification);

#endif /* CONFIG_RAW_HID */

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, NICE_VIEW_HID_SCREEN_WIDTH, NICE_VIEW_HID_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(widget->obj, LVGL_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(widget->obj, 0, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);
    memset(&widget->state, 0, sizeof(widget->state));

    /* offscreen drawing canvas (68x160 portrait) */
    widget->portrait_canvas = lv_canvas_create(widget->obj);
    lv_obj_set_pos(widget->portrait_canvas, -NICE_VIEW_HID_PORTRAIT_WIDTH - 1, 0);
    lv_canvas_set_buffer(widget->portrait_canvas, widget->portrait_cbuf,
                         NICE_VIEW_HID_PORTRAIT_WIDTH, NICE_VIEW_HID_PORTRAIT_HEIGHT,
                         CANVAS_COLOR_FORMAT);

    /* visible canvas (160x68) — receives the rotated buffer */
    widget->screen_canvas = lv_canvas_create(widget->obj);
    lv_obj_align(widget->screen_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(widget->screen_canvas, widget->screen_cbuf, NICE_VIEW_HID_SCREEN_WIDTH,
                         NICE_VIEW_HID_SCREEN_HEIGHT, CANVAS_COLOR_FORMAT);

    /*
     * Paint a clean background up-front so we don't show partial state.
     * The widget_*_init() calls below each schedule an initial state fetch
     * that triggers draw_status(); ordering them after a clean canvas
     * means the first fully-populated draw is what the user sees, instead
     * of icons appearing before text labels.
     */
    fill_canvas(widget->portrait_canvas);
    rotate_portrait_canvas(widget->portrait_cbuf, widget->screen_cbuf);
    lv_obj_invalidate(widget->screen_canvas);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_output_status_init();
    widget_layer_status_init();
#ifdef CONFIG_RAW_HID
    widget_is_connected_init();
    widget_time_init();
    widget_volume_init();
#ifdef CONFIG_NICE_VIEW_HID_SHOW_LAYOUT
    widget_layout_init();
#endif
    widget_media_title_init();
    widget_media_artist_init();
#endif

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
