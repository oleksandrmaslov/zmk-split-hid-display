/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/activity.h>
#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/usb.h>

#include "peripheral_status.h"

/*
 * Layout: 68 × 160 portrait canvas → rotated to 160 × 68 final image.
 * Mirrors references/Peripheral.svg pixel-for-pixel.
 *
 * Header (always):  battery (4, 6), BT (52, 3)
 * Music block:
 *   - artist  text @ (4, 29..156),  Montserrat-14, rotated 900 (sideways, reads upward)
 *   - title   text @ (21, 29..156), Montserrat-18, rotated 900
 *   - play triangle @ (58, 30) — drawn as a small chevron (5×6) when media is playing
 *   - "Playing"/"Offline" status @ (54, 39..) Montserrat-14 rotated 900
 *
 * Marquee: when title or artist exceeds the available 127 px axial run, advance a
 * character-window every CONFIG_NICE_VIEW_HID_MEDIA_SCROLL_INTERVAL_MS ms. The text
 * loops continuously — its start trails its end by a CONFIG_NICE_VIEW_HID_MEDIA_SCROLL_GAP_CHARS
 * wide blank separator, so the wrap point is easy to read and the line never blanks.
 * Scrolling is paused when battery drops below CONFIG_NICE_VIEW_HID_MEDIA_SCROLL_MIN_BATTERY
 * (and not charging) — that's the battery-friendly knob.
 *
 * The marquee also rewinds to the start and stops advancing whenever the keyboard
 * goes idle (ZMK pauses display refreshes then to save power). That keeps the
 * persistent LCD resting on a clean start-of-text frame instead of freezing at a
 * random offset, and typing resumes the scroll from the beginning.
 */

#define MEDIA_AXIAL_START 29
#define MEDIA_AXIAL_END 156
#define MEDIA_AXIAL_LENGTH (MEDIA_AXIAL_END - MEDIA_AXIAL_START)

/* Upper bound on the loop separator, used to size the marquee compose buffer. */
#define MEDIA_SCROLL_GAP_MAX 32

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct peripheral_status_state {
    bool connected;
};

#if IS_ENABLED(CONFIG_RAW_HID) && IS_ENABLED(CONFIG_NICE_VIEW_HID_MEDIA_SCROLL)
static lv_timer_t *media_scroll_timer;
static uint16_t media_scroll_step;
#endif

static void fill_canvas(lv_obj_t *canvas) {
    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);
}

static bool is_text_pixel(lv_color32_t px) {
    const uint8_t fg = lv_color_luminance(LVGL_FOREGROUND);
    const uint8_t bg = lv_color_luminance(LVGL_BACKGROUND);
    const uint8_t threshold = ((uint16_t)fg + bg) / 2;
    const uint8_t value = px.red;

    return fg > bg ? value > threshold : value < threshold;
}

static void draw_sideways_text(struct zmk_widget_status *widget, lv_coord_t x, lv_coord_t y,
                               lv_coord_t axial_max, lv_color_t color,
                               lv_draw_label_dsc_t *dsc, const char *txt) {
    if (txt == NULL || txt[0] == '\0' || dsc->font == NULL) {
        return;
    }

    fill_canvas(widget->text_canvas);

    canvas_draw_text(widget->text_canvas, 0, 0, NICE_VIEW_HID_TEXT_CANVAS_WIDTH, dsc, txt);

    const lv_coord_t copy_w = MIN(axial_max, NICE_VIEW_HID_TEXT_CANVAS_WIDTH);
    const lv_coord_t line_height =
        MIN((lv_coord_t)lv_font_get_line_height(dsc->font), NICE_VIEW_HID_TEXT_CANVAS_HEIGHT);
    if (copy_w <= 0 || line_height <= 0) {
        return;
    }

    for (lv_coord_t sy = 0; sy < line_height; sy++) {
        for (lv_coord_t sx = 0; sx < copy_w; sx++) {
            if (!is_text_pixel(lv_canvas_get_px(widget->text_canvas, sx, sy))) {
                continue;
            }

            const lv_coord_t dx = x + (line_height - 1 - sy);
            const lv_coord_t dy = y + sx;
            if (dx >= 0 && dx < NICE_VIEW_HID_PORTRAIT_WIDTH && dy >= 0 &&
                dy < NICE_VIEW_HID_PORTRAIT_HEIGHT) {
                lv_canvas_set_px(widget->portrait_canvas, dx, dy, color, LV_OPA_COVER);
            }
        }
    }
}

static void draw_play_icon(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    /* small play chevron, 5×6 — matches mdi:play sketch in references/Peripheral.svg */
    lv_draw_rect_dsc_t fg;
    init_rect_dsc(&fg, LVGL_FOREGROUND);
    static const uint8_t row_widths[] = {7, 5, 5, 3, 3, 1};
    static const uint8_t row_offsets[] = {0, 1, 1, 2, 2, 3};
    for (uint8_t i = 0; i < ARRAY_SIZE(row_widths); i++) {
        canvas_draw_rect(canvas, x + row_offsets[i], y + i, row_widths[i], 1, &fg);
    }
}

static void draw_header(lv_obj_t *canvas, const struct status_state *state) {
    draw_battery(canvas, state);

    if (state->connected) {
        draw_elemental_bluetooth_logo(canvas, 52, 3);
    } else {
        draw_elemental_bluetooth_logo_outlined(canvas, 52, 3);
    }
}

static const char *fallback_title(const struct status_state *state) {
#if IS_ENABLED(CONFIG_RAW_HID)
    if (!state->connected) return "Waiting link";
    if (!state->is_connected) return "Connect RAW HID";
    return state->media_title[0] ? state->media_title : "Now playing";
#else
    return state->connected ? "Connected" : "Disconnected";
#endif
}

static const char *fallback_artist(const struct status_state *state) {
#if IS_ENABLED(CONFIG_RAW_HID)
    if (!state->connected) return "Split offline";
    if (!state->is_connected) return "Waiting host";
    return state->media_artist[0] ? state->media_artist : " ";
#else
    ARG_UNUSED(state);
    return "";
#endif
}

#if IS_ENABLED(CONFIG_NICE_VIEW_HID_MEDIA_SCROLL)
/*
 * Char-level marquee: every tick, advance a UTF-8-aware character offset into
 * the conceptually endless string "<text><gap><text><gap>…". Very long titles
 * slide across the visible axial run and wrap around seamlessly. When the text
 * already fits, we render it statically at offset 0 (no animation cost).
 */
static size_t utf8_char_advance(const char *s) {
    if (s == NULL || *s == '\0') return 0;
    uint8_t b = (uint8_t)*s;
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1; /* invalid — advance by 1 to make progress */
}

static size_t utf8_strlen(const char *s) {
    size_t n = 0;
    while (s != NULL && *s) {
        s += utf8_char_advance(s);
        n++;
    }
    return n;
}

static const char *utf8_advance_chars(const char *s, size_t chars) {
    while (chars > 0 && s != NULL && *s) {
        s += utf8_char_advance(s);
        chars--;
    }
    return s;
}

static lv_coord_t measure_text_width(const char *txt, const lv_font_t *font) {
    lv_point_t size;
    lv_text_get_size(&size, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}
#endif

static bool scroll_allowed(const struct status_state *state) {
#if IS_ENABLED(CONFIG_NICE_VIEW_HID_MEDIA_SCROLL)
    if (state->battery >= CONFIG_NICE_VIEW_HID_MEDIA_SCROLL_MIN_BATTERY) return true;
    return state->charging;
#else
    ARG_UNUSED(state);
    return false;
#endif
}

static void draw_marquee_text(struct zmk_widget_status *widget, lv_coord_t x, lv_coord_t y,
                              lv_coord_t axial_max, const lv_font_t *font, lv_color_t color,
                              const char *txt, uint16_t step, bool may_scroll) {
    lv_draw_label_dsc_t dsc;
    init_label_dsc(&dsc, color, font, LV_TEXT_ALIGN_LEFT);
    /*
     * LV_TEXT_FLAG_EXPAND tells LVGL to ignore max_w when laying out text,
     * which is what we want for sideways media labels — without it, "Bohemian
     * Rhapsody" wraps onto a second line that LVGL renders as a side-by-side
     * column after rotation, giving the "narrow text field" artifact.
     */
    dsc.flag |= LV_TEXT_FLAG_EXPAND;

#if IS_ENABLED(CONFIG_NICE_VIEW_HID_MEDIA_SCROLL)
    lv_coord_t full_w = measure_text_width(txt, font);
    if (!may_scroll || full_w <= axial_max) {
        draw_sideways_text(widget, x, y, axial_max, color, &dsc, txt);
        return;
    }

    size_t total_chars = utf8_strlen(txt);
    if (total_chars == 0) {
        return;
    }

    /*
     * Materialise one period of the looping ticker — "<text><gap><text>" — and
     * slide a character window across it. Two copies plus the gap guarantee the
     * window is always backed by enough glyphs to fill the axial run, so the
     * line never goes blank: as the tail scrolls off, the wrapped-around head is
     * already trailing it behind the blank gap separator.
     */
    size_t tlen = strlen(txt);
    if (tlen > NICE_VIEW_HID_TEXT_MAX_LEN) {
        tlen = NICE_VIEW_HID_TEXT_MAX_LEN;
    }
    const size_t gap_chars =
        MIN((size_t)CONFIG_NICE_VIEW_HID_MEDIA_SCROLL_GAP_CHARS, (size_t)MEDIA_SCROLL_GAP_MAX);

    static char loop_buf[2 * NICE_VIEW_HID_TEXT_MAX_LEN + MEDIA_SCROLL_GAP_MAX + 1];
    size_t pos = 0;
    memcpy(loop_buf + pos, txt, tlen);
    pos += tlen;
    memset(loop_buf + pos, ' ', gap_chars);
    pos += gap_chars;
    memcpy(loop_buf + pos, txt, tlen);
    pos += tlen;
    loop_buf[pos] = '\0';

    const size_t start_pause = CONFIG_NICE_VIEW_HID_MEDIA_SCROLL_START_PAUSE_STEPS;
    const size_t loop_chars = total_chars + gap_chars;
    const size_t cycle = start_pause + loop_chars;
    size_t phase = step % cycle;
    size_t skip = (phase > start_pause) ? (phase - start_pause) : 0;

    const char *windowed = utf8_advance_chars(loop_buf, skip);
    draw_sideways_text(widget, x, y, axial_max, color, &dsc, windowed);
#else
    ARG_UNUSED(step);
    ARG_UNUSED(may_scroll);
    /* Static mode: rely on canvas clipping rather than wrapping. */
    draw_sideways_text(widget, x, y, axial_max, color, &dsc, txt);
#endif
}

static void draw_media(struct zmk_widget_status *widget, const struct status_state *state,
                       uint16_t step) {
    const char *title = fallback_title(state);
    const char *artist = fallback_artist(state);

    bool may_scroll = scroll_allowed(state);

    /* play indicator + status (always show — fall back to "Offline" when the link is down) */
    draw_play_icon(widget->portrait_canvas, 58, 30);
    lv_draw_label_dsc_t status_dsc;
    init_label_dsc(&status_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);
    status_dsc.flag |= LV_TEXT_FLAG_EXPAND;
    draw_sideways_text(widget, 54, 39, MEDIA_AXIAL_LENGTH, LVGL_FOREGROUND, &status_dsc,
                       state->connected ? "Playing" : "Offline");

    draw_marquee_text(widget, 4, MEDIA_AXIAL_START, MEDIA_AXIAL_LENGTH, &lv_font_montserrat_14,
                      LVGL_FOREGROUND, artist, step, may_scroll);
    draw_marquee_text(widget, 21, MEDIA_AXIAL_START, MEDIA_AXIAL_LENGTH, &lv_font_montserrat_18,
                      LVGL_FOREGROUND, title, step, may_scroll);
}

static void redraw_widget(struct zmk_widget_status *widget) {
    fill_canvas(widget->portrait_canvas);
    draw_header(widget->portrait_canvas, &widget->state);
#if IS_ENABLED(CONFIG_RAW_HID) && IS_ENABLED(CONFIG_NICE_VIEW_HID_MEDIA_SCROLL)
    draw_media(widget, &widget->state, media_scroll_step);
#else
    draw_media(widget, &widget->state, 0);
#endif

    rotate_portrait_canvas(widget->portrait_cbuf, widget->screen_cbuf);
    lv_obj_invalidate(widget->screen_canvas);
}

#if IS_ENABLED(CONFIG_RAW_HID) && IS_ENABLED(CONFIG_NICE_VIEW_HID_MEDIA_SCROLL)
static bool any_widget_needs_scroll(void) {
    struct zmk_widget_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) {
        if (!w->state.connected || !w->state.is_connected) continue;
        if (!scroll_allowed(&w->state)) continue;
        if (w->state.media_title[0] == '\0' && w->state.media_artist[0] == '\0') continue;
        /* either field overflowing? */
        lv_coord_t t = measure_text_width(
            w->state.media_title[0] ? w->state.media_title : "Now playing",
            &lv_font_montserrat_18);
        lv_coord_t a = measure_text_width(
            w->state.media_artist[0] ? w->state.media_artist : " ", &lv_font_montserrat_14);
        if (t > MEDIA_AXIAL_LENGTH || a > MEDIA_AXIAL_LENGTH) return true;
    }
    return false;
}

static void media_scroll_tick(lv_timer_t *t) {
    ARG_UNUSED(t);
    /*
     * Hold at the start while idle: the activity listener already rewound the
     * step and repainted, so here we just avoid advancing (and the per-tick
     * repaint that goes with it) until the keyboard is active again.
     */
    if (zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE || !any_widget_needs_scroll()) {
        media_scroll_step = 0;
        return;
    }
    media_scroll_step++;
    struct zmk_widget_status *w;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, w, node) { redraw_widget(w); }
}

/*
 * Rewind the marquee to the start on every activity-state transition. Going idle
 * leaves the frozen frame at the beginning of the text (rather than mid-scroll),
 * and becoming active again restarts the scroll cleanly from the start.
 */
struct activity_status_state {
    enum zmk_activity_state activity;
};

static struct activity_status_state activity_status_get_state(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    return (struct activity_status_state){
        .activity = (ev != NULL) ? ev->state : zmk_activity_get_state(),
    };
}

static void activity_status_update_cb(struct activity_status_state state) {
    ARG_UNUSED(state);
    media_scroll_step = 0;
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { redraw_widget(widget); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_activity_status, struct activity_status_state,
                            activity_status_update_cb, activity_status_get_state)
ZMK_SUBSCRIPTION(widget_activity_status, zmk_activity_state_changed);
#endif

static void copy_text_field(char *dst, const char *src) {
#if IS_ENABLED(CONFIG_RAW_HID)
    strncpy(dst, src, NICE_VIEW_HID_TEXT_MAX_LEN);
    dst[NICE_VIEW_HID_TEXT_MAX_LEN] = '\0';
#else
    ARG_UNUSED(dst);
    ARG_UNUSED(src);
#endif
}

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif
    widget->state.battery = state.level;
    redraw_widget(widget);
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

static struct peripheral_status_state get_state(const zmk_event_t *_eh) {
    return (struct peripheral_status_state){.connected = zmk_split_bt_peripheral_is_connected()};
}

static void set_connection_status(struct zmk_widget_status *widget,
                                  struct peripheral_status_state state) {
    widget->state.connected = state.connected;
    redraw_widget(widget);
}

static void output_status_update_cb(struct peripheral_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_connection_status(widget, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_status, struct peripheral_status_state,
                            output_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_peripheral_status, zmk_split_peripheral_status_changed);

#ifdef CONFIG_RAW_HID

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
        redraw_widget(widget);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_is_connected, struct is_connected_notification,
                            is_hid_connected_update_cb, get_is_hid_connected)
ZMK_SUBSCRIPTION(widget_is_connected, is_connected_notification);

static struct media_title_notification get_media_title(const zmk_event_t *eh) {
    struct media_title_notification *n = as_media_title_notification(eh);
    return n ? *n : (struct media_title_notification){0};
}

static void media_title_update_cb(struct media_title_notification title) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        copy_text_field(widget->state.media_title, title.value);
#if IS_ENABLED(CONFIG_NICE_VIEW_HID_MEDIA_SCROLL)
        media_scroll_step = 0;
#endif
        redraw_widget(widget);
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
#if IS_ENABLED(CONFIG_NICE_VIEW_HID_MEDIA_SCROLL)
        media_scroll_step = 0;
#endif
        redraw_widget(widget);
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

    widget->portrait_canvas = lv_canvas_create(widget->obj);
    lv_obj_set_pos(widget->portrait_canvas, -NICE_VIEW_HID_PORTRAIT_WIDTH - 1, 0);
    lv_canvas_set_buffer(widget->portrait_canvas, widget->portrait_cbuf,
                         NICE_VIEW_HID_PORTRAIT_WIDTH, NICE_VIEW_HID_PORTRAIT_HEIGHT,
                         CANVAS_COLOR_FORMAT);

    widget->text_canvas = lv_canvas_create(widget->obj);
    lv_obj_set_pos(widget->text_canvas, -NICE_VIEW_HID_TEXT_CANVAS_WIDTH - 1, 0);
    lv_canvas_set_buffer(widget->text_canvas, widget->text_cbuf, NICE_VIEW_HID_TEXT_CANVAS_WIDTH,
                         NICE_VIEW_HID_TEXT_CANVAS_HEIGHT, CANVAS_COLOR_FORMAT);

    widget->screen_canvas = lv_canvas_create(widget->obj);
    lv_obj_align(widget->screen_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(widget->screen_canvas, widget->screen_cbuf, NICE_VIEW_HID_SCREEN_WIDTH,
                         NICE_VIEW_HID_SCREEN_HEIGHT, CANVAS_COLOR_FORMAT);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_peripheral_status_init();
#ifdef CONFIG_RAW_HID
    widget_is_connected_init();
    widget_media_title_init();
    widget_media_artist_init();
#if IS_ENABLED(CONFIG_NICE_VIEW_HID_MEDIA_SCROLL)
    widget_activity_status_init();
    if (media_scroll_timer == NULL) {
        media_scroll_timer = lv_timer_create(media_scroll_tick,
                                             CONFIG_NICE_VIEW_HID_MEDIA_SCROLL_INTERVAL_MS, NULL);
    }
#endif
#endif

    redraw_widget(widget);
    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
