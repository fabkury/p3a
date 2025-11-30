#pragma once

/**
 * @brief Get CA certificate for HTTPS provisioning endpoint
 * 
 * @return Pointer to PEM-formatted certificate string
 */
const char* makapix_get_provision_ca_cert(void);

/**
 * @brief Get CA certificate for MQTT broker
 * 
 * @return Pointer to PEM-formatted certificate string
 */
const char* makapix_get_mqtt_ca_cert(void);

