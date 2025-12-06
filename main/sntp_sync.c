#include "sntp_sync.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include <string.h>
#include <time.h>

static const char *TAG = "sntp_sync";
static bool s_synchronized = false;

static void sntp_sync_time_cb(struct timeval *tv)
{
    s_synchronized = true;
    ESP_LOGI(TAG, "Time synchronized: %s", ctime(&tv->tv_sec));
}

esp_err_t sntp_sync_init(void)
{
    if (s_synchronized) {
        ESP_LOGI(TAG, "SNTP already synchronized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SNTP");

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = sntp_sync_time_cb;
    config.start = true;
    config.renew_servers_after_new_IP = true;
    config.index_of_first_server = 0;
    config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    config.smooth_sync = false;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SNTP: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SNTP initialized, waiting for synchronization...");
    return ESP_OK;
}

bool sntp_sync_is_synchronized(void)
{
    return s_synchronized;
}

esp_err_t sntp_sync_get_iso8601(char *buf, size_t len)
{
    if (!buf || len < 21) { // ISO8601 needs at least 20 chars + null
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_synchronized) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    gmtime_r(&now, &timeinfo);  // Use gmtime_r for UTC (Z suffix)

    // Format as ISO 8601: YYYY-MM-DDTHH:MM:SSZ (UTC)
    int ret = snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                       timeinfo.tm_year + 1900,
                       timeinfo.tm_mon + 1,
                       timeinfo.tm_mday,
                       timeinfo.tm_hour,
                       timeinfo.tm_min,
                       timeinfo.tm_sec);

    if (ret < 0 || (size_t)ret >= len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

void sntp_sync_stop(void)
{
    esp_netif_sntp_deinit();
    s_synchronized = false;
    ESP_LOGI(TAG, "SNTP stopped");
}

