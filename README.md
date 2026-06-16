# ZMK Split HID Display

This ZMK module packages the three pieces needed for a split nice!view keyboard
to show host-provided media data on both halves:

- Raw HID transport for host-to-keyboard data.
- Split peripheral output relay for central-to-peripheral forwarding.
- A custom nice!view status screen with time, layout, volume, and media text.

The central half receives Raw HID reports from the host companion app and
forwards them to connected peripherals over a relay channel. The peripheral
reassembles chunked reports and raises the same Raw HID events locally, so the
display code can use one event path on both halves.

## Installation

Add this module to `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: oleksandrmaslov
      url-base: https://github.com/oleksandrmaslov
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-split-hid-display
      remote: oleksandrmaslov
      revision: main
  self:
    path: config
```

Use the same shields as before:

```yaml
include:
  - board: nice_nano//zmk
    shield: corne_widgets_left nice_view_adapter nice_view_hid_adapter raw_hid_adapter
  - board: nice_nano//zmk
    shield: corne_widgets_right nice_view_adapter nice_view_hid_adapter raw_hid_adapter
```

Add a relay node on both halves. The channel must match
`CONFIG_RAW_HID_SPLIT_RELAY_CHANNEL`.

```dts
/ {
    rawhid_or: rawhid_or {
        compatible = "zmk,split-peripheral-output-relay";
        relay-channel = <1>;
        device = <&nice_view>;
    };
};
```

Recommended config:

```conf
CONFIG_ZMK_DISPLAY=y
CONFIG_NICE_VIEW_HID=y
CONFIG_RAW_HID=y
CONFIG_RAW_HID_REPORT_SIZE=32
CONFIG_RAW_HID_FORWARD_TO_PERIPHERAL=y
CONFIG_RAW_HID_SPLIT_RELAY_CHANNEL=1
CONFIG_ZMK_SPLIT_PERIPHERAL_OUTPUT_MAX_PAYLOAD=32
CONFIG_NICE_VIEW_HID_MEDIA_SCROLL=y
```

The `qmk-hid-host` companion app can send time, layout, volume, artist, and
title payloads using the Raw HID usage page `0xFF60` and usage `0x61`.

## Configuration

| Name | Description | Default |
| --- | --- | --- |
| `CONFIG_RAW_HID` | Enable Raw HID transport | n |
| `CONFIG_RAW_HID_USAGE_PAGE` | HID usage page | `0xFF60` |
| `CONFIG_RAW_HID_USAGE` | HID usage | `0x61` |
| `CONFIG_RAW_HID_REPORT_SIZE` | Raw HID report size in bytes | 32 |
| `CONFIG_RAW_HID_FORWARD_TO_PERIPHERAL` | Forward central Raw HID reports to split peripherals | y when split relay is enabled |
| `CONFIG_RAW_HID_SPLIT_RELAY_CHANNEL` | Relay channel for Raw HID forwarding | 1 |
| `CONFIG_ZMK_SPLIT_PERIPHERAL_OUTPUT_MAX_PAYLOAD` | Max relay payload bytes | 32 |
| `CONFIG_NICE_VIEW_HID` | Enable the custom nice!view status screen | n |
| `CONFIG_NICE_VIEW_HID_SHOW_LAYOUT` | Show host layout name | y |
| `CONFIG_NICE_VIEW_HID_LAYOUTS` | Comma-separated layout names | `EN` |
| `CONFIG_NICE_VIEW_HID_INVERTED` | Invert display colors | n |
| `CONFIG_NICE_VIEW_HID_MEDIA_SCROLL` | Scroll overflowing media text | y |
| `CONFIG_NICE_VIEW_HID_MEDIA_SCROLL_INTERVAL_MS` | Media scroll interval in milliseconds | 350 |
| `CONFIG_NICE_VIEW_HID_MEDIA_SCROLL_START_PAUSE_STEPS` | Scroll ticks to pause at text start | 3 |
| `CONFIG_NICE_VIEW_HID_MEDIA_SCROLL_END_PAUSE_STEPS` | Blank scroll ticks between loop end and restart | 10 |
| `CONFIG_NICE_VIEW_HID_MEDIA_SCROLL_MIN_BATTERY` | Minimum battery percentage for scrolling unless charging | 25 |

## Source

This module combines code originally split across:

- `zmk-raw-hid`
- `zmk-split-peripheral-output-relay`
- `zmk-nice-view-hid`
