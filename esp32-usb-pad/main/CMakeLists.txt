# macro_script.h is generated from PicoSwitch/payload.txt at build time;
# editing payload.txt and rebuilding is enough to update the macro.
get_filename_component(ESP32PAD_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(PAYLOAD_FILE "${ESP32PAD_DIR}/../PicoSwitch/payload.txt"
    CACHE FILEPATH "Chat-XSS payload.txt to embed as the HID macro")
set(MACRO_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/macro_script.h")

idf_component_register(
  EMBED_TXTFILES "web/index.html"
  SRCS "main.c" "hid_pad.c" "net_api.c"
  INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
  REQUIRES esp_tinyusb freertos esp_timer esp_wifi esp_netif esp_event
           esp_http_server nvs_flash lwip app_update
)

# Standalone mode (no network): run the macro by itself after USB enumeration.
set(MACRO_REPEAT 0 CACHE STRING "Repeat the macro forever at boot")
set(MACRO_REPEAT_DELAY_MS 5000 CACHE STRING "Delay between macro rounds")
set(NETPAD_SKIP_USB 0 CACHE STRING "Skip tinyusb init (debug console builds)")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
  MACRO_REPEAT=${MACRO_REPEAT}
  MACRO_REPEAT_DELAY_MS=${MACRO_REPEAT_DELAY_MS}
  NETPAD_SKIP_USB=${NETPAD_SKIP_USB}
)

# add_custom_command is not scriptable, and the component library target does
# not exist yet during IDF's early requirements expansion.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  add_custom_command(
    OUTPUT ${MACRO_SCRIPT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}
    COMMAND ${Python3_EXECUTABLE}
            ${ESP32PAD_DIR}/tools/payload_to_esp32.py
            ${PAYLOAD_FILE} ${MACRO_SCRIPT}
    DEPENDS ${PAYLOAD_FILE} ${ESP32PAD_DIR}/tools/payload_to_esp32.py
    COMMENT "Generating macro_script.h from payload.txt"
  )

  add_custom_target(macro_script DEPENDS ${MACRO_SCRIPT})
  add_dependencies(${COMPONENT_LIB} macro_script)
endif()
