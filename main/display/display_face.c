#include "display_face.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "display_face";

// ========== 硬件引脚定义 ==========
#define PIN_NUM_SCLK 38
#define PIN_NUM_MOSI 39
#define PIN_NUM_RST 40
#define PIN_NUM_DC 41
#define PIN_NUM_CS 42
#define PIN_NUM_BLK 2

// 假设你的 ST7789 是最常见的 240x240 方屏，如果是矩形请改为 240x320
#define LCD_H_RES 240
#define LCD_V_RES 240

// ========== UI 全局变量 ==========
static lv_obj_t *left_eye;
static lv_obj_t *right_eye;
static lv_timer_t *anim_timer;
static int current_mood = 0;
static int anim_frame = 0;

// 情绪状态机数组 (发呆、思考、开心、睡觉)
static const char *mood_frames[4][2] = {
    {"(O_O)  ", "(-_-)  "}, // 0: Idle (大眼/闭眼)
    {"(>_<)  ", "(>_<). "}, // 1: Thinking (憋气)
    {"(^O^)  ", "(^o^)  "}, // 2: Happy (大笑/小笑)
    {"(-_-)z ", "(-_-)zz"}  // 3: Sleep (打呼噜)
};

/* 动画定时器回调：通过物理形变改变眼睛大小 */
static void face_anim_cb(lv_timer_t *timer)
{
    anim_frame = (anim_frame + 1) % 2;
    lvgl_port_lock(0);

    if (current_mood == 0)
    {
        // 状态 0: 发呆 (Idle) - 偶尔眨眼
        // 设定 800ms 触发一次，睁眼时间长，闭眼时间短（用 frame 模拟）
        if (anim_frame == 0)
        {
            lv_obj_set_size(left_eye, 40, 80);
            lv_obj_set_size(right_eye, 40, 80);
        }
        else
        {
            // 眨眼：高度被压扁到 10
            lv_obj_set_size(left_eye, 40, 10);
            lv_obj_set_size(right_eye, 40, 10);
        }
    }
    else if (current_mood == 1)
    {
        // 状态 1: 思考 (Thinking) - 一大一小，左右横跳
        if (anim_frame == 0)
        {
            lv_obj_set_size(left_eye, 40, 60);
            lv_obj_set_size(right_eye, 40, 20); // 右眼眯起
        }
        else
        {
            lv_obj_set_size(left_eye, 40, 20); // 左眼眯起
            lv_obj_set_size(right_eye, 40, 60);
        }
    }
    else if (current_mood == 2)
    {
        // 状态 2: 开心 (Happy) - 变成两道弯弯的缝（用压扁模拟）
        lv_obj_set_size(left_eye, 40, 15);
        lv_obj_set_size(right_eye, 40, 15);
    }
    else if (current_mood == 3)
    {
        // 状态 3: 睡觉 (Sleep) - 完全闭上，变成两条细线
        lv_obj_set_size(left_eye, 40, 5);
        lv_obj_set_size(right_eye, 40, 5);
    }

    lvgl_port_unlock();
}

esp_err_t display_face_init(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = 40 * 1000 * 1000, // 40MHz 刷屏速度
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install ST7789 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    // 注意：ESP-IDF v5 自带了 ST7789 驱动，直接用内置的即可
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    // 很多 ST7789 屏幕需要颜色反转才正常显示，如果你的颜色发白，把这里改成 false
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // 点亮背光
    gpio_set_direction(PIN_NUM_BLK, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_BLK, 1);

    ESP_LOGI(TAG, "Initialize LVGL");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * LCD_V_RES / 10, // 1/10 屏幕大小的显存
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true // 翻转 16 位的色彩高低字节
        }};
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);

    // ========== 绘制 UI 界面 (纯图形化) ==========
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0); // 黑底屏幕

    // --- 创建左眼 ---
    left_eye = lv_obj_create(scr);
    lv_obj_set_scrollbar_mode(left_eye, LV_SCROLLBAR_MODE_OFF);     // 关闭滚动条
    lv_obj_set_size(left_eye, 40, 80);                              // 宽40，高80
    lv_obj_set_style_radius(left_eye, 20, 0);                       // 圆角半径20，让它变成药丸形状
    lv_obj_set_style_bg_color(left_eye, lv_color_hex(0x00FF00), 0); // 极客绿
    lv_obj_set_style_border_width(left_eye, 0, 0);                  // 去除默认边框
    // 将左眼放在屏幕中心偏左 50 像素的位置
    lv_obj_align(left_eye, LV_ALIGN_CENTER, -50, 0);

    // --- 创建右眼 ---
    right_eye = lv_obj_create(scr);
    lv_obj_set_scrollbar_mode(right_eye, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(right_eye, 40, 80);
    lv_obj_set_style_radius(right_eye, 20, 0);
    lv_obj_set_style_bg_color(right_eye, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(right_eye, 0, 0);
    // 将右眼放在屏幕中心偏右 50 像素的位置
    lv_obj_align(right_eye, LV_ALIGN_CENTER, 50, 0);

    // 创建动画心跳 (每 600ms 动一次)
    anim_timer = lv_timer_create(face_anim_cb, 600, NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Display initialized successfully with Graphical Eyes");
    return ESP_OK;
}

void display_set_mood(int mood)
{
    if (mood >= 0 && mood <= 3)
    {
        current_mood = mood;
    }
}