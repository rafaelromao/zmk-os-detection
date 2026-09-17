/*
 * Copyright (c) 2026 Rafael Romão
 *
 * SPDX-License-Identifier: MIT
 *
 * The BLE fingerprints below follow the signatures published by
 * cormoran/zmk-feature-os-detection (MIT), narrowed to the two operating
 * systems this module names. See the README for the full attribution.
 */

#define DT_DRV_COMPAT zmk_os_detection

#include <zephyr/devicetree.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>

#include "os_detection_internal.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Zephyr 3.5 has no GATT authorization callback -- BT_GATT_AUTHORIZATION_CUSTOM
 * arrived later -- so reads are watched the same way setup packets are, by
 * wrapping the function they all pass through. bt_gatt_attr_read() is public
 * and is what every attribute of interest hands its value to: the HID report
 * map and HIDS info in ZMK's own hog.c, GAP Appearance in the host's gatt.c,
 * the DIS PnP ID in dis.c.
 *
 * It is called for a great deal else besides, including battery level polls
 * that never stop, so the cost of a miss has to stay at one UUID compare --
 * hence the settled latch below rather than reclassifying per read.
 */

struct profile_state {
    struct zmk_os_ble_stats stats;
    struct k_work_delayable settle;
    uint8_t index;
    bool settled;
};

static struct profile_state profiles[ZMK_BLE_PROFILE_COUNT];

static int profile_for(struct bt_conn *conn) {
    if (conn == NULL) {
        return -1;
    }
    return zmk_ble_profile_index(bt_conn_get_dst(conn));
}

static void settle_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct profile_state *p = CONTAINER_OF(dwork, struct profile_state, settle);

    /* Latch before classifying: everything arriving from here on is ordinary
     * traffic, not discovery, and must not reopen the question. */
    p->settled = true;

    enum zmk_os detected = zmk_os_classify_ble(&p->stats);
    LOG_DBG("os detection: ble profile=%u report_map=%u hids_info=%u pnp=%u appearance=%u "
            "mtu=%u -> os=%d",
            p->index, p->stats.report_map, p->stats.hids_info, p->stats.pnp_id,
            p->stats.appearance, p->stats.att_mtu, detected);
    zmk_os_detection_report_ble(p->index, detected);
}

static void observe(struct bt_conn *conn, const struct bt_gatt_attr *attr) {
    int index = profile_for(conn);
    if (index < 0 || index >= ZMK_BLE_PROFILE_COUNT) {
        return;
    }

    struct profile_state *p = &profiles[index];
    if (p->settled || attr == NULL) {
        return;
    }

    if (bt_uuid_cmp(attr->uuid, BT_UUID_HIDS_REPORT_MAP) == 0) {
        p->stats.report_map++;
    } else if (bt_uuid_cmp(attr->uuid, BT_UUID_HIDS_INFO) == 0) {
        p->stats.hids_info++;
    } else if (bt_uuid_cmp(attr->uuid, BT_UUID_DIS_PNP_ID) == 0) {
        p->stats.pnp_id++;
    } else if (bt_uuid_cmp(attr->uuid, BT_UUID_GAP_APPEARANCE) == 0) {
        p->stats.appearance++;
    } else {
        /* Not a signal, and deliberately not a reason to restart the debounce:
         * a host that polls battery level forever would otherwise never let
         * the verdict settle. */
        return;
    }

    k_work_reschedule(&p->settle, K_MSEC(CONFIG_ZMK_OS_DETECTION_BLE_SETTLE_MS));
}

ssize_t __real_bt_gatt_attr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                                 uint16_t buf_len, uint16_t offset, const void *value,
                                 uint16_t value_len);

ssize_t __wrap_bt_gatt_attr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                                 uint16_t buf_len, uint16_t offset, const void *value,
                                 uint16_t value_len) {
    observe(conn, attr);
    return __real_bt_gatt_attr_read(conn, attr, buf, buf_len, offset, value, value_len);
}

static void on_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        return;
    }
    int index = profile_for(conn);
    if (index < 0 || index >= ZMK_BLE_PROFILE_COUNT) {
        return;
    }

    /* A reconnect is a fresh discovery, and may be a host that has been
     * reinstalled since. Start the count over. */
    struct profile_state *p = &profiles[index];
    k_work_cancel_delayable(&p->settle);
    memset(&p->stats, 0, sizeof(p->stats));
    p->settled = false;
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason) {
    int index = profile_for(conn);
    if (index < 0 || index >= ZMK_BLE_PROFILE_COUNT) {
        return;
    }
    /* Do not let a pending settle fire against a profile that has gone. */
    k_work_cancel_delayable(&profiles[index].settle);
}

static struct bt_conn_cb conn_cb = {
    .connected = on_connected,
    .disconnected = on_disconnected,
};

/* Recorded for the log line only -- a possible future signal, since Apple and
 * BlueZ hosts negotiate different maximums, but not one this module acts on. */
static void on_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx) {
    int index = profile_for(conn);
    if (index < 0 || index >= ZMK_BLE_PROFILE_COUNT) {
        return;
    }
    profiles[index].stats.att_mtu = rx;
}

static struct bt_gatt_cb gatt_cb = {
    .att_mtu_updated = on_mtu_updated,
};

static int os_detection_ble_init(void) {
    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        profiles[i].index = i;
        k_work_init_delayable(&profiles[i].settle, settle_work_handler);
    }
    bt_conn_cb_register(&conn_cb);
    bt_gatt_cb_register(&gatt_cb);
    return 0;
}

SYS_INIT(os_detection_ble_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
