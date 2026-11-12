#pragma once
#include <stdbool.h>
#include "driver/i2c_master.h"

struct aht15_data_t {
    float temperature_c;
    float humidity_percent;
};

bool aht15_init();
bool aht15_read(aht15_data_t *data);
