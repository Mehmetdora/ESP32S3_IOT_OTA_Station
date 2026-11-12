#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "wifi_manager.h"
#include "aht15.h"
#include "telemetry.h"
#include "ota_manager.h"
#include "app_config.h"
#include <time.h>
#include <stdlib.h>

static const char *TAG = "MAIN";        // Log dosyalarının başına MAIN eklenmesi için

static void sensor_task(void *arg)
{
    QueueHandle_t queue = telemetry_get_queue();
    aht15_data_t data = {};

    if (!aht15_init()) {
        ESP_LOGE(TAG, "AHT15 initialization failed");
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        // Time synchronization is required before timestamping a sample.
        if (!wifi_manager_wait_ready()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (aht15_read(&data)) {
            sensor_sample_t sample = {};
            sample.temperature_c = data.temperature_c;
            sample.humidity_percent = data.humidity_percent;
            sample.measured_at = time(nullptr);

            // Do not block the sensor task if telemetry/network is busy.
            if (xQueueSend(queue, &sample, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Telemetry queue full; sample dropped");
            } else {
                ESP_LOGI(TAG, "Sensor: %.2f C, %.2f %%RH",
                         data.temperature_c, data.humidity_percent);
            }
        } else {
            ESP_LOGW(TAG, "AHT15 read failed");
        }

        vTaskDelay(pdMS_TO_TICKS(AHT15_SAMPLE_INTERVAL_MS));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=============================");
    ESP_LOGI(TAG, "ESP32-S3 WEATHER / OTA");
    ESP_LOGI(TAG, "Firmware: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "=============================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    // Turkey is UTC+3. Laravel receives the explicit +0300 offset.
    setenv("TZ", "TRT-3", 1);
    tzset();

    wifi_manager_init();

    if (!telemetry_init()) {
        ESP_LOGE(TAG, "Telemetry initialization failed");
        return;
    }

    BaseType_t ok;

    ok = xTaskCreate(
        sensor_task, "sensor_task", SENSOR_TASK_STACK_SIZE,
        nullptr, SENSOR_TASK_PRIORITY, nullptr);
    if (ok != pdPASS) ESP_LOGE(TAG, "sensor_task creation failed");

    ok = xTaskCreate(
        telemetry_task, "telemetry_task", TELEMETRY_TASK_STACK_SIZE,
        nullptr, TELEMETRY_TASK_PRIORITY, nullptr);
    if (ok != pdPASS) ESP_LOGE(TAG, "telemetry_task creation failed");

    ok = xTaskCreate(
        ota_manager_task, "ota_task", OTA_TASK_STACK_SIZE,
        nullptr, OTA_TASK_PRIORITY, nullptr);
    if (ok != pdPASS) ESP_LOGE(TAG, "ota_task creation failed");
}
