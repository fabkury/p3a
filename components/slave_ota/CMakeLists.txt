idf_component_register(
    SRCS "slave_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition app_update freertos
    PRIV_REQUIRES log esp_hosted esp_system
)

# The esp_hosted component provides the OTA APIs
# It's included via the main project's dependencies

# Embed slave firmware binary into the slave_fw partition
set(SLAVE_FW_BIN "${CMAKE_CURRENT_SOURCE_DIR}/firmware/network_adapter.bin")
if(EXISTS ${SLAVE_FW_BIN})
    message(STATUS "Slave firmware found: ${SLAVE_FW_BIN}")
    # This will be flashed to the slave_fw partition
    esptool_py_flash_to_partition(flash "slave_fw" "${SLAVE_FW_BIN}")
else()
    message(WARNING "Slave firmware not found at ${SLAVE_FW_BIN}")
    message(WARNING "Build the slave project first: cd ../slave && idf.py build")
endif()

