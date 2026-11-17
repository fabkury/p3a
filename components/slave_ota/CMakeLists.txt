# Layer: board. Keeps the ESP32-C6 Wi-Fi co-processor's firmware current: at
# boot it compares what the C6 runs with the image in the slave_fw partition
# and updates the C6 through esp_hosted when it is older.
idf_component_register(
    SRCS "slave_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition app_update
    PRIV_REQUIRES log esp_hosted esp_system p3a_util
)

# The C6 image ships inside the P4's flash, in the slave_fw partition.
# network_adapter.bin is a prebuilt binary; it is not built by this project.
set(SLAVE_FW_BIN "${CMAKE_CURRENT_SOURCE_DIR}/firmware/network_adapter.bin")
if(EXISTS ${SLAVE_FW_BIN})
    esptool_py_flash_to_partition(flash "slave_fw" "${SLAVE_FW_BIN}")
    # esptool v5 (IDF 5.5.2+) validates write_flash files against the target
    # chip and refuses network_adapter.bin, an ESP32-C6 image that sits in the
    # P4's slave_fw partition by design. --force skips that check. Replace with
    # a first-class option once upstream has one:
    # https://github.com/espressif/esp-idf/issues/18695
    set_property(TARGET flash APPEND PROPERTY SUB_ARGS "--force")
else()
    message(FATAL_ERROR "Co-processor firmware not found at ${SLAVE_FW_BIN}")
endif()
