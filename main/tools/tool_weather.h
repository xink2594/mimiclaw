#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * @brief Execute the weather_now tool
 * Fetches current weather for a specified location via Seniverse API.
 */
esp_err_t tool_weather_now_execute(const char *input_json, char *output, size_t output_size);

/**
 * @brief Execute the weather_forecast tool
 * Fetches 3-day weather forecast (today, tomorrow, day after) for a location.
 */
esp_err_t tool_weather_daily_execute(const char *input_json, char *output, size_t output_size);