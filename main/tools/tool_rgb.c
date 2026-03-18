#include "tools/tool_registry.h"
#include "led_strip.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tool_rgb";

// ESP32-S3 绝大部分开发板的内置 RGB 都在引脚 48
#define RGB_LED_PIN 48

static led_strip_handle_t led_strip;
static bool is_initialized = false;

/* 初始化 WS2812 灯条 */
esp_err_t tool_rgb_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_PIN,
        .max_leds = 1, // 板子上只有 1 颗灯
    };

    // 使用 RMT 外设来生成高精度的高低电平脉冲
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (err == ESP_OK)
    {
        led_strip_clear(led_strip); // 初始状态灭灯
        is_initialized = true;
        ESP_LOGI(TAG, "WS2812 RGB LED initialized on GPIO %d", RGB_LED_PIN);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to initialize WS2812 RGB LED");
    }
    return err;
}

/* 核心执行逻辑：接收 {"r": 255, "g": 0, "b": 128} 格式的 JSON */
esp_err_t tool_rgb_execute(const char *input_json, char *output, size_t output_size)
{
    if (!is_initialized)
    {
        snprintf(output, output_size, "Error: RGB LED not initialized");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(input_json);
    if (!root)
    {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *r_obj = cJSON_GetObjectItem(root, "r");
    cJSON *g_obj = cJSON_GetObjectItem(root, "g");
    cJSON *b_obj = cJSON_GetObjectItem(root, "b");

    // 严谨校验输入参数
    if (!cJSON_IsNumber(r_obj) || !cJSON_IsNumber(g_obj) || !cJSON_IsNumber(b_obj))
    {
        snprintf(output, output_size, "Error: 'r', 'g', 'b' required (0-255)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int r = (int)r_obj->valuedouble;
    int g = (int)g_obj->valuedouble;
    int b = (int)b_obj->valuedouble;

    // 防止大模型乱给数值
    r = (r < 0) ? 0 : (r > 255) ? 255
                                : r;
    g = (g < 0) ? 0 : (g > 255) ? 255
                                : g;
    b = (b < 0) ? 0 : (b > 255) ? 255
                                : b;

    // 驱动底层发光
    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);

    // 组装返回给大模型的话术
    snprintf(output, output_size, "{\"status\":\"success\", \"current_color\": {\"r\":%d, \"g\":%d, \"b\":%d}}", r, g, b);
    ESP_LOGI(TAG, "RGB LED set to (%d, %d, %d)", r, g, b);

    cJSON_Delete(root);
    return ESP_OK;
}