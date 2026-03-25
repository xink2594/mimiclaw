#include "tools/tool_weather.h"
#include "mimi_config.h" // 确保这里能读到 MIMI_SECRET_SENIVERSE_KEY
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "tool_weather";

#define WEATHER_NOW_URL "https://api.seniverse.com/v3/weather/now.json?key=%s&location=%s&language=zh-Hans&unit=c"
#define WEATHER_DAILY_URL "https://api.seniverse.com/v3/weather/daily.json?key=%s&location=%s&language=zh-Hans&unit=c&start=0&days=3"

/* 用于累加 HTTP 响应的结构体 */
typedef struct
{
    char *buf;
    size_t len;
    size_t cap;
} weather_http_resp_t;

/* 简单的 URL 编码函数，用于处理中文城市名 */
static void url_encode(const char *src, char *dest, size_t dest_size)
{
    const char *hex = "0123456789ABCDEF";
    size_t pos = 0;
    for (size_t i = 0; src[i] != '\0' && pos < dest_size - 3; i++)
    {
        unsigned char c = src[i];
        /* 保留无需编码的字符 */
        if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
            ('0' <= c && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            dest[pos++] = c;
        }
        else
        {
            /* 对中文等特殊字符进行 %XX 编码 */
            dest[pos++] = '%';
            dest[pos++] = hex[c >> 4];
            dest[pos++] = hex[c & 15];
        }
    }
    dest[pos] = '\0';
}

/* HTTP 事件回调 */
static esp_err_t weather_http_event_handler(esp_http_client_event_t *evt)
{
    weather_http_resp_t *resp = (weather_http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA)
    {
        if (resp->len + evt->data_len >= resp->cap)
        {
            size_t new_cap = resp->cap * 2;
            if (new_cap < resp->len + evt->data_len + 1)
            {
                new_cap = resp->len + evt->data_len + 1;
            }
            char *tmp = realloc(resp->buf, new_cap);
            if (!tmp)
                return ESP_ERR_NO_MEM;
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

esp_err_t tool_weather_now_execute(const char *input_json, char *output, size_t output_size)
{
    ESP_LOGI(TAG, "Executing weather_now tool...");

    /* 1. 解析大模型传入的参数 */
    cJSON *root = cJSON_Parse(input_json);
    if (!root)
    {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *loc_obj = cJSON_GetObjectItem(root, "location");
    if (!loc_obj || !cJSON_IsString(loc_obj) || strlen(loc_obj->valuestring) == 0)
    {
        snprintf(output, output_size, "Error: 'location' string is required");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    const char *location = loc_obj->valuestring;

    char encoded_loc[128] = {0};
    url_encode(location, encoded_loc, sizeof(encoded_loc));

    /* 2. 构造 API URL (免费版只支持 text, code, temperature 3项核心数据) */
    char url[512];
    snprintf(url, sizeof(url), WEATHER_NOW_URL, MIMI_SECRET_SENIVERSE_KEY, encoded_loc);

    /* 3. 发起 HTTP GET 请求 */
    weather_http_resp_t resp = {.buf = calloc(1, 2048), .len = 0, .cap = 2048};
    if (!resp.buf)
    {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = weather_http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        free(resp.buf);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status_code != 200)
    {
        ESP_LOGE(TAG, "Weather API failed: err=%s, http_code=%d", esp_err_to_name(err), status_code);
        snprintf(output, output_size, "Error: HTTP request failed or location not found (Code: %d)", status_code);
        free(resp.buf);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* 4. 解析返回的 JSON 并精简提炼给大模型 */
    cJSON *resp_json = cJSON_Parse(resp.buf);
    free(resp.buf);

    if (!resp_json)
    {
        snprintf(output, output_size, "Error: Failed to parse weather API response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *results = cJSON_GetObjectItem(resp_json, "results");
    cJSON *first_result = cJSON_GetArrayItem(results, 0);
    if (first_result)
    {
        cJSON *loc_node = cJSON_GetObjectItem(first_result, "location");
        cJSON *now_node = cJSON_GetObjectItem(first_result, "now");
        cJSON *update_node = cJSON_GetObjectItem(first_result, "last_update");

        if (loc_node && now_node)
        {
            const char *city_name = cJSON_GetObjectItem(loc_node, "name")->valuestring;
            const char *weather_text = cJSON_GetObjectItem(now_node, "text")->valuestring;
            const char *temp = cJSON_GetObjectItem(now_node, "temperature")->valuestring;
            const char *last_update = update_node ? update_node->valuestring : "unknown";

            /* 输出给 LLM 的纯文本极简格式 */
            snprintf(output, output_size,
                     "Weather in %s:\nCondition: %s\nTemperature: %s Celsius\nLast Updated: %s",
                     city_name, weather_text, temp, last_update);

            ESP_LOGI(TAG, "Weather fetch success for %s", city_name);
        }
        else
        {
            snprintf(output, output_size, "Error: Missing core data in response");
        }
    }
    else
    {
        snprintf(output, output_size, "Error: Location not recognized by API");
    }

    cJSON_Delete(resp_json);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_weather_daily_execute(const char *input_json, char *output, size_t output_size)
{
    ESP_LOGI(TAG, "Executing weather_forecast tool...");

    /* 1. 解析大模型传入的参数，实现“IP定位”默认后备逻辑 */
    cJSON *root = cJSON_Parse(input_json);
    const char *location = "ip"; // 默认值：通过设备公网 IP 自动定位

    if (root)
    {
        cJSON *loc_obj = cJSON_GetObjectItem(root, "location");
        // 如果 LLM 传了 location，并且不是空字符串，就用 LLM 传的
        if (loc_obj && cJSON_IsString(loc_obj) && strlen(loc_obj->valuestring) > 0)
        {
            location = loc_obj->valuestring;
        }
    }

    char encoded_loc[128] = {0};
    url_encode(location, encoded_loc, sizeof(encoded_loc));

    /* 2. 构造 API URL */
    char url[512];
    snprintf(url, sizeof(url), WEATHER_DAILY_URL, MIMI_SECRET_SENIVERSE_KEY, encoded_loc);

    /* 3. 发起 HTTP GET 请求 (复用之前写好的 weather_http_event_handler) */
    weather_http_resp_t resp = {.buf = calloc(1, 2048), .len = 0, .cap = 2048};
    if (!resp.buf)
    {
        if (root)
            cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = weather_http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        free(resp.buf);
        if (root)
            cJSON_Delete(root);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status_code != 200)
    {
        ESP_LOGE(TAG, "Weather Forecast API failed: err=%s, http_code=%d", esp_err_to_name(err), status_code);
        snprintf(output, output_size, "Error: HTTP request failed or location not found (Code: %d)", status_code);
        free(resp.buf);
        if (root)
            cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* 4. 解析并组装包含 3 天预报的纯文本 */
    cJSON *resp_json = cJSON_Parse(resp.buf);
    free(resp.buf);

    if (!resp_json)
    {
        snprintf(output, output_size, "Error: Failed to parse weather API response");
        if (root)
            cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *results = cJSON_GetObjectItem(resp_json, "results");
    cJSON *first_result = cJSON_GetArrayItem(results, 0);
    if (first_result)
    {
        cJSON *loc_node = cJSON_GetObjectItem(first_result, "location");
        cJSON *daily_array = cJSON_GetObjectItem(first_result, "daily");

        if (loc_node && daily_array && cJSON_IsArray(daily_array))
        {
            const char *city_name = cJSON_GetObjectItem(loc_node, "name")->valuestring;

            // 使用游标来安全地拼接多行文本
            char *cursor = output;
            size_t remaining = output_size;
            int written = snprintf(cursor, remaining, "3-Day Forecast for %s:\n", city_name);

            if (written > 0 && written < remaining)
            {
                cursor += written;
                remaining -= written;
            }

            int days_count = cJSON_GetArraySize(daily_array);
            for (int i = 0; i < days_count; i++)
            {
                cJSON *day_item = cJSON_GetArrayItem(daily_array, i);
                if (!day_item)
                    continue;

                const char *date = cJSON_GetObjectItem(day_item, "date")->valuestring;
                const char *text_day = cJSON_GetObjectItem(day_item, "text_day")->valuestring;
                const char *text_night = cJSON_GetObjectItem(day_item, "text_night")->valuestring;
                const char *high = cJSON_GetObjectItem(day_item, "high")->valuestring;
                const char *low = cJSON_GetObjectItem(day_item, "low")->valuestring;

                // 拼装例如: "- 2026-03-24: Day 晴, Night 多云, Temp: 7~20 C"
                written = snprintf(cursor, remaining, "- %s: Day %s, Night %s, Temp: %s~%s C\n",
                                   date, text_day, text_night, low, high);
                if (written > 0 && written < remaining)
                {
                    cursor += written;
                    remaining -= written;
                }
                else
                {
                    break; // 防止缓冲区溢出
                }
            }
            ESP_LOGI(TAG, "Weather forecast fetch success for %s", city_name);
        }
        else
        {
            snprintf(output, output_size, "Error: Missing daily forecast data in response");
        }
    }
    else
    {
        snprintf(output, output_size, "Error: Location not recognized by API");
    }

    cJSON_Delete(resp_json);
    if (root)
        cJSON_Delete(root);
    return ESP_OK;
}