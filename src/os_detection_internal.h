/*
 * Copyright (c) 2026 Rafael Romão
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/os_detection.h>

/*
 * What each transport counts, and how those counts become a guess.
 *
 * Kept free of Zephyr and ZMK headers so the rules can be exercised on a host
 * compiler -- the fingerprints are the part most likely to need correcting
 * against a real machine, and they are the part hardest to reach on one.
 */

/* --- USB --------------------------------------------------------------- */

struct zmk_os_usb_stats {
    /* GET_DESCRIPTOR(STRING) requests, split by the length asked for. */
    uint16_t string_probe;  /* wLength == 2: a header probe */
    uint16_t string_full;   /* wLength == 255: the whole buffer, unprompted */
    uint16_t string_other;  /* anything else: a re-read at the advertised size */

    /* Recorded for the log line only; nothing below reads them. */
    uint16_t bos_wlength;
    bool bos_seen;
};

/*
 * macOS reads a descriptor twice: a 2-byte probe for the length byte, then a
 * re-read at exactly that length. Linux reads every string straight into a
 * 255-byte buffer and never probes.
 *
 * The probe is what carries the signal, so it is checked first and its
 * absence is what the Linux rule requires -- a host that probes AND happens
 * to ask for 255 somewhere must not fall through to Linux.
 */
static inline enum zmk_os zmk_os_classify_usb(const struct zmk_os_usb_stats *s) {
    if (s->string_probe > 0 && s->string_other > 0) {
        return ZMK_OS_MACOS;
    }
    if (s->string_full > 0 && s->string_probe == 0) {
        return ZMK_OS_LINUX;
    }
    return ZMK_OS_UNKNOWN;
}

/* --- BLE --------------------------------------------------------------- */

struct zmk_os_ble_stats {
    uint16_t report_map; /* HIDS Report Map */
    uint16_t hids_info;  /* HIDS Information */
    uint16_t pnp_id;     /* DIS PnP ID */
    uint16_t appearance; /* GAP Appearance */

    /* Recorded for the log line only; nothing below reads them. */
    uint16_t att_mtu;
};

/*
 * A BlueZ desktop walks the whole tree -- GAP Appearance and the DIS PnP ID
 * as well as the HID report map. macOS takes the report map and ignores both.
 *
 * Much weaker than the USB rule, and knowingly wrong for one case: an Android
 * phone also skips Appearance, so it reads as macOS here. That is accepted
 * rather than fixed, because telling those two apart needs a signal this
 * module does not collect, and neither is a host this keyboard is built for.
 */
static inline enum zmk_os zmk_os_classify_ble(const struct zmk_os_ble_stats *s) {
    if (s->appearance > 0 || s->pnp_id > 0) {
        return ZMK_OS_LINUX;
    }
    if (s->report_map > 0) {
        return ZMK_OS_MACOS;
    }
    return ZMK_OS_UNKNOWN;
}

/* --- reporting --------------------------------------------------------- */

/* Called by a transport once its fingerprint has gone quiet. */
void zmk_os_detection_report_usb(enum zmk_os detected);
void zmk_os_detection_report_ble(uint8_t profile_index, enum zmk_os detected);
