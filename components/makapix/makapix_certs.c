#include "makapix_certs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "makapix_certs";

// Embedded certificates - these will be loaded from files at build time
// For now, using placeholders that should be replaced with actual certificates

// CA certificate for HTTPS provisioning endpoint
// Note: MQTT certificates are now retrieved from API and stored in SPIFFS
static const char makapix_ca_cert_pem[] = 
#include "certs/makapix_ca_cert.inc"
;

const char* makapix_get_provision_ca_cert(void)
{
    ESP_LOGI(TAG, "CA cert length: %d bytes", strlen(makapix_ca_cert_pem));
    ESP_LOGD(TAG, "CA cert starts: %.64s...", makapix_ca_cert_pem);
    return makapix_ca_cert_pem;
}

