/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>

#define ZMK_SPLIT_PERIPHERAL_OUTPUT_PAYLOAD_MAX CONFIG_ZMK_SPLIT_PERIPHERAL_OUTPUT_MAX_PAYLOAD

struct zmk_split_bt_output_relay_event {
    uint8_t relay_channel;
    uint8_t value;
    uint8_t payload_size;
    uint8_t payload[ZMK_SPLIT_PERIPHERAL_OUTPUT_PAYLOAD_MAX];
} __packed;

struct zmk_split_output_event {
    const struct device *dev;
    uint8_t value;
    uint8_t payload_size;
    uint8_t payload[ZMK_SPLIT_PERIPHERAL_OUTPUT_PAYLOAD_MAX];
};

int zmk_split_bt_invoke_output(const struct device *dev,
                               struct zmk_split_bt_output_relay_event event);
int zmk_split_bt_invoke_output_channel(uint8_t relay_channel, uint8_t value,
                                       const uint8_t *payload, uint8_t payload_size);
