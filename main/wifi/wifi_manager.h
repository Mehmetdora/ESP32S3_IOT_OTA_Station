#pragma once
#include <stdbool.h>

void wifi_manager_init();
bool wifi_manager_is_connected();
bool wifi_manager_wait_ready();
