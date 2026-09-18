/*
 * Copyright (c) 2026 Rafael Romão
 *
 * SPDX-License-Identifier: MIT
 *
 * The USB fingerprints below follow the signatures published by
 * cormoran/zmk-feature-os-detection (MIT). See the README for the attribution.
 */

#define DT_DRV_COMPAT zmk_os_detection

#include <zephyr/devicetree.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/usb/usb_device.h>

#include "os_detection_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Two things have to be true before a host's descriptor reads can be watched
 * at all, and both are side effects of CONFIG_USB_DEVICE_BOS:
 *
 *  - usb_handle_bos() only exists when it is set, and that is the function
 *    the hook below wraps. Zephyr calls it from usb_handle_standard_request()
 *    ahead of the dispatch, for every standard setup packet, which is exactly
 *    the vantage point wanted -- it sees GET_DESCRIPTOR and SET_ADDRESS alike
 *    and answers -ENOTSUP to everything that is not a BOS read.
 *
 *  - it also bumps bcdUSB from 2.00 to 2.01 in the device descriptor, which
 *    is what makes a host ask for a BOS descriptor in the first place.
 *
 * That second one has a sharp edge. Zephyr's BOS header starts life with
 * wTotalLength = 0 -- shorter than its own 5-byte header, so an invalid
 * descriptor -- and is only corrected by usb_bos_register_cap(). Nothing in
 * ZMK calls it. A host that reads that answer is within its rights to give
 * up on the device: cormoran's hardware testing found Windows aborts
 * enumeration there and never reaches SET_CONFIGURATION.
 *
 * So register one capability. The USB 2.0 Extension descriptor with
 * bmAttributes = 0 is the honest choice: it claims no LPM support, which is
 * true of this stack, and makes the response well-formed.
 *
 * This must run before ZMK's usb_enable() at APPLICATION. POST_KERNEL always
 * precedes APPLICATION whatever the priority numbers are, so use that rather
 * than racing within a level.
 */
USB_DEVICE_BOS_DESC_DEFINE_CAP struct usb_bos_capability_lpm os_detection_bos_cap = {
    .bLength = sizeof(struct usb_bos_capability_lpm),
    .bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
    .bDevCapabilityType = USB_BOS_CAPABILITY_EXTENSION,
    .bmAttributes = 0,
};

static int os_detection_bos_init(void) {
    usb_bos_register_cap((void *)&os_detection_bos_cap);
    return 0;
}

SYS_INIT(os_detection_bos_init, POST_KERNEL, 0);

static struct zmk_os_usb_stats stats;

/* Uptime of the last setup packet counted, so a gap can be measured. */
static int64_t last_setup_ms;

static void settle_work_handler(struct k_work *work) {
    enum zmk_os detected = zmk_os_classify_usb(&stats);
    LOG_DBG("os detection: usb probe=%u full=%u other=%u bos=%d/%u -> os=%d", stats.string_probe,
            stats.string_full, stats.string_other, stats.bos_seen, stats.bos_wlength, detected);
    zmk_os_detection_report_usb(detected);
}

/*
 * The counts are cleared by SET_ADDRESS and by nothing else, and that is
 * deliberate. Clearing them here instead, once a verdict had been delivered,
 * was tried and is wrong: a host's descriptor reads arrive in more than one
 * burst, so several settle windows fire during a single enumeration, and
 * emptying the counts between them makes one machine produce two verdicts that
 * disagree. The layer moved to the alt OS and back on its own.
 *
 * A window is one enumeration. Only the host says when that begins.
 */

static K_WORK_DELAYABLE_DEFINE(settle_work, settle_work_handler);

/* Observation only. Never changes what the host is told. */
static void observe(const struct usb_setup_packet *setup) {
    if (setup->bRequest == USB_SREQ_SET_ADDRESS) {
        /* The host has started enumerating again; the counts so far belong to
         * a previous attempt, or a previous machine. */
        memset(&stats, 0, sizeof(stats));
        return;
    }

    if (setup->bRequest != USB_SREQ_GET_DESCRIPTOR || !usb_reqtype_is_to_host(setup)) {
        return;
    }

    /*
     * A long enough silence means the packet opening it belongs to a different
     * computer, so the counts so far describe the last one and must go.
     *
     * SET_ADDRESS above is meant to do this and evidently does not reach here
     * on every enumeration -- behind a KVM the layer would enter the alt OS
     * mode and never leave, which only happens if a macOS session's counts
     * survive into a Linux one. This does not depend on seeing any particular
     * request: a host's descriptor reads arrive within about 20 ms of each
     * other (usbmon, worst case 17), and two hosts are seconds apart, so the
     * gap itself says where one enumeration ends.
     */
    int64_t now = k_uptime_get();
    if (now - last_setup_ms > CONFIG_ZMK_OS_DETECTION_USB_STALE_MS) {
        memset(&stats, 0, sizeof(stats));
    }
    last_setup_ms = now;

    switch (USB_GET_DESCRIPTOR_TYPE(setup->wValue)) {
    case USB_DESC_STRING:
        if (setup->wLength == 2) {
            stats.string_probe++;
        } else if (setup->wLength == 255) {
            stats.string_full++;
        } else {
            stats.string_other++;
        }
        break;
    case USB_DESC_BOS:
        stats.bos_seen = true;
        stats.bos_wlength = setup->wLength;
        break;
    default:
        /* Not a signal. Returning here rather than falling through matters:
         * restarting the debounce on untracked traffic would keep pushing the
         * verdict out for as long as the host keeps talking. */
        return;
    }

    k_work_reschedule(&settle_work, K_MSEC(CONFIG_ZMK_OS_DETECTION_USB_SETTLE_MS));
}

int __real_usb_handle_bos(struct usb_setup_packet *setup, int32_t *len, uint8_t **data);

int __wrap_usb_handle_bos(struct usb_setup_packet *setup, int32_t *len, uint8_t **data) {
    observe(setup);
    return __real_usb_handle_bos(setup, len, data);
}

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
