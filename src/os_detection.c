/*
 * Copyright (c) 2026 Rafael Romão
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_os_detection

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/events/usb_conn_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
#include <zmk/ble.h>
#endif

#include "os_detection_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * The two lists become bitmasks over enum zmk_os at build time. A name that
 * is not one of the enum's becomes an undeclared ZMK_OS_<NAME> here and fails
 * the build, which is the whole reason for going through the token macro
 * rather than comparing strings at runtime.
 */
#define OS_BIT(node_id, prop, idx) BIT(UTIL_CAT(ZMK_OS_, DT_STRING_UPPER_TOKEN_BY_IDX(node_id, prop, idx)))
#define OS_MASK(prop) (DT_INST_FOREACH_PROP_ELEM_SEP(0, prop, OS_BIT, (|)))

#define ACTIVATE_MASK OS_MASK(activate_for)
#define DEACTIVATE_MASK OS_MASK(deactivate_for)

#define OS_LAYER ((zmk_keymap_layer_id_t)DT_INST_PROP(0, layer))
#define SETTLE_MS DT_INST_PROP(0, settle_ms)

BUILD_ASSERT((ACTIVATE_MASK & DEACTIVATE_MASK) == 0,
             "an OS cannot be in both activate-for and deactivate-for");

#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_USB)
static enum zmk_os usb_detected = ZMK_OS_UNKNOWN;
#endif

#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
static enum zmk_os ble_detected[ZMK_BLE_PROFILE_COUNT];
#endif

/*
 * Reading the answer off the *selected* endpoint, rather than off whichever
 * transport last reported, is what keeps a USB fingerprint from being applied
 * while a BLE profile is in use. Both can be live at once -- the cable stays
 * enumerated after &out OUT_BLE -- and writing then would stamp the laptop's
 * OS onto the phone's layer state, which zmk,persistent-layers would then
 * dutifully record against the phone.
 */
enum zmk_os zmk_os_detection_current(void) {
    struct zmk_endpoint_instance endpoint = zmk_endpoint_get_selected();

    switch (endpoint.transport) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_USB)
    case ZMK_TRANSPORT_USB:
        return usb_detected;
#endif
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
    case ZMK_TRANSPORT_BLE:
        if (endpoint.ble.profile_index >= ZMK_BLE_PROFILE_COUNT) {
            return ZMK_OS_UNKNOWN;
        }
        return ble_detected[endpoint.ble.profile_index];
#endif
    default:
        return ZMK_OS_UNKNOWN;
    }
}

static void apply(void) {
    enum zmk_os os = zmk_os_detection_current();
    if (os == ZMK_OS_UNKNOWN) {
        return;
    }

    uint32_t bit = BIT(os);
    bool on;
    if (ACTIVATE_MASK & bit) {
        on = true;
    } else if (DEACTIVATE_MASK & bit) {
        on = false;
    } else {
        /* Detected, but the keymap has nothing to say about it. */
        return;
    }

    /*
     * Comparing first is not just an optimisation: a write raises
     * zmk_layer_state_changed, which zmk,persistent-layers records and
     * eventually commits to flash. Agreeing silently is what keeps a
     * plug/unplug cycle on a known host free of flash writes.
     */
    if (zmk_keymap_layer_active(OS_LAYER) == on) {
        return;
    }

    LOG_INF("os detection: os %d -> layer %d %s", os, OS_LAYER, on ? "on" : "off");

    /*
     * Locking, both ways, to match &lock_on / &lock_off on the toggles layer
     * and zmk,persistent-layers' own replay. A non-locking deactivation of a
     * layer that was locked on is refused outright, and a non-locking
     * activation would leave the layer clearable by every &to and auto-layer
     * exit -- so the mode would quietly stop being a mode.
     */
    if (on) {
        zmk_keymap_layer_activate(OS_LAYER, true);
    } else {
        zmk_keymap_layer_deactivate(OS_LAYER, true);
    }
}

static void apply_work_handler(struct k_work *work) { apply(); }

static K_WORK_DELAYABLE_DEFINE(apply_work, apply_work_handler);

/*
 * Never applied straight from the event handler. Both this module and
 * zmk,persistent-layers subscribe to zmk_endpoint_changed, and their relative
 * order is link order -- so a synchronous write here would be overwritten by
 * a replay roughly half the time. A short delay puts it deterministically
 * last, and lets the replay supply the instant answer in the meantime.
 */
static void schedule_apply(void) { k_work_reschedule(&apply_work, K_MSEC(SETTLE_MS)); }

void zmk_os_detection_report_usb(enum zmk_os detected) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_USB)
    if (usb_detected == detected) {
        return;
    }
    usb_detected = detected;
    schedule_apply();
#endif
}

void zmk_os_detection_report_ble(uint8_t profile_index, enum zmk_os detected) {
#if IS_ENABLED(CONFIG_ZMK_OS_DETECTION_BLE)
    if (profile_index >= ZMK_BLE_PROFILE_COUNT || ble_detected[profile_index] == detected) {
        return;
    }
    ble_detected[profile_index] = detected;
    schedule_apply();
#endif
}

static int os_detection_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_OS_DETECTION_USB)
    const struct zmk_usb_conn_state_changed *usb_ev = as_zmk_usb_conn_state_changed(eh);
    if (usb_ev != NULL) {
        if (usb_ev->conn_state == ZMK_USB_CONN_NONE) {
            /* The next cable may be a different machine; do not carry a guess
             * across an unplug. */
            usb_detected = ZMK_OS_UNKNOWN;
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    /* An endpoint change can make a different host's answer the current one
     * without any new fingerprint having arrived. */
    schedule_apply();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(os_detection, os_detection_listener);
ZMK_SUBSCRIPTION(os_detection, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_OS_DETECTION_USB)
ZMK_SUBSCRIPTION(os_detection, zmk_usb_conn_state_changed);
#endif

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
