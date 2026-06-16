zephyr_library()

zephyr_include_directories(include)
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)

if(CONFIG_ZMK_SPLT_PERIPHERAL_OUTPUT_RELAY)
  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(src/output_relay_central.c)
  endif()

  if(CONFIG_ZMK_SPLIT AND (NOT CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
    zephyr_library_sources(src/output_relay_peripheral.c)
  endif()
endif()

if(CONFIG_RAW_HID)
  zephyr_library_sources(src/events.c)
  zephyr_library_sources(src/usb_hid.c)

  # Only include BLE HOG transport on non-split or central builds; peripherals use the relay.
  if(CONFIG_ZMK_BLE AND (NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
    zephyr_library_sources(src/hog.c)
  endif()

  if(CONFIG_RAW_HID_FORWARD_TO_PERIPHERAL AND CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(src/split_bridge.c)
  endif()
endif()

if(CONFIG_NICE_VIEW_HID)
  if(CONFIG_RAW_HID)
    zephyr_library_sources(src/hid.c)
  endif()

  zephyr_library_sources(src/custom_status_screen.c)
  zephyr_library_sources(src/widgets/util.c)

  if(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(src/widgets/status.c)
  else()
    zephyr_library_sources(src/widgets/peripheral_status.c)
  endif()
endif()
