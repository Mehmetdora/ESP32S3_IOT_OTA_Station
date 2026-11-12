#pragma once

// ---------------- Firmware ----------------
#define FIRMWARE_VERSION "1.2.0"

// ---------------- Server ----------------
#define API_BASE_URL "https://www.mehmetdora.me"
#define FIRMWARE_API_URL API_BASE_URL "/api/esp32/firmware"
#define HEARTBEAT_URL API_BASE_URL "/api/esp32/device/heartbeat"
#define TELEMETRY_URL API_BASE_URL "/api/esp32/telemetry"

// ---------------- Sensor ----------------
// XIAO ESP32-S3 Sense / common default I2C pins.
#define AHT15_SDA_GPIO 5
#define AHT15_SCL_GPIO 6
#define AHT15_I2C_PORT I2C_NUM_0
#define AHT15_I2C_FREQ_HZ 100000
#define AHT15_I2C_TIMEOUT_MS 1000
#define AHT15_SAMPLE_INTERVAL_MS 10000

// ---------------- Network ----------------
#define WIFI_WAIT_MS 30000
#define TIME_SYNC_WAIT_MS 15000
#define API_TIMEOUT_MS 10000
#define OTA_TIMEOUT_MS 20000
#define MIN_VALID_EPOCH 1700000000

// ---------------- Telemetry ----------------
#define TELEMETRY_QUEUE_LENGTH 8
#define TELEMETRY_TASK_STACK_SIZE 8192
#define TELEMETRY_TASK_PRIORITY 4
#define SENSOR_TASK_STACK_SIZE 4096
#define SENSOR_TASK_PRIORITY 4

// ---------------- OTA ----------------
#define OTA_CHECK_INTERVAL_MS 60000
#define OTA_BUFFER_SIZE 4096
#define OTA_TASK_STACK_SIZE 12288
#define OTA_TASK_PRIORITY 5
#define MAX_API_RESPONSE_BYTES 8192
#define MAX_REDIRECTS 3
#define ROLLBACK_VALIDATE_TIMEOUT_MS 300000
#define NVS_NAMESPACE "ota"
#define NVS_KEY_LAST_VER "last_ver"

// 0 = validate server certificate using ESP-IDF certificate bundle.
#define USE_INSECURE_TLS 0
