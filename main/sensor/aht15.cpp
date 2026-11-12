#include "aht15.h"
#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "AHT15";
static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;

static bool write_command(const uint8_t *cmd, size_t len)
{
    return i2c_master_transmit(s_dev, cmd, len, AHT15_I2C_TIMEOUT_MS) == ESP_OK;
}

bool aht15_init()
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = AHT15_I2C_PORT;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(AHT15_SDA_GPIO);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(AHT15_SCL_GPIO);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = 0x38;
    dev_cfg.scl_speed_hz = AHT15_I2C_FREQ_HZ;

    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AHT15 device init failed: %s", esp_err_to_name(err));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(40));

    uint8_t init_cmd[] = {0xE1, 0x08, 0x00};
    if (!write_command(init_cmd, sizeof(init_cmd))) {
        ESP_LOGW(TAG, "AHT15 initialization command failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "AHT15 initialized at 0x38, SDA=%d SCL=%d",
             AHT15_SDA_GPIO, AHT15_SCL_GPIO);
    return true;
}

bool aht15_read(aht15_data_t *data)
{
    if (!s_dev || !data) return false;

    // Trigger measurement: 0xAC, 0x33, 0x00.
    uint8_t trigger[] = {0xAC, 0x33, 0x00};
    if (!write_command(trigger, sizeof(trigger))) return false;

    vTaskDelay(pdMS_TO_TICKS(80));

    uint8_t raw[6] = {};
    bool ready = false;

    // AHT15 may still be busy after the minimum conversion delay.
    // Poll the sensor status by reading the 6-byte measurement frame.
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (i2c_master_receive(s_dev, raw, sizeof(raw), AHT15_I2C_TIMEOUT_MS) != ESP_OK)
            return false;

        if ((raw[0] & 0x80) == 0) {
            ready = true;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!ready) {
        ESP_LOGW(TAG, "AHT15 measurement timeout");
        return false;
    }

    uint32_t humidity_raw =
        (static_cast<uint32_t>(raw[1]) << 12) |
        (static_cast<uint32_t>(raw[2]) << 4) |
        (static_cast<uint32_t>(raw[3]) >> 4);

    uint32_t temperature_raw =
        ((static_cast<uint32_t>(raw[3]) & 0x0F) << 16) |
        (static_cast<uint32_t>(raw[4]) << 8) |
        raw[5];

    data->humidity_percent =
        (static_cast<float>(humidity_raw) * 100.0f) / 1048576.0f;

    data->temperature_c =
        (static_cast<float>(temperature_raw) * 200.0f) / 1048576.0f - 50.0f;

    return true;
}
