/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <lvgl.h>
#include <zmk/endpoints.h>
#include <nice_view_hid/hid.h>

#define NICE_VIEW_HID_PROFILE_COUNT 5

#define NICE_VIEW_HID_SCREEN_WIDTH 160
#define NICE_VIEW_HID_SCREEN_HEIGHT 68
#define NICE_VIEW_HID_PORTRAIT_WIDTH 68
#define NICE_VIEW_HID_PORTRAIT_HEIGHT 160

#define CANVAS_COLOR_FORMAT LV_COLOR_FORMAT_L8
#define SCREEN_CANVAS_BUF_SIZE                                                                     \
    LV_CANVAS_BUF_SIZE(NICE_VIEW_HID_SCREEN_WIDTH, NICE_VIEW_HID_SCREEN_HEIGHT,                    \
                       LV_COLOR_FORMAT_GET_BPP(CANVAS_COLOR_FORMAT), LV_DRAW_BUF_STRIDE_ALIGN)
#define PORTRAIT_CANVAS_BUF_SIZE                                                                   \
    LV_CANVAS_BUF_SIZE(NICE_VIEW_HID_PORTRAIT_WIDTH, NICE_VIEW_HID_PORTRAIT_HEIGHT,                \
                       LV_COLOR_FORMAT_GET_BPP(CANVAS_COLOR_FORMAT), LV_DRAW_BUF_STRIDE_ALIGN)

#define LVGL_BACKGROUND                                                                            \
    IS_ENABLED(CONFIG_NICE_VIEW_HID_INVERTED) ? lv_color_black() : lv_color_white()
#define LVGL_FOREGROUND                                                                            \
    IS_ENABLED(CONFIG_NICE_VIEW_HID_INVERTED) ? lv_color_white() : lv_color_black()

struct status_state {
    uint8_t battery;
    bool charging;
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    struct zmk_endpoint_instance selected_endpoint;
    uint8_t active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
    bool profile_connected[NICE_VIEW_HID_PROFILE_COUNT];
    bool profile_bonded[NICE_VIEW_HID_PROFILE_COUNT];
    uint8_t layer_index;
    const char *layer_label;
#elif IS_ENABLED(CONFIG_ZMK_SPLIT)
    bool connected;
#endif
#if IS_ENABLED(CONFIG_RAW_HID)
    bool is_connected;
    uint8_t hour;
    uint8_t minute;
    uint8_t volume;
    uint8_t layout;
    char media_artist[NICE_VIEW_HID_TEXT_MAX_LEN + 1];
    char media_title[NICE_VIEW_HID_TEXT_MAX_LEN + 1];
#endif
};

struct battery_status_state {
    uint8_t level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    bool usb_present;
#endif
};

void rotate_portrait_canvas(uint8_t *source_buf, uint8_t *dest_buf);
void draw_battery(lv_obj_t *canvas, const struct status_state *state);
void draw_elemental_bluetooth_logo(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y);
void draw_elemental_bluetooth_logo_outlined(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y);
void draw_elemental_bluetooth_searching(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y);
void draw_elemental_usb_logo(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y);
void draw_profile_selected(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y);
void draw_profile_selected_free(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y);
void draw_profile_bonded(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y);
void draw_profile_free(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y);
void draw_language_icon(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y);
void draw_volume_icon(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, uint8_t level);
void init_label_dsc(lv_draw_label_dsc_t *label_dsc, lv_color_t color, const lv_font_t *font,
                    lv_text_align_t align);
void init_rect_dsc(lv_draw_rect_dsc_t *rect_dsc, lv_color_t bg_color);
void canvas_draw_rect(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                      lv_draw_rect_dsc_t *draw_dsc);
void canvas_draw_text(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t max_w,
                      lv_draw_label_dsc_t *draw_dsc, const char *txt);
void canvas_draw_img(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, const lv_image_dsc_t *src,
                     lv_draw_image_dsc_t *draw_dsc);
