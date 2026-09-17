/*
 * Copyright (c) 2026 Rafael Romão
 *
 * SPDX-License-Identifier: MIT
 *
 * The classifiers are the part of this module most likely to be wrong, and
 * the part hardest to reach on real hardware. They are kept free of Zephyr so
 * they can be exercised with nothing but a host compiler:
 *
 *     cc -I include -I src -o /tmp/t tests/test_classify.c && /tmp/t
 *
 * The sequences below are what each host is expected to do; if a capture from
 * a real machine disagrees, change the table, then the rule.
 */

#include <stdio.h>
#include <string.h>

#include "os_detection_internal.h"

static int failures;

static const char *os_name(enum zmk_os os) {
    switch (os) {
    case ZMK_OS_MACOS:
        return "macos";
    case ZMK_OS_LINUX:
        return "linux";
    default:
        return "unknown";
    }
}

static void check(const char *what, enum zmk_os got, enum zmk_os want) {
    if (got == want) {
        printf("  ok    %-42s -> %s\n", what, os_name(got));
        return;
    }
    printf("  FAIL  %-42s -> %s (wanted %s)\n", what, os_name(got), os_name(want));
    failures++;
}

/* --- USB ---------------------------------------------------------------- */

/* A 2-byte probe then a re-read at the advertised length, per descriptor. */
static void usb_macos(struct zmk_os_usb_stats *s) {
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 3; i++) {
        s->string_probe++; /* wLength = 2 */
        s->string_other++; /* wLength = 24, 24, 34 */
    }
    s->bos_seen = true;
    s->bos_wlength = 5;
}

/* Every string straight into the full buffer, including the language list. */
static void usb_linux(struct zmk_os_usb_stats *s) {
    memset(s, 0, sizeof(*s));
    s->string_full = 4; /* wLength = 255 each time */
    s->bos_seen = true;
    s->bos_wlength = 12;
}

/* --- BLE ---------------------------------------------------------------- */

static void ble_macos(struct zmk_os_ble_stats *s) {
    memset(s, 0, sizeof(*s));
    s->report_map = 1;
    s->att_mtu = 527;
}

static void ble_linux(struct zmk_os_ble_stats *s) {
    memset(s, 0, sizeof(*s));
    s->report_map = 1;
    s->hids_info = 1;
    s->pnp_id = 1;
    s->appearance = 1;
    s->att_mtu = 517;
}

int main(void) {
    struct zmk_os_usb_stats u;
    struct zmk_os_ble_stats b;

    printf("USB:\n");
    usb_macos(&u);
    check("macOS enumeration", zmk_os_classify_usb(&u), ZMK_OS_MACOS);
    usb_linux(&u);
    check("Linux enumeration", zmk_os_classify_usb(&u), ZMK_OS_LINUX);

    memset(&u, 0, sizeof(u));
    check("nothing observed", zmk_os_classify_usb(&u), ZMK_OS_UNKNOWN);

    /* A host cut off mid-enumeration: one probe, no re-read. Neither rule
     * should claim it -- an unknown host must stay unknown rather than fall
     * through to Linux. */
    memset(&u, 0, sizeof(u));
    u.string_probe = 1;
    check("probe with no re-read", zmk_os_classify_usb(&u), ZMK_OS_UNKNOWN);

    /* A probing host that also asked for 255 somewhere must not read as
     * Linux: the probe is the signal, and the Linux rule requires none. */
    memset(&u, 0, sizeof(u));
    u.string_probe = 1;
    u.string_other = 1;
    u.string_full = 1;
    check("probe plus a 255-byte read", zmk_os_classify_usb(&u), ZMK_OS_MACOS);

    printf("BLE:\n");
    ble_macos(&b);
    check("macOS discovery", zmk_os_classify_ble(&b), ZMK_OS_MACOS);
    ble_linux(&b);
    check("Linux discovery", zmk_os_classify_ble(&b), ZMK_OS_LINUX);

    memset(&b, 0, sizeof(b));
    check("nothing observed", zmk_os_classify_ble(&b), ZMK_OS_UNKNOWN);

    /* Battery polls and nothing else: no fingerprint attribute was read, so
     * there is nothing to go on. */
    memset(&b, 0, sizeof(b));
    b.att_mtu = 247;
    check("mtu only, no reads", zmk_os_classify_ble(&b), ZMK_OS_UNKNOWN);

    /* Appearance alone is enough -- PnP ID is optional on the host side. */
    memset(&b, 0, sizeof(b));
    b.report_map = 1;
    b.appearance = 1;
    check("appearance without pnp id", zmk_os_classify_ble(&b), ZMK_OS_LINUX);

    printf("\n%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
