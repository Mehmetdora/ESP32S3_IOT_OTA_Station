#include "ota_manager.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "device_id.h"
#include "http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static const char *TAG = "OTA";
static bool s_pending_verify = false;
static uint32_t s_boot_ms = 0;

static uint32_t now_ms()
{
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void rollback_init()
{
    s_boot_ms = now_ms();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        ESP_LOGW(TAG, "New firmware pending validation");
    }
}

static void rollback_mark_valid()
{
    if (!s_pending_verify) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "Firmware validation successful; rollback cancelled");
        s_pending_verify = false;
    }
}

static void rollback_check_timeout()
{
    if (!s_pending_verify) return;
    if (now_ms() - s_boot_ms < ROLLBACK_VALIDATE_TIMEOUT_MS) return;
    ESP_LOGE(TAG, "Firmware validation timeout; rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    s_pending_verify = false;
}

static bool is_hex_string(const char *s, size_t expected_len)
{
    if (!s || strlen(s) != expected_len) return false;
    for (size_t i = 0; i < expected_len; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static bool check_for_update(char *bin_url, size_t bin_url_size,
                             char *expected_sha, size_t expected_sha_size,
                             char *server_version, size_t server_version_size)
{
    char url[256] = {};
    snprintf(url, sizeof(url), "%s?version=%s", FIRMWARE_API_URL, FIRMWARE_VERSION);

    http_response_t response = {};
    auto config = make_https_config(url, &response, API_TIMEOUT_MS);
    auto client = esp_http_client_init(&config);
    if (!client) return false;

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || !response.buffer || response.overflow || response.oom) {
        response_free(&response);
        return false;
    }

    cJSON *root = cJSON_Parse(response.buffer);
    response_free(&response);
    if (!root) return false;

    const cJSON *available = cJSON_GetObjectItem(root, "update_available");
    const cJSON *version = cJSON_GetObjectItem(root, "version");
    snprintf(server_version, server_version_size, "%s",
             cJSON_IsString(version) ? version->valuestring : "");

    bool update = cJSON_IsTrue(available) &&
                  server_version[0] &&
                  strcmp(server_version, FIRMWARE_VERSION) != 0;

    if (update) {
        const cJSON *url_item = cJSON_GetObjectItem(root, "url");
        const cJSON *sha_item = cJSON_GetObjectItem(root, "sha256");
        if (!cJSON_IsString(url_item) || !cJSON_IsString(sha_item) ||
            strncmp(url_item->valuestring, "https://", 8) != 0 ||
            !is_hex_string(sha_item->valuestring, 64)) {
            update = false;
        } else {
            snprintf(bin_url, bin_url_size, "%s", url_item->valuestring);
            snprintf(expected_sha, expected_sha_size, "%s", sha_item->valuestring);
        }
    }

    cJSON_Delete(root);
    return update;
}

static bool ota_get_last_version(char *out, size_t size)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t len = size;
    esp_err_t err = nvs_get_str(handle, NVS_KEY_LAST_VER, out, &len);
    nvs_close(handle);
    return err == ESP_OK;
}

static void ota_set_last_version(const char *version)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_str(handle, NVS_KEY_LAST_VER, version) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

static void ota_clear_last_version()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_erase_key(handle, NVS_KEY_LAST_VER) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

static bool is_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static bool open_firmware_stream(esp_http_client_handle_t client, int *content_length_out)
{
    for (int attempt = 0; attempt <= MAX_REDIRECTS; ++attempt) {
        if (esp_http_client_open(client, 0) != ESP_OK) return false;
        int content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 200) {
            if (content_length <= 0) return false;
            *content_length_out = content_length;
            return true;
        }
        if (!is_redirect_status(status)) return false;
        if (esp_http_client_set_redirection(client) != ESP_OK) return false;
        char new_url[512] = {};
        if (esp_http_client_get_url(client, new_url, sizeof(new_url)) != ESP_OK ||
            strncmp(new_url, "https://", 8) != 0) return false;
        esp_http_client_close(client);
    }
    return false;
}

static bool perform_ota(const char *bin_url, const char *expected_sha)
{
    esp_http_client_handle_t client = nullptr;
    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    bool hash_started = false;
    uint8_t *buffer = nullptr;
    psa_hash_operation_t sha_op = PSA_HASH_OPERATION_INIT;
    const esp_partition_t *update_partition = nullptr;
    bool success = false;

    do {
        buffer = static_cast<uint8_t *>(malloc(OTA_BUFFER_SIZE));
        if (!buffer) break;

        auto config = make_https_config(bin_url, nullptr, OTA_TIMEOUT_MS);
        client = esp_http_client_init(&config);
        if (!client) break;

        int content_length = 0;
        if (!open_firmware_stream(client, &content_length)) break;

        update_partition = esp_ota_get_next_update_partition(nullptr);
        if (!update_partition || static_cast<uint32_t>(content_length) > update_partition->size)
            break;

        if (psa_hash_setup(&sha_op, PSA_ALG_SHA_256) != PSA_SUCCESS) break;
        hash_started = true;

        if (esp_ota_begin(update_partition, content_length, &ota_handle) != ESP_OK) break;
        ota_started = true;

        int total_read = 0;
        while (total_read < content_length) {
            int to_read = content_length - total_read;
            if (to_read > OTA_BUFFER_SIZE) to_read = OTA_BUFFER_SIZE;

            int read_len = esp_http_client_read(client, reinterpret_cast<char *>(buffer), to_read);
            if (read_len <= 0) break;
            if (psa_hash_update(&sha_op, buffer, read_len) != PSA_SUCCESS) break;
            if (esp_ota_write(ota_handle, buffer, read_len) != ESP_OK) break;
            total_read += read_len;
        }

        if (total_read != content_length) break;

        uint8_t hash[32] = {};
        size_t hash_len = 0;
        if (psa_hash_finish(&sha_op, hash, sizeof(hash), &hash_len) != PSA_SUCCESS || hash_len != 32)
            break;
        hash_started = false;

        char calculated_hex[65] = {};
        for (int i = 0; i < 32; ++i)
            snprintf(calculated_hex + i * 2, 3, "%02x", hash[i]);

        if (strcasecmp(calculated_hex, expected_sha) != 0) {
            ESP_LOGE(TAG, "SHA-256 mismatch; OTA aborted");
            break;
        }

        if (esp_ota_end(ota_handle) != ESP_OK) {
            ota_started = false;
            break;
        }
        ota_started = false;

        if (esp_ota_set_boot_partition(update_partition) != ESP_OK) break;
        success = true;
    } while (false);

    if (hash_started) psa_hash_abort(&sha_op);
    if (ota_started) esp_ota_abort(ota_handle);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(buffer);
    return success;
}

static void send_heartbeat()
{
    if (!wifi_manager_is_connected()) return;

    char device_id[32] = {};
    get_device_id(device_id, sizeof(device_id));

    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return;

    http_response_t response = {};
    auto config = make_https_config(HEARTBEAT_URL, &response, API_TIMEOUT_MS);
    auto client = esp_http_client_init(&config);
    if (!client) { free(body); return; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    ESP_LOGI(TAG, "Heartbeat HTTP=%d", status);

    esp_http_client_cleanup(client);
    response_free(&response);
    free(body);
}

void ota_manager_task(void *arg)
{
    rollback_init();

    while (true) {
        if (!wifi_manager_wait_ready()) {
            rollback_check_timeout();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        char bin_url[512] = {};
        char expected_sha[65] = {};
        char server_version[64] = {};

        bool update_available = check_for_update(
            bin_url, sizeof(bin_url), expected_sha, sizeof(expected_sha),
            server_version, sizeof(server_version));

        // Any successful firmware API response validates the pending image,
        // same behavior as the existing working firmware.
        rollback_mark_valid();

        if (update_available) {
            char last_version[64] = {};
            bool already_attempted = ota_get_last_version(last_version, sizeof(last_version)) &&
                                     strcmp(last_version, server_version) == 0;

            if (!already_attempted) {
                ESP_LOGI(TAG, "New firmware: %s", server_version);
                if (perform_ota(bin_url, expected_sha)) {
                    ota_set_last_version(server_version);
                    ESP_LOGI(TAG, "OTA successful; restarting");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                } else {
                    ESP_LOGE(TAG, "OTA failed");
                }
            }
        } else {
            ota_clear_last_version();
        }

        send_heartbeat();
        rollback_check_timeout();
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
    }
}
