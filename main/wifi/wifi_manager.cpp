#include "wifi_manager.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "app_config.h"
#include "secrets.h"

static const char *TAG = "WIFI";
static EventGroupHandle_t s_wifi_events = nullptr;
static SemaphoreHandle_t s_time_mutex = nullptr;
static bool s_sntp_started = false;
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;

static void event_handler(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d; reconnecting",
                 event ? static_cast<int>(event->reason) : -1);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "WiFi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

void wifi_manager_init()
{
    s_wifi_events = xEventGroupCreate();
    s_time_mutex = xSemaphoreCreateMutex();
    if (!s_wifi_events || !s_time_mutex) {
        ESP_LOGE(TAG, "Failed to create WiFi synchronization objects");
        esp_restart();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr, nullptr));

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), WIFI_SSID,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(wifi_config.sta.password), WIFI_PASS,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool wifi_manager_is_connected()
{
    return s_wifi_events &&
           (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT);
}

static bool ensure_time_synced()
{
#if USE_INSECURE_TLS
    return true;
#else
    if (xSemaphoreTake(s_time_mutex, pdMS_TO_TICKS(TIME_SYNC_WAIT_MS)) != pdTRUE)
        return false;

    time_t now = 0;
    time(&now);
    if (now > MIN_VALID_EPOCH) {
        xSemaphoreGive(s_time_mutex);
        return true;
    }

    if (!s_sntp_started) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&cfg) != ESP_OK) {
            xSemaphoreGive(s_time_mutex);
            return false;
        }
        s_sntp_started = true;
    }

    ESP_LOGI(TAG, "Waiting for SNTP time synchronization...");
    esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_WAIT_MS));
    time(&now);

    bool ok = now > MIN_VALID_EPOCH;
    xSemaphoreGive(s_time_mutex);
    return ok;
#endif
}

bool wifi_manager_wait_ready()
{
    if (!s_wifi_events) return false;

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(WIFI_WAIT_MS));

    if (!(bits & WIFI_CONNECTED_BIT)) return false;
    return ensure_time_synced();
}
