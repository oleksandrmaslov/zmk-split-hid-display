/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>
#include "util.h"

#define NICE_VIEW_HID_TEXT_CANVAS_WIDTH NICE_VIEW_HID_PORTRAIT_HEIGHT
#define NICE_VIEW_HID_TEXT_CANVAS_HEIGHT 24
#define NICE_VIEW_HID_TEXT_CANVAS_BUF_SIZE                                                         \
    LV_CANVAS_BUF_SIZE(NICE_VIEW_HID_TEXT_CANVAS_WIDTH, NICE_VIEW_HID_TEXT_CANVAS_HEIGHT,          \
                       LV_COLOR_FORMAT_GET_BPP(CANVAS_COLOR_FORMAT), LV_DRAW_BUF_STRIDE_ALIGN)

struct zmk_widget_status {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *screen_canvas;
    lv_obj_t *portrait_canvas;
    lv_obj_t *text_canvas;
    uint8_t screen_cbuf[SCREEN_CANVAS_BUF_SIZE];
    uint8_t portrait_cbuf[PORTRAIT_CANVAS_BUF_SIZE];
    uint8_t text_cbuf[NICE_VIEW_HID_TEXT_CANVAS_BUF_SIZE];
    struct status_state state;
};

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget);
