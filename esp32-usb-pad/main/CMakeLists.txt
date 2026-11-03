# macro_script.h is generated from PicoSwitch/payload.txt at build time;
# editing payload.txt and rebuilding is enough to update the macro.
get_filename_component(ESP32PAD_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(PAYLOAD_FILE "${ESP32PAD_DIR}/../PicoSwitch/payload.txt"
    CACHE FILEPATH "Chat-XSS payload.txt to embed as the HID macro")
set(MACRO_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/macro_script.h")

idf_component_register(
  SRCS "main.c"
  INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
  REQUIRES esp_tinyusb freertos esp_timer
)

set(LANG_JP 1 CACHE STRING "Enable JP IME Alt+Space before each line")
set(MACRO_REPEAT 1 CACHE STRING "Repeat the macro forever")
set(MACRO_REPEAT_DELAY_MS 5000 CACHE STRING "Delay between macro rounds")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
  LANG_JP=${LANG_JP}
  MACRO_REPEAT=${MACRO_REPEAT}
  MACRO_REPEAT_DELAY_MS=${MACRO_REPEAT_DELAY_MS}
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
