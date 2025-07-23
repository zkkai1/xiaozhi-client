#include "wifi_board.h"
#include "audio_codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "dht11.h"
#include "system_info.h"
#include "mpu6050.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_timer.h>
#include "esp_http_client.h"

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_14_1);

class CompactWifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    
    // DHT11传感器相关
    DHT11* dht11_sensor_;
    esp_timer_handle_t dht11_timer_;
    float temperature_threshold_ = 29.0f;  // 温度阈值，默认30°C
    float humidity_threshold_ = 88.0f;     // 湿度阈值，默认70%
    bool temperature_trigger_enabled_ = true;
    bool humidity_trigger_enabled_ = true;
    static bool temperature_triggered_;
    static bool humidity_triggered_;
    // 新增：保存最近一次温度
    float last_temperature_ = -1000.0f;

    // MPU6050相关
    i2c_master_bus_handle_t mpu6050_i2c_bus_;
    MPU6050* mpu6050_ = nullptr;
    esp_timer_handle_t mpu6050_timer_ = nullptr;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            {&font_puhui_14_1, &font_awesome_14_1});
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            // 检查GPIO20电平
            int gpio20_level = gpio_get_level(GPIO_NUM_20);
            ESP_LOGI("GPIO20", "按钮点击时GPIO20电平: %d", gpio20_level);
            
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });
        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // 初始化GPIO触发功能
    void InitializeGpioTrigger() {
        // 配置GPIO20为输入模式，用于高低电平触发
        gpio_config_t trigger_io_conf = {
            .pin_bit_mask = (1ULL << GPIO_NUM_20),  // GPIO20
            .mode = GPIO_MODE_INPUT,                // 输入模式
            .pull_up_en = GPIO_PULLUP_DISABLE,      // 禁用上拉
            .pull_down_en = GPIO_PULLDOWN_ENABLE,   // 启用下拉，确保平时为低电平
            .intr_type = GPIO_INTR_DISABLE          // 禁用中断，避免重启
        };
        
        printf("GPIO20: 开始配置GPIO20...\n");
        
        // 配置GPIO
        esp_err_t ret = gpio_config(&trigger_io_conf);
        if (ret != ESP_OK) {
            ESP_LOGE("GPIO20", "GPIO配置失败: %d", ret);
            return;
        }
        ESP_LOGI("GPIO20", "GPIO20配置成功");

        // 测试GPIO配置是否生效
        int test_level = gpio_get_level(GPIO_NUM_20);
        ESP_LOGI("GPIO20", "配置后GPIO20电平: %d", test_level);
        
        // 验证GPIO配置是否生效
        ESP_LOGI("GPIO20", "GPIO20配置完成，使用定时器监控电平变化...");
    }

    // 初始化DHT11传感器
    void InitializeDht11() {
        ESP_LOGI(TAG, "初始化DHT11传感器...");
        
        // 创建DHT11传感器实例，使用config.h中定义的GPIO引脚
        dht11_sensor_ = new DHT11(DHT11_GPIO_PIN);
        
        // 创建定时器，每5秒读取一次DHT11数据
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                CompactWifiBoard* board = static_cast<CompactWifiBoard*>(arg);
                board->ReadDht11Data();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "dht11_timer",
            .skip_unhandled_events = true,
        };
        
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &dht11_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(dht11_timer_, 1000000)); // 1秒
        
        ESP_LOGI(TAG, "DHT11传感器初始化完成，使用GPIO %d", DHT11_GPIO_PIN);
    }
    
    // 读取DHT11数据并检查阈值
    void ReadDht11Data() {
        if (dht11_sensor_->Read()) {
            float temperature = dht11_sensor_->GetTemperature();
            float humidity = dht11_sensor_->GetHumidity();
            last_temperature_ = temperature; // 新增：保存最近一次温度
            
            // 串口打印温湿度信息
            ESP_LOGI(TAG, "DHT11数据: 温度=%.1f°C, 湿度=%.1f%%\n", temperature, humidity);
            
            // 检查温度阈值触发
            if (temperature_trigger_enabled_ && !temperature_triggered_) {
                if (dht11_sensor_->CheckTemperatureThreshold(temperature_threshold_, true)) {
                    ESP_LOGI(TAG, "温度超过阈值%.1f°C，发送温度告警到服务器", temperature_threshold_);
                    temperature_triggered_ = true;
                    SendTemperatureAlertToServer(temperature);
                }
            } else if (temperature < temperature_threshold_ - 0.5f) { // 温度降低2度后重置触发
                temperature_triggered_ = false;
            }
            
            // 检查湿度阈值触发
            if (humidity_trigger_enabled_ && !humidity_triggered_) {
                if (dht11_sensor_->CheckHumidityThreshold(humidity_threshold_, true)) {
                    ESP_LOGI(TAG, "湿度超过阈值%.1f%%，触发对话", humidity_threshold_);
                    humidity_triggered_ = true;
                    std::string wake_word = "小爱同学";
                    //Application::GetInstance().WakeWordInvoke(wake_word);
                }
            } else if (humidity < humidity_threshold_ - 5.0f) { // 湿度降低5%后重置触发
                humidity_triggered_ = false;
            }
        } else {
            ESP_LOGW(TAG, "DHT11读取失败");
        }
    }

    // 初始化MPU6050 I2C
    void InitializeMpu6050I2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)1, // 使用I2C_NUM_1，避免与显示屏冲突
            .sda_io_num = MPU6050_SDA_PIN,
            .scl_io_num = MPU6050_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &mpu6050_i2c_bus_));
    }

    // 初始化MPU6050传感器
    void InitializeMpu6050() {
        ESP_LOGI(TAG, "初始化MPU6050...");
        mpu6050_ = new MPU6050(mpu6050_i2c_bus_);
        if (!mpu6050_->Initialize()) {
            ESP_LOGE(TAG, "MPU6050初始化失败");
            return;
        }
        // 创建定时器，定时读取六轴数据
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                CompactWifiBoard* board = static_cast<CompactWifiBoard*>(arg);
                board->ReadMpu6050Data();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "mpu6050_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &mpu6050_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(mpu6050_timer_, 1000000)); // 1秒
        ESP_LOGI(TAG, "MPU6050初始化完成");
    }

    // 新增：上报加速度事件
    void SendAcceAlertToServer(float acce_x) {
        char post_data[256];
        snprintf(post_data, sizeof(post_data),
            "{\"device_id\":\"%s\",\"event\":\"acce_high\",\"value\":%.2f,\"message\":\"喝水太快\"}",
            SystemInfo::GetMacAddress().c_str(), acce_x);

        esp_http_client_config_t config = {
            .url = "http://192.168.111.67:8003/xiaozhi/acce_alert", // 按你的接口
            .method = HTTP_METHOD_POST,
            .timeout_ms = 3000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "device-id", "98:88:e0:06:7f:20");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGI("HTTP", "HTTP POST Status = %d, content_length = %lld",
                     esp_http_client_get_status_code(client),
                     (long long)esp_http_client_get_content_length(client));
            int content_length = esp_http_client_get_content_length(client);
            if (content_length > 0) {
                char *buffer = (char *)malloc(content_length + 1);
                int total_read_len = 0;
                int read_len = 0;
                while (total_read_len < content_length) {
                    read_len = esp_http_client_read(client, buffer + total_read_len, content_length - total_read_len);
                    if (read_len <= 0) break;
                    total_read_len += read_len;
                }
                buffer[total_read_len] = 0;
                ESP_LOGI("HTTP", "Response: %s", buffer);
                free(buffer);
            }
        } else {
            ESP_LOGE("HTTP", "HTTP POST request failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }

    // 读取MPU6050数据并打印
    void ReadMpu6050Data() {
        if (!mpu6050_) return;
        int16_t accel[3], gyro[3], temp;
        if (mpu6050_->ReadAccelGyro(accel, gyro, temp)) {
            float acce_x = accel[0] / 16384.0f;
            float acce_y = accel[1] / 16384.0f;
            float acce_z = accel[2] / 16384.0f;
            float gyro_x = gyro[0] / 131.0f;
            float gyro_y = gyro[1] / 131.0f;
            float gyro_z = gyro[2] / 131.0f;
            float temp_c = temp / 340.0f + 36.53f;
            ESP_LOGI(TAG, "acce_x:%.2f, acce_y:%.2f, acce_z:%.2f", acce_x, acce_y, acce_z);//加速度
            ESP_LOGI(TAG, "gyro_x:%.2f, gyro_y:%.2f, gyro_z:%.2f", gyro_x, gyro_y, gyro_z);//角速度
            ESP_LOGI(TAG, "temp:%.2f°C", temp_c);
            // 新增：检测喝水太快并上报
            static bool acce_alert_triggered = false;
            if (acce_x > 0.9f && !acce_alert_triggered) {
                ESP_LOGI(TAG, "检测到喝水太快，acce_x=%.2f，上报服务器", acce_x);
                SendAcceAlertToServer(acce_x);
                acce_alert_triggered = true;
            } else if (acce_x < 0.5f) {
                acce_alert_triggered = false;
            }
        } else {
            ESP_LOGW(TAG, "读取MPU6050数据失败");
        }
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Lamp"));
    }

    void SendTemperatureAlertToServer(float temperature) {
        char post_data[256];
        snprintf(post_data, sizeof(post_data),
            "{\"device_id\":\"%s\",\"event\":\"temperature_high\",\"value\":%.1f,\"message\":\"水太烫了，别喝！\"}",
            SystemInfo::GetMacAddress().c_str(), temperature);

        esp_http_client_config_t config = {
            .url = "http://192.168.111.67:8003/xiaozhi/temperature_alert", // 替换为你的服务器地址
            //.url = "http://httpbin.org/post",
            .method = HTTP_METHOD_POST,
            .timeout_ms = 3000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);

        esp_http_client_set_header(client, "Content-Type", "application/json");
        //esp_http_client_set_header(client, "device-id", "my_cup_001");
        esp_http_client_set_header(client, "device-id", "98:88:e0:06:7f:20");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGI("HTTP", "HTTP POST Status = %d, content_length = %lld",
                     esp_http_client_get_status_code(client),
                     (long long)esp_http_client_get_content_length(client));

            //串口打印服务器返回内容
            int content_length = esp_http_client_get_content_length(client);
            if (content_length > 0) {
                char *buffer = (char *)malloc(content_length + 1);
                int total_read_len = 0;
                int read_len = 0;
                while (total_read_len < content_length) {
                    read_len = esp_http_client_read(client, buffer + total_read_len, content_length - total_read_len);
                    if (read_len <= 0) break;
                    total_read_len += read_len;
                }
                buffer[total_read_len] = 0; // 字符串结尾
                ESP_LOGI("HTTP", "Response: %s", buffer);
                free(buffer);
            } else {
                // 也可以尝试读取一段固定长度
                char buffer[256];
                int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
                if (read_len > 0) {
                    buffer[read_len] = 0;
                    ESP_LOGI("HTTP", "Response: %s", buffer);
                }
            }
        } else {
            ESP_LOGE("HTTP", "HTTP POST request failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }

public:
    CompactWifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        printf("CompactWifiBoard: 开始初始化...\n");
        InitializeDisplayI2c();
        printf("CompactWifiBoard: I2C初始化完成\n");
        InitializeSsd1306Display();
        printf("CompactWifiBoard: 显示屏初始化完成\n");
        InitializeButtons();
        printf("CompactWifiBoard: 按钮初始化完成\n");
        InitializeIot();
        printf("CompactWifiBoard: IoT初始化完成\n");
        InitializeGpioTrigger();
        printf("CompactWifiBoard: GPIO触发初始化完成\n");
        InitializeDht11();
        printf("CompactWifiBoard: DHT11传感器初始化完成\n");
        InitializeMpu6050I2c();
        InitializeMpu6050();
        printf("CompactWifiBoard: MPU6050初始化完成\n");
    }
    
    ~CompactWifiBoard() {
        if (dht11_timer_) {
            esp_timer_stop(dht11_timer_);
            esp_timer_delete(dht11_timer_);
        }
        if (dht11_sensor_) {
            delete dht11_sensor_;
        }
        if (mpu6050_timer_) {
            esp_timer_stop(mpu6050_timer_);
            esp_timer_delete(mpu6050_timer_);
        }
        if (mpu6050_) {
            delete mpu6050_;
        }
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
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

    // 新增：获取水杯温度接口
    float get_temperature() override {
        return last_temperature_;
    }
};

// 静态变量定义
bool CompactWifiBoard::temperature_triggered_ = false;
bool CompactWifiBoard::humidity_triggered_ = false;

DECLARE_BOARD(CompactWifiBoard);
