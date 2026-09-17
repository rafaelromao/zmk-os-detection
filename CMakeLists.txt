# Copyright (c) 2026 Rafael Romão
# SPDX-License-Identifier: MIT

# The Kconfig symbols already carry the central-or-unibody and per-transport
# conditions.
if(CONFIG_ZMK_OS_DETECTION)
  zephyr_include_directories(include)
  target_sources(app PRIVATE src/os_detection.c)

  if(CONFIG_ZMK_OS_DETECTION_USB)
    if(NOT CONFIG_USB_DEVICE_STACK)
      # Kconfig should have caught this. If it ever does not, fail here rather
      # than at link time with an unresolved __real_usb_handle_bos.
      message(FATAL_ERROR
        "ZMK_OS_DETECTION_USB watches enumeration by wrapping usb_handle_bos(), "
        "which only exists in Zephyr's legacy USB device stack "
        "(CONFIG_USB_DEVICE_STACK). This build does not have it -- it may have "
        "moved to the device_next stack. Set CONFIG_ZMK_OS_DETECTION_USB=n.")
    endif()
    target_sources(app PRIVATE src/os_detection_usb.c)
    zephyr_ld_options(-Wl,--wrap=usb_handle_bos)
  endif()

  if(CONFIG_ZMK_OS_DETECTION_BLE)
    target_sources(app PRIVATE src/os_detection_ble.c)
    zephyr_ld_options(-Wl,--wrap=bt_gatt_attr_read)
  endif()
endif()
