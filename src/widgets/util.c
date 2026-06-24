/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include "util.h"

/*
 * Battery and connectivity glyphs are adapted from kevinpastor/nice-view-elemental,
 * MIT licensed.
 */

#if CONFIG_NICE_VIEW_HID_INVERTED
#define ELEMENTAL_BG_COLOR_BYTES 0x00, 0x00, 0x00, 0xff
#define ELEMENTAL_TRANSPARENT_BG_COLOR_BYTES 0x00, 0x00, 0x00, 0x00
#define ELEMENTAL_FG_COLOR_BYTES 0xff, 0xff, 0xff, 0xff
#else
#define ELEMENTAL_BG_COLOR_BYTES 0xff, 0xff, 0xff, 0xff
#define ELEMENTAL_TRANSPARENT_BG_COLOR_BYTES 0xff, 0xff, 0xff, 0x00
#define ELEMENTAL_FG_COLOR_BYTES 0x00, 0x00, 0x00, 0xff
#endif

static const uint8_t bluetooth_logo_map[] = {
    ELEMENTAL_BG_COLOR_BYTES,
    ELEMENTAL_FG_COLOR_BYTES,
    0x0f, 0x00, 0x3f, 0xc0, 0x7f, 0xe0, 0x7b, 0xe0, 0xf9, 0xf0, 0xfa, 0xf0,
    0xeb, 0x70, 0xf2, 0xf0, 0xf9, 0xf0, 0xf2, 0xf0, 0xeb, 0x70, 0xfa, 0xf0,
    0xf9, 0xf0, 0x7b, 0xe0, 0x7f, 0xe0, 0x3f, 0xc0, 0x0f, 0x00,
};

static const uint8_t bluetooth_logo_outlined_map[] = {
    ELEMENTAL_BG_COLOR_BYTES,
    ELEMENTAL_FG_COLOR_BYTES,
    0x0f, 0x00, 0x30, 0xc0, 0x40, 0x20, 0x44, 0x20, 0x86, 0x10, 0x85, 0x10,
    0x94, 0x90, 0x8d, 0x10, 0x86, 0x10, 0x8d, 0x10, 0x94, 0x90, 0x85, 0x10,
    0x86, 0x10, 0x44, 0x20, 0x40, 0x20, 0x30, 0xc0, 0x0f, 0x00,
};

static const uint8_t bluetooth_searching_map[] = {
    ELEMENTAL_BG_COLOR_BYTES,
    ELEMENTAL_FG_COLOR_BYTES,
    0x0f, 0x00, 0x30, 0xc0, 0x40, 0x20, 0x40, 0x20, 0x82, 0x10, 0x81, 0x10,
    0x89, 0x10, 0x84, 0x90, 0x94, 0x90, 0x84, 0x90, 0x89, 0x10, 0x81, 0x10,
    0x82, 0x10, 0x40, 0x20, 0x40, 0x20, 0x30, 0xc0, 0x0f, 0x00,
};

static const uint8_t usb_logo_map[] = {
    ELEMENTAL_BG_COLOR_BYTES,
    ELEMENTAL_FG_COLOR_BYTES,
    0x00, 0x10, 0x00, 0x00, 0xf8, 0x00, 0x01, 0x10, 0x00,
    0xe2, 0x00, 0x80, 0xff, 0xff, 0xc0, 0xe0, 0x80, 0x80,
    0x00, 0x40, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x0c, 0x00,
};

static const lv_image_dsc_t bluetooth_logo = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_I1,
    .header.w = 12,
    .header.h = 17,
    .header.stride = 2,
    .data_size = sizeof(bluetooth_logo_map),
    .data = bluetooth_logo_map,
};

static const lv_image_dsc_t bluetooth_logo_outlined = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_I1,
    .header.w = 12,
    .header.h = 17,
    .header.stride = 2,
    .data_size = sizeof(bluetooth_logo_outlined_map),
    .data = bluetooth_logo_outlined_map,
};

static const lv_image_dsc_t bluetooth_searching = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_I1,
    .header.w = 12,
    .header.h = 17,
    .header.stride = 2,
    .data_size = sizeof(bluetooth_searching_map),
    .data = bluetooth_searching_map,
};

static const lv_image_dsc_t usb_logo = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_I1,
    .header.w = 18,
    .header.h = 9,
    .header.stride = 3,
    .data_size = sizeof(usb_logo_map),
    .data = usb_logo_map,
};

/*
 * Custom UI bitmaps prepared by the user (references/*.c). The original
 * arrays were exported in LVGL 8 LV_IMG_CF_ALPHA_1BIT layout (no palette,
 * 1 bit/pixel, MSB first, padded to byte). We re-wrap that same pixel data
 * with the LVGL 9 LV_COLOR_FORMAT_I1 palette prefix used elsewhere in this
 * module so they integrate with the existing canvas draw helpers.
 */

/* Profile dots — 10×10, stride 2 */
static const uint8_t selected_profile_map[] = {
    ELEMENTAL_BG_COLOR_BYTES, ELEMENTAL_FG_COLOR_BYTES,
  0x7f, 0x80, 0xff, 0xc0, 0xff, 0xc0, 0xff, 0xc0, 0xff, 0xc0,
  0xff, 0xc0, 0xff, 0xc0, 0xff, 0xc0, 0xff, 0xc0, 0x7f, 0x80,
};
static const uint8_t bonded_profile_map[] = {
    ELEMENTAL_BG_COLOR_BYTES, ELEMENTAL_FG_COLOR_BYTES,
  0x7f, 0x80, 0xc0, 0xc0, 0x80, 0x40, 0x80, 0x40, 0x80, 0x40,
  0x80, 0x40, 0x80, 0x40, 0x80, 0x40, 0xc0, 0xc0, 0x7f, 0x80,
};
static const uint8_t free_profile_map[] = {
    ELEMENTAL_BG_COLOR_BYTES, ELEMENTAL_FG_COLOR_BYTES,
    0x55, 0x00, 0x00, 0x40, 0x80, 0x00, 0x00, 0x40, 0x80, 0x00,
    0x00, 0x40, 0x80, 0x00, 0x00, 0x40, 0x80, 0x00, 0x2a, 0x80,
};
static const uint8_t selected_free_profile_map[] = {
    ELEMENTAL_BG_COLOR_BYTES, ELEMENTAL_FG_COLOR_BYTES,
    0x55, 0x00, 0x7f, 0xc0, 0xff, 0x80, 0x7f, 0xc0, 0xff, 0x80,
    0x7f, 0xc0, 0xff, 0x80, 0x7f, 0xc0, 0xff, 0x80, 0x2a, 0x80,
};

/* 10×10 globe icon */
static const uint8_t language_icon_map[] = {
    ELEMENTAL_BG_COLOR_BYTES, ELEMENTAL_FG_COLOR_BYTES,
    0x1e, 0x00, 0x73, 0x80, 0x52, 0x80, 0xff, 0xc0, 0xa1, 0x40,
    0xa1, 0x40, 0xff, 0xc0, 0x52, 0x80, 0x73, 0x80, 0x1e, 0x00,
};

/* Volume / speaker glyphs */
static const uint8_t speaker_mute_map[] = {
    ELEMENTAL_BG_COLOR_BYTES, ELEMENTAL_FG_COLOR_BYTES,
    0x08, 0x00, 0x1a, 0x10, 0xf9, 0x20, 0xf8, 0xc0,
    0xf8, 0x40, 0xf9, 0x20, 0x1a, 0x10, 0x08, 0x00,
};
static const uint8_t speaker_middle_volume_map[] = {
    ELEMENTAL_BG_COLOR_BYTES, ELEMENTAL_FG_COLOR_BYTES,
    0x08, 0x18, 0xfa, 0xfb, 0xfb, 0xfa, 0x18, 0x08,
};
static const uint8_t volume_loud_wave_map[] = {
    ELEMENTAL_TRANSPARENT_BG_COLOR_BYTES, ELEMENTAL_FG_COLOR_BYTES,
    0x80, 0x60, 0x20, 0x10, 0x10, 0x10, 0x10, 0x20, 0x60, 0x80,
};

#define DEFINE_I1_IMG(_name, _w, _h, _stride)                                                      \
    static const lv_image_dsc_t _name = {                                                          \
        .header.magic = LV_IMAGE_HEADER_MAGIC,                                                     \
        .header.cf = LV_COLOR_FORMAT_I1,                                                           \
        .header.w = (_w),                                                                          \
        .header.h = (_h),                                                                          \
        .header.stride = (_stride),                                                                \
        .data_size = sizeof(_name##_map),                                                          \
        .data = _name##_map,                                                                       \
    }

DEFINE_I1_IMG(selected_profile, 10, 10, 2);
DEFINE_I1_IMG(bonded_profile, 10, 10, 2);
DEFINE_I1_IMG(free_profile, 10, 10, 2);
DEFINE_I1_IMG(selected_free_profile, 10, 10, 2);
DEFINE_I1_IMG(language_icon, 10, 10, 2);
DEFINE_I1_IMG(speaker_mute, 12, 8, 2);
DEFINE_I1_IMG(speaker_middle_volume, 8, 8, 1);
DEFINE_I1_IMG(volume_loud_wave, 4, 10, 1);

static void draw_static_img(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y,
                            const lv_image_dsc_t *src) {
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    canvas_draw_img(canvas, x, y, src, &dsc);
}

void rotate_portrait_canvas(uint8_t *source_buf, uint8_t *dest_buf) {
    const uint32_t source_stride =
        lv_draw_buf_width_to_stride(NICE_VIEW_HID_PORTRAIT_WIDTH, CANVAS_COLOR_FORMAT);
    const uint32_t dest_stride =
        lv_draw_buf_width_to_stride(NICE_VIEW_HID_SCREEN_WIDTH, CANVAS_COLOR_FORMAT);

    lv_draw_sw_rotate(source_buf, dest_buf, NICE_VIEW_HID_PORTRAIT_WIDTH,
                      NICE_VIEW_HID_PORTRAIT_HEIGHT, source_stride, dest_stride,
                      LV_DISPLAY_ROTATION_270, CANVAS_COLOR_FORMAT);
}

static void canvas_set_px(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_color_t color) {
    lv_canvas_set_px(canvas, x, y, color, LV_OPA_COVER);
}

#define ELEMENTAL_BATTERY_SOURCE_WIDTH 11
#define ELEMENTAL_BATTERY_X 4
#define ELEMENTAL_BATTERY_Y 6

static void battery_set_px(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t source_x,
                           lv_coord_t source_y, lv_color_t color) {
    canvas_set_px(canvas, x + source_y, y + (ELEMENTAL_BATTERY_SOURCE_WIDTH - 1 - source_x),
                  color);
}

static void battery_draw_rect(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t source_x,
                              lv_coord_t source_y, lv_coord_t w, lv_coord_t h,
                              lv_color_t color) {
    for (lv_coord_t dy = 0; dy < h; dy++) {
        for (lv_coord_t dx = 0; dx < w; dx++) {
            battery_set_px(canvas, x, y, source_x + dx, source_y + dy, color);
        }
    }
}

/*
 * Battery pixels come from nice-view-elemental's vertical 11x24 glyph. The
 * module's layout is authored in portrait coordinates, so draw those pixels
 * rotated counter-clockwise into a 24x11 battery with the terminal on the right.
 */
static void draw_battery_outline(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    battery_draw_rect(canvas, x, y, 10, 2, 1, 19, LVGL_FOREGROUND);
    battery_draw_rect(canvas, x, y, 2, 22, 7, 1, LVGL_FOREGROUND);
    battery_draw_rect(canvas, x, y, 0, 2, 1, 19, LVGL_FOREGROUND);
    battery_draw_rect(canvas, x, y, 2, 0, 7, 1, LVGL_FOREGROUND);

    battery_set_px(canvas, x, y, 9, 1, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 9, 21, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 1, 21, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 1, 1, LVGL_FOREGROUND);

    battery_draw_rect(canvas, x, y, 4, 23, 3, 1, LVGL_FOREGROUND);
}

static void draw_battery_lightning_bolt(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    battery_set_px(canvas, x, y, 8, 11, LVGL_BACKGROUND);
    battery_set_px(canvas, x, y, 8, 12, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 8, 13, LVGL_BACKGROUND);

    battery_set_px(canvas, x, y, 7, 10, LVGL_BACKGROUND);
    battery_set_px(canvas, x, y, 7, 11, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 7, 12, LVGL_BACKGROUND);

    battery_set_px(canvas, x, y, 6, 9, LVGL_BACKGROUND);
    battery_set_px(canvas, x, y, 6, 10, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 6, 11, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 6, 12, LVGL_BACKGROUND);

    battery_set_px(canvas, x, y, 5, 9, LVGL_BACKGROUND);
    battery_set_px(canvas, x, y, 5, 10, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 5, 11, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 5, 12, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 5, 13, LVGL_BACKGROUND);

    battery_set_px(canvas, x, y, 4, 10, LVGL_BACKGROUND);
    battery_set_px(canvas, x, y, 4, 11, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 4, 12, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 4, 13, LVGL_BACKGROUND);

    battery_set_px(canvas, x, y, 3, 10, LVGL_BACKGROUND);
    battery_set_px(canvas, x, y, 3, 11, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 3, 12, LVGL_BACKGROUND);

    battery_set_px(canvas, x, y, 2, 9, LVGL_BACKGROUND);
    battery_set_px(canvas, x, y, 2, 10, LVGL_FOREGROUND);
    battery_set_px(canvas, x, y, 2, 11, LVGL_BACKGROUND);
}

static void draw_elemental_battery_at(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y,
                                      uint8_t level, bool charging) {
    draw_battery_outline(canvas, x, y);

    uint8_t clamped_level = level > 100 ? 100 : level;
    const uint8_t height = (19 * clamped_level) / 100;
    battery_draw_rect(canvas, x, y, 2, 2, 7, height, LVGL_FOREGROUND);

    battery_set_px(canvas, x, y, 8, 2, LVGL_BACKGROUND);
    battery_set_px(canvas, x, y, 8, 20, LVGL_BACKGROUND);
    battery_set_px(canvas, x, y, 2, 20, LVGL_BACKGROUND);
    battery_set_px(canvas, x, y, 2, 2, LVGL_BACKGROUND);

    if (charging) {
        draw_battery_lightning_bolt(canvas, x, y);
    }
}

void draw_battery(lv_obj_t *canvas, const struct status_state *state) {
    draw_elemental_battery_at(canvas, ELEMENTAL_BATTERY_X, ELEMENTAL_BATTERY_Y, state->battery,
                              state->charging);
}

void draw_elemental_bluetooth_logo(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    canvas_draw_img(canvas, x, y, &bluetooth_logo, &img_dsc);
}

void draw_elemental_bluetooth_logo_outlined(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    canvas_draw_img(canvas, x, y, &bluetooth_logo_outlined, &img_dsc);
}

void draw_elemental_bluetooth_searching(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    canvas_draw_img(canvas, x, y, &bluetooth_searching, &img_dsc);
}

void draw_elemental_usb_logo(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    canvas_draw_img(canvas, x, y, &usb_logo, &img_dsc);
}

void draw_profile_selected(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    draw_static_img(canvas, x, y, &selected_profile);
}

void draw_profile_bonded(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    draw_static_img(canvas, x, y, &bonded_profile);
}

void draw_profile_free(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    draw_static_img(canvas, x, y, &free_profile);
}

void draw_profile_selected_free(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    draw_static_img(canvas, x, y, &selected_free_profile);
}

void draw_language_icon(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y) {
    draw_static_img(canvas, x, y, &language_icon);
}

/*
 * Volume icon dispatcher.
 *   level == 0          → speaker_mute (12×8)
 *   level <= 50         → speaker_middle_volume (8×8) at the speaker position
 *   level > 50          → speaker_middle_volume + volume_loud_wave (4×10) appended
 *                          to the right of the speaker
 */
void draw_volume_icon(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, uint8_t level) {
    if (level == 0) {
        /* mute: 12×8 — anchor by speaker body so visual center stays put */
        draw_static_img(canvas, x, y, &speaker_mute);
        return;
    }
    /* speaker + medium waves baked in (8×8) */
    draw_static_img(canvas, x, y, &speaker_middle_volume);
    if (level > 50) {
        /* extra outer wave (4×10) — sits flush to the right of the speaker block,
         * shifted up 1 px so its 10-tall extent is vertically centered on the 8-tall body. */
        draw_static_img(canvas, x + 6, y - 1, &volume_loud_wave);
    }
}

void init_label_dsc(lv_draw_label_dsc_t *label_dsc, lv_color_t color, const lv_font_t *font,
                    lv_text_align_t align) {
    lv_draw_label_dsc_init(label_dsc);
    label_dsc->color = color;
    label_dsc->font = font;
    label_dsc->align = align;
}

void init_rect_dsc(lv_draw_rect_dsc_t *rect_dsc, lv_color_t bg_color) {
    lv_draw_rect_dsc_init(rect_dsc);
    rect_dsc->bg_color = bg_color;
    rect_dsc->bg_opa = LV_OPA_COVER;
}

void canvas_draw_rect(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                      lv_draw_rect_dsc_t *draw_dsc) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_area_t coords = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(&layer, draw_dsc, &coords);

    lv_canvas_finish_layer(canvas, &layer);
}

void canvas_draw_text(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t max_w,
                      lv_draw_label_dsc_t *draw_dsc, const char *txt) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    draw_dsc->text = txt;
    lv_area_t coords = {x, y, x + max_w - 1, y + lv_obj_get_height(canvas) - 1};
    lv_draw_label(&layer, draw_dsc, &coords);

    lv_canvas_finish_layer(canvas, &layer);
}

void canvas_draw_img(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, const lv_image_dsc_t *src,
                     lv_draw_image_dsc_t *draw_dsc) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    draw_dsc->src = src;
    lv_area_t coords = {x, y, x + src->header.w - 1, y + src->header.h - 1};
    lv_draw_image(&layer, draw_dsc, &coords);

    lv_canvas_finish_layer(canvas, &layer);
}
