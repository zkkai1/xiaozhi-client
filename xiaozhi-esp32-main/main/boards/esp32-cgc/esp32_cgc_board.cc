#include "wifi_board.h"
#include "audio_codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "led/single_led.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include <esp_lcd_ili9341.h>
#endif

#if defined(LCD_TYPE_GC9A01_SERIAL)
#include <esp_lcd_gc9a01.h>
static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb1, (uint8_t[]){0x80}, 1, 0},
    {0xb2, (uint8_t[]){0x27}, 1, 0},
    {0xb3, (uint8_t[]){0x13}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x05}, 1, 0},
    {0xac, (uint8_t[]){0xc8}, 1, 0},
    {0xab, (uint8_t[]){0x0f}, 1, 0},
    {0x3a, (uint8_t[]){0x05}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x08}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xea, (uint8_t[]){0x02}, 1, 0},
    {0xe8, (uint8_t[]){0x2A}, 1, 0},
    {0xe9, (uint8_t[]){0x47}, 1, 0},
    {0xe7, (uint8_t[]){0x5f}, 1, 0},
    {0xc6, (uint8_t[]){0x21}, 1, 0},
    {0xc7, (uint8_t[]){0x15}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1D, 0x38, 0x09, 0x4D, 0x92, 0x2F, 0x35, 0x52, 0x1E, 0x0C,
                0x04, 0x12, 0x14, 0x1f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x16, 0x40, 0x1C, 0x54, 0xA9, 0x2D, 0x2E, 0x56, 0x10, 0x0D,
                0x0C, 0x1A, 0x14, 0x1E},
    14, 0},
    {0xf4, (uint8_t[]){0x00, 0x00, 0xFF}, 3, 0},
    {0xba, (uint8_t[]){0xFF, 0xFF}, 2, 0},
};
#endif
 
#define TAG "ESP32_CGC"

LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_14_1);

class ESP32_CGC : public WifiBoard {
private:
    Button boot_button_;
    LcdDisplay* display_;
    Button asr_button_;

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RESET_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };        
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif
        
        esp_lcd_panel_reset(panel);
 

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef  LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    {
                                        .text_font = &font_puhui_14_1,
                                        .icon_font = &font_awesome_14_1,
                                        .emoji_font = font_emoji_32_init(),
                                    });
    }


 
    void InitializeButtons() {
        // 配置GPIO39为输入模式，用于高低电平触发
        gpio_config_t trigger_io_conf = {
            .pin_bit_mask = (1ULL << GPIO_NUM_45),  // GPIO45
            .mode = GPIO_MODE_INPUT,                // 输入模式
            .pull_up_en = GPIO_PULLUP_DISABLE,      // 禁用上拉
            .pull_down_en = GPIO_PULLDOWN_ENABLE,   // 启用下拉，确保平时为低电平
            .intr_type = GPIO_INTR_POSEDGE          // 上升沿触发
        };
        
        printf("GPIO45: 开始配置GPIO45...\n");
        
        // 先禁用中断
        gpio_intr_disable(GPIO_NUM_45);
        ESP_LOGI("GPIO45", "已禁用GPIO45中断");
        
        // 配置GPIO
        esp_err_t ret = gpio_config(&trigger_io_conf);
        if (ret != ESP_OK) {
            ESP_LOGE("GPIO45", "GPIO配置失败: %d", ret);
            return;
        }
        ESP_LOGI("GPIO45", "GPIO45配置成功");

        // 安装GPIO中断服务
        ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {  // ESP_ERR_INVALID_STATE表示服务已经安装
            ESP_LOGE("GPIO45", "中断服务安装失败: %d", ret);
            return;
        }
        ESP_LOGI("GPIO45", "GPIO中断服务安装成功");

        // 添加GPIO中断处理函数
        ret = gpio_isr_handler_add(GPIO_NUM_45, [](void* arg) {
            // 直接在主任务中处理中断
            static uint32_t last_trigger_time = 0;
            uint32_t current_time = esp_timer_get_time() / 1000; // 转换为毫秒
            
            ESP_LOGI("GPIO45", "检测到中断，当前电平: %d", gpio_get_level(GPIO_NUM_45));
            
            // 简单的去抖动，确保两次触发间隔至少200ms
            if (current_time - last_trigger_time > 200) {
                last_trigger_time = current_time;
                ESP_LOGI("GPIO45", "通过去抖动检查，准备触发对话");
                
                // 在主任务中触发对话
                Application::GetInstance().Schedule([]() {
                    int level = gpio_get_level(GPIO_NUM_45);
                    ESP_LOGI("GPIO45", "Schedule中检查电平: %d", level);
                    if (level == 1) {
                        ESP_LOGI("GPIO45", "检测到高电平，触发对话");
                        std::string wake_word = "小爱同学";
                        Application::GetInstance().WakeWordInvoke(wake_word);
                    } else {
                        ESP_LOGI("GPIO45", "电平已变为低电平，不触发对话");
                    }
                });
            } else {
                ESP_LOGI("GPIO45", "去抖动检查未通过，忽略此次触发");
            }
        }, NULL);
        
        if (ret != ESP_OK) {
            ESP_LOGE("GPIO45", "中断处理函数注册失败: %d", ret);
            return;
        }
        ESP_LOGI("GPIO45", "中断处理函数注册成功");

        // 最后启用中断
        gpio_intr_enable(GPIO_NUM_45);
        ESP_LOGI("GPIO45", "GPIO45中断已启用，初始化完成");
        
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

        asr_button_.OnClick([this]() {
            std::string wake_word="小爱同学";
            Application::GetInstance().WakeWordInvoke(wake_word);
        });

    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
    }

public:
    ESP32_CGC() :
	boot_button_(BOOT_BUTTON_GPIO), asr_button_(ASR_BUTTON_GPIO) {
        printf("ESP32_CGC: 开始初始化...\n");
        InitializeSpi();
        printf("ESP32_CGC: SPI初始化完成\n");
        InitializeLcdDisplay();
        printf("ESP32_CGC: LCD初始化完成\n");
        InitializeButtons();
        printf("ESP32_CGC: 按钮初始化完成\n");
        InitializeIot();
        printf("ESP32_CGC: IoT初始化完成\n");
        GetBacklight()->RestoreBrightness();
        printf("ESP32_CGC: 背光初始化完成\n");
    }

    virtual AudioCodec* GetAudioCodec() override 
    {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(ESP32_CGC);
