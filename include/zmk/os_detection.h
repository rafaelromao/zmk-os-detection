/*
 * Copyright (c) 2026 Rafael Romão
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/*
 * Only the OSes this module can actually tell apart. Deliberately not an
 * exhaustive list of operating systems: a name here is a promise that some
 * fingerprint returns it. Windows and the mobile OSes are absent because
 * nothing in src/os_detection_{usb,ble}.c can name them -- see the README.
 *
 * The names double as devicetree tokens: "macos" in an activate-for list
 * becomes ZMK_OS_MACOS at build time, so adding a case here and a rule to a
 * classifier is all it takes to make a new name usable in a keymap.
 */
enum zmk_os {
    ZMK_OS_UNKNOWN = 0,
    ZMK_OS_MACOS,
    ZMK_OS_LINUX,
};

/**
 * The OS detected for the endpoint that is currently selected.
 *
 * ZMK_OS_UNKNOWN when nothing has been detected for it yet, when the
 * fingerprint did not match anything, or when no endpoint is selected.
 */
enum zmk_os zmk_os_detection_current(void);
