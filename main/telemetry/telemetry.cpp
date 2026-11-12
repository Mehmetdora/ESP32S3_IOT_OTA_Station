#include "telemetry.h"
#include "app_config.h"
#include "secrets.h"
#include "wifi_manager.h"
#include "device_id.h"
#include "http_client.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "TELEMETRY";
static QueueHandle_t s_queue = nullptr;

bool telemetry_init()
{
    s_queue = xQueueCreate(TELEMETRY_QUEUE_LENGTH, sizeof(sensor_sample_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Telemetry queue creation failed");
        return false;
    }
    return true;
}

QueueHandle_t telemetry_get_queue()
{
    return s_queue;
}

static int get_rssi()
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        return ap.rssi;
    return 0;
}

static bool post_sample(const sensor_sample_t &sample)
{
    char device_id[32] = {};
    get_device_id(device_id, sizeof(device_id));

    char timestamp[40] = {};
    struct tm tm_value = {};
    localtime_r(&sample.measured_at, &tm_value);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &tm_value);

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);
    cJSON_AddNumberToObject(root, "temperature", sample.temperature_c);
    cJSON_AddNumberToObject(root, "humidity", sample.humidity_percent);
    cJSON_AddNumberToObject(root, "rssi", sample.rssi);
    cJSON_AddStringToObject(root, "measured_at", timestamp);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;

    http_response_t response = {};
    auto config = make_https_config(TELEMETRY_URL, &response, API_TIMEOUT_MS);
    auto client = esp_http_client_init(&config);
    if (!client) {
        free(body);
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Authorization", "Bearer " TELEMETRY_API_TOKEN);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;

    if (err == ESP_OK && status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Telemetry sent: %.2f C, %.2f %%RH, HTTP %d",
                 sample.temperature_c, sample.humidity_percent, status);
    } else {
        ESP_LOGW(TAG, "Telemetry failed: err=%s HTTP=%d",
                 esp_err_to_name(err), status);
    }

    esp_http_client_cleanup(client);
    response_free(&response);
    free(body);
    return err == ESP_OK && status >= 200 && status < 300;
}

void telemetry_task(void *arg)
{
    sensor_sample_t sample = {};

    while (true) {
        if (!wifi_manager_wait_ready()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (xQueueReceive(s_queue, &sample, pdMS_TO_TICKS(2000)) == pdTRUE) {
            sample.rssi = get_rssi();
            post_sample(sample);
        }
    }
}
