#include "device_id.h"
#include <stdio.h>
#include "esp_err.h"
#include "esp_mac.h"

void get_device_id(char *buffer, size_t buffer_size)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);

    if (err != ESP_OK) {
        snprintf(buffer, buffer_size, "UNKNOWN");
        return;
    }

    // Keep the same byte order used by the existing firmware.
    snprintf(buffer, buffer_size,
             "%02X%02X%02X%02X%02X%02X",
             mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
}
