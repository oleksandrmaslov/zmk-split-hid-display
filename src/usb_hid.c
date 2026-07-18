#include <raw_hid/raw_hid.h>
#include <raw_hid/events.h>

#include <string.h>

#include <zmk/usb.h>

#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct device *raw_hid_dev;
static uint8_t raw_hid_usb_report[CONFIG_RAW_HID_REPORT_SIZE];

static K_SEM_DEFINE(hid_sem, 1, 1);

static void in_ready_cb(const struct device *dev) {
    ARG_UNUSED(dev);
    k_sem_give(&hid_sem);
}

#define HID_REPORT_TYPE_MASK 0xff00
#define HID_REPORT_ID_MASK 0x00ff

#define HID_REPORT_TYPE_OUTPUT 0x200

static int get_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    ARG_UNUSED(dev);
    ARG_UNUSED(setup);
    ARG_UNUSED(len);
    ARG_UNUSED(data);

    return -ENOTSUP;
}

static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    ARG_UNUSED(dev);

    if (setup == NULL || len == NULL || data == NULL) {
        return -EINVAL;
    }

    if ((setup->wValue & HID_REPORT_TYPE_MASK) != HID_REPORT_TYPE_OUTPUT ||
        (setup->wValue & HID_REPORT_ID_MASK) != 0U) {
        LOG_ERR("[# raw-hid #] Set: Unsupported report type %d requested",
                (setup->wValue & HID_REPORT_TYPE_MASK) >> 8);
        return -ENOTSUP;
    }
    if (*len < 0 || *len > CONFIG_RAW_HID_REPORT_SIZE) {
        LOG_WRN("USB - Raw HID report too large: %d", *len);
        return -EMSGSIZE;
    }
    if (*len == 0) {
        return 0;
    }
    if (*data == NULL) {
        LOG_WRN("USB - Raw HID report has no payload");
        return -EINVAL;
    }

    LOG_INF("USB - Received Raw HID report of length %i", *len);
    LOG_HEXDUMP_DBG(*data, (size_t)*len, "USB - Received Raw HID report");
    raise_raw_hid_received_event(
        (struct raw_hid_received_event){.data = *data, .length = (uint8_t)*len});

    return 0;
}

static const struct hid_ops ops = {
    .int_in_ready = in_ready_cb,
    .get_report = get_report_cb,
    .set_report = set_report_cb,
};

static void send_report(const uint8_t *data, uint8_t len) {
    if (data == NULL) {
        LOG_WRN("USB - Ignoring Raw HID report with no data");
        return;
    }
    if (raw_hid_dev == NULL) {
        LOG_WRN("USB - Raw HID device is not ready");
        return;
    }
    if (!zmk_usb_is_hid_ready()) {
        LOG_DBG("USB - Raw HID endpoint is not connected");
        return;
    }
    if (len > CONFIG_RAW_HID_REPORT_SIZE) {
        LOG_WRN("USB - Dropping oversized Raw HID report (%u bytes)", (unsigned int)len);
        return;
    }
    if (k_sem_take(&hid_sem, K_NO_WAIT) != 0) {
        LOG_WRN("USB - Raw HID endpoint is busy; dropping report");
        return;
    }

    LOG_INF("USB - Sending Raw HID report of length %i", len);
    memset(raw_hid_usb_report, 0, sizeof(raw_hid_usb_report));
    memcpy(raw_hid_usb_report, data, len);
    LOG_HEXDUMP_DBG(raw_hid_usb_report, sizeof(raw_hid_usb_report),
                    "USB - Sending Raw HID report");

    int err = hid_int_ep_write(raw_hid_dev, raw_hid_usb_report, sizeof(raw_hid_usb_report), NULL);
    if (err) {
        LOG_ERR("Failed to send report: %i", err);
        k_sem_give(&hid_sem);
    }
}

static int raw_hid_sent_event_listener(const zmk_event_t *eh) {
    struct raw_hid_sent_event *event = as_raw_hid_sent_event(eh);
    if (event) {
        send_report(event->data, event->length);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(usb_process_raw_hid_sent_event, raw_hid_sent_event_listener);
ZMK_SUBSCRIPTION(usb_process_raw_hid_sent_event, raw_hid_sent_event);

static int raw_hid_init(void) {
    raw_hid_dev = device_get_binding(CONFIG_RAW_HID_DEVICE);
    if (raw_hid_dev == NULL) {
        LOG_ERR("Unable to locate HID device");
        return -EINVAL;
    }

    usb_hid_register_device(raw_hid_dev, raw_hid_report_desc, sizeof(raw_hid_report_desc), &ops);

    int err = usb_hid_init(raw_hid_dev);
    if (err) {
        LOG_ERR("Unable to initialize Raw HID device (%d)", err);
        return err;
    }

    return 0;
}

SYS_INIT(raw_hid_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
