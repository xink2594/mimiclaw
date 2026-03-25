#pragma once

#include "esp_err.h"

/**
 * @brief 初始化 ST7789 屏幕并挂载 LVGL
 */
esp_err_t display_face_init(void);

/**
 * @brief 外部接口：让大模型改变屏幕情绪
 * @param mood 情绪代号 (0: 发呆, 1: 思考, 2: 开心, 3: 睡觉)
 */
void display_set_mood(int mood);