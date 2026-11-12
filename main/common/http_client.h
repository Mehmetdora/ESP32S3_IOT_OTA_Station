#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_http_client.h"

struct http_response_t {
    char *buffer;
    size_t length;
    size_t capacity;
    bool overflow;
    bool oom;
};

esp_http_client_config_t make_https_config(
    const char *url,
    http_response_t *response,
    int timeout_ms);

void response_free(http_response_t *response);
