#include "http_client.h"
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "HTTP";

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    auto *response = static_cast<http_response_t *>(evt->user_data);
    if (!response) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        if (esp_http_client_get_status_code(evt->client) < 200 ||
            esp_http_client_get_status_code(evt->client) >= 300) {
            return ESP_OK;
        }

        size_t needed = response->length + static_cast<size_t>(evt->data_len) + 1;
        if (needed > MAX_API_RESPONSE_BYTES) {
            response->overflow = true;
            return ESP_OK;
        }

        if (needed > response->capacity) {
            size_t new_capacity = response->capacity ? response->capacity : 512;
            while (new_capacity < needed) new_capacity *= 2;
            if (new_capacity > MAX_API_RESPONSE_BYTES)
                new_capacity = MAX_API_RESPONSE_BYTES;

            char *new_buffer = static_cast<char *>(realloc(response->buffer, new_capacity));
            if (!new_buffer) {
                response->oom = true;
                ESP_LOGE(TAG, "HTTP response memory allocation failed");
                return ESP_OK;
            }
            response->buffer = new_buffer;
            response->capacity = new_capacity;
        }

        memcpy(response->buffer + response->length, evt->data, evt->data_len);
        response->length += evt->data_len;
        response->buffer[response->length] = '\0';
    }

    return ESP_OK;
}

esp_http_client_config_t make_https_config(
    const char *url,
    http_response_t *response,
    int timeout_ms)
{
    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.user_data = response;
    config.timeout_ms = timeout_ms;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;

#if USE_INSECURE_TLS
    config.skip_cert_common_name_check = true;
#else
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    return config;
}

void response_free(http_response_t *response)
{
    if (!response) return;
    free(response->buffer);
    response->buffer = nullptr;
    response->length = 0;
    response->capacity = 0;
    response->overflow = false;
    response->oom = false;
}
