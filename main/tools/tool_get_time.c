#include "tool_get_time.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "tool_time";

static const char *MONTHS[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/* Parse "Sat, 01 Feb 2025 10:25:00 GMT" → set system clock, return formatted string */
static bool parse_and_set_time(const char *date_str, char *out, size_t out_size)
{
    int day, year, hour, min, sec;
    char mon_str[4] = {0};

    if (sscanf(date_str, "%*[^,], %d %3s %d %d:%d:%d",
               &day, mon_str, &year, &hour, &min, &sec) != 6)
    {
        return false;
    }

    int mon = -1;
    for (int i = 0; i < 12; i++)
    {
        if (strcmp(mon_str, MONTHS[i]) == 0)
        {
            mon = i;
            break;
        }
    }
    if (mon < 0)
        return false;

    struct tm tm = {
        .tm_sec = sec,
        .tm_min = min,
        .tm_hour = hour,
        .tm_mday = day,
        .tm_mon = mon,
        .tm_year = year - 1900,
    };

    /* Convert UTC to epoch — mktime expects local, so temporarily set UTC */
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(&tm);

    /* Restore timezone */
    setenv("TZ", MIMI_TIMEZONE, 1);
    tzset();

    if (t < 0)
        return false;

    struct timeval tv = {.tv_sec = t};
    settimeofday(&tv, NULL);

    /* Format in local time */
    struct tm local;
    localtime_r(&t, &local);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S %Z (%A)", &local);

    return true;
}

/* 从 ESP32 硬件时钟获取时间 */
static esp_err_t fetch_time_direct(char *out, size_t out_size)
{
    // 直接读取 ESP32 内部硬件时钟（已经被 NTP 自动校准）
    time_t now;
    time(&now);

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    // 如果年份小于 2024，说明 NTP 还没同步完，稍微等一下
    if (timeinfo.tm_year < (2024 - 1900))
    {
        snprintf(out, out_size, "Time is still syncing with NTP server...");
        return ESP_OK;
    }

    // 格式化输出精准时间
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &timeinfo);
    return ESP_OK;
}

/* 读取本地时间 */
esp_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size)
{
    ESP_LOGI(TAG, "Fetching current time...");

    esp_err_t err = fetch_time_direct(output, output_size);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Time: %s", output);
    }
    else
    {
        snprintf(output, output_size, "Error: failed to fetch time (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", output);
    }

    return err;
}
