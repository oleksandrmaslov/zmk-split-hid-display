"""Replay host reports through the real BLE write callback with event I/O stubbed."""

import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/hog.c").read_text()
start = source.index("static ssize_t write_hids_raw_hid_report(")
end = source.index("static ssize_t write_ctrl_point(", start)
callback = source[start:end]

environment = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

struct bt_conn;
struct bt_gatt_attr;
struct raw_hid_received_event { const uint8_t *data; uint8_t length; };
#define BT_ATT_ERR_INVALID_OFFSET 7
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN 13
#define BT_GATT_ERR(error) (-(error))
#define LOG_WRN(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_HEXDUMP_DBG(...) ((void)0)
static unsigned events;
static uint8_t received[CONFIG_RAW_HID_REPORT_SIZE];
static uint8_t received_len;
static void raise_raw_hid_received_event(struct raw_hid_received_event event) {
    assert(event.length <= CONFIG_RAW_HID_REPORT_SIZE);
    received_len = event.length;
    memcpy(received, event.data, event.length);
    ++events;
}
"""

checks = r"""
static void accept(const uint8_t *wire, uint16_t len, const uint8_t *expected,
                   uint8_t expected_len) {
    events = 0;
    assert(write_hids_raw_hid_report(NULL, NULL, wire, len, 0, 0) == len);
    assert(events == 1);
    assert(received_len == expected_len);
    assert(memcmp(received, expected, expected_len) == 0);
}

static void reject(const uint8_t *wire, uint16_t len, uint16_t offset, int error) {
    events = 0;
    assert(write_hids_raw_hid_report(NULL, NULL, wire, len, offset, 0) == -error);
    assert(events == 0);
}

int main(void) {
    uint8_t wire[CONFIG_RAW_HID_REPORT_SIZE + 2] = {0};
    for (unsigned i = 1; i <= CONFIG_RAW_HID_REPORT_SIZE; ++i) wire[i] = i;
    wire[1] = 0xAA; /* Captured HIDAPI time report: zero ID, then payload. */
    accept(wire + 1, CONFIG_RAW_HID_REPORT_SIZE, wire + 1, CONFIG_RAW_HID_REPORT_SIZE);
    accept(wire, CONFIG_RAW_HID_REPORT_SIZE + 1, wire + 1, CONFIG_RAW_HID_REPORT_SIZE);
    reject(wire, CONFIG_RAW_HID_REPORT_SIZE + 2, 0, BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    reject(wire, CONFIG_RAW_HID_REPORT_SIZE + 1, 1, BT_ATT_ERR_INVALID_OFFSET);
    wire[0] = 1;
    reject(wire, CONFIG_RAW_HID_REPORT_SIZE + 1, 0, BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    wire[0] = 0;
    /* A zero in a legal-size payload is data, not a report-ID prefix. */
    accept(wire, CONFIG_RAW_HID_REPORT_SIZE, wire, CONFIG_RAW_HID_REPORT_SIZE);
    accept(wire, 0, wire, 0);
    if (CONFIG_RAW_HID_REPORT_SIZE >= 3) accept(wire + 1, 3, wire + 1, 3);
    puts("Native and zero-prefixed reports preserve payload, bounds and ATT write length.");
}
"""

with tempfile.TemporaryDirectory(prefix="raw-hid-test-") as directory:
    root = Path(directory)
    test = root / "check.c"
    test.write_text(environment + callback + checks)
    for size in (32, 1, 255):
        binary = root / f"check-{size}"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", f"-DCONFIG_RAW_HID_REPORT_SIZE={size}",
             str(test), "-o", str(binary)], check=True,
        )
        subprocess.run([str(binary)], check=True)
