#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct sensor_sample_t {
    float temperature_c;
    float humidity_percent;
    int rssi;
    time_t measured_at;
};

bool telemetry_init();
QueueHandle_t telemetry_get_queue();
void telemetry_task(void *arg);
