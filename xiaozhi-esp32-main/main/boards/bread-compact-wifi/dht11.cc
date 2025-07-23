#include "dht11.h"
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include "esp_log.h"

#define TAG "DHT11"

DHT11::DHT11(gpio_num_t pin) : pin_(pin), temperature_(0.0f), humidity_(0.0f), data_valid_(false) {
    // 配置GPIO为输出模式
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    
    ESP_LOGI(TAG, "DHT11 initialized on GPIO %d", pin_);
}

DHT11::~DHT11() {
    // 清理GPIO配置
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
}

bool DHT11::ReadBit() {
    // 等待数据线变低
    int64_t start_time = esp_timer_get_time();
    while (gpio_get_level(pin_) == 1) {
        if (esp_timer_get_time() - start_time > 100) { // 100us超时
            return false;
        }
    }
    
    // 等待数据线变高
    start_time = esp_timer_get_time();
    while (gpio_get_level(pin_) == 0) {
        if (esp_timer_get_time() - start_time > 100) { // 100us超时
            return false;
        }
    }
    
    // 测量高电平持续时间
    start_time = esp_timer_get_time();
    while (gpio_get_level(pin_) == 1) {
        if (esp_timer_get_time() - start_time > 100) { // 100us超时
            return false;
        }
    }
    
    int64_t duration = esp_timer_get_time() - start_time;
    return duration > 50; // 如果高电平持续时间大于50us，则为1，否则为0
}

bool DHT11::ReadByte(uint8_t& byte) {
    byte = 0;
    for (int i = 7; i >= 0; i--) {
        bool bit = ReadBit();
        if (bit) {
            byte |= (1 << i);
        }
    }
    return true;
}

bool DHT11::ReadData(uint8_t data[5]) {
    // 发送启动信号
    gpio_set_direction(pin_, GPIO_MODE_OUTPUT);
    gpio_set_level(pin_, 0);
    esp_rom_delay_us(DHT11_START_SIGNAL_US);
    gpio_set_level(pin_, 1);
    esp_rom_delay_us(40);
    
    // 切换到输入模式
    gpio_set_direction(pin_, GPIO_MODE_INPUT);
    
    // 等待DHT11响应
    int64_t start_time = esp_timer_get_time();
    while (gpio_get_level(pin_) == 1) {
        if (esp_timer_get_time() - start_time > 100) { // 100us超时
            ESP_LOGE(TAG, "DHT11 response timeout");
            return false;
        }
    }
    
    // 等待响应结束
    start_time = esp_timer_get_time();
    while (gpio_get_level(pin_) == 0) {
        if (esp_timer_get_time() - start_time > 100) { // 100us超时
            ESP_LOGE(TAG, "DHT11 response end timeout");
            return false;
        }
    }
    
    // 等待数据开始
    start_time = esp_timer_get_time();
    while (gpio_get_level(pin_) == 1) {
        if (esp_timer_get_time() - start_time > 100) { // 100us超时
            ESP_LOGE(TAG, "DHT11 data start timeout");
            return false;
        }
    }
    
    // 读取40位数据
    for (int i = 0; i < 5; i++) {
        if (!ReadByte(data[i])) {
            ESP_LOGE(TAG, "Failed to read byte %d", i);
            return false;
        }
    }
    
    return true;
}

bool DHT11::Read() {
    uint8_t data[5];
    
    if (!ReadData(data)) {
        data_valid_ = false;
        return false;
    }
    
    // 验证校验和
    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        ESP_LOGE(TAG, "DHT11 checksum error: calculated=%d, received=%d", checksum, data[4]);
        data_valid_ = false;
        return false;
    }
    
    // 解析温湿度数据
    humidity_ = static_cast<float>(data[0]) + static_cast<float>(data[1]) / 10.0f;
    temperature_ = static_cast<float>(data[2]) + static_cast<float>(data[3]) / 10.0f;
    
    // DHT11温度范围检查
    if (temperature_ < 0.0f || temperature_ > 50.0f) {
        ESP_LOGE(TAG, "DHT11 temperature out of range: %.1f°C", temperature_);
        data_valid_ = false;
        return false;
    }
    
    // DHT11湿度范围检查
    if (humidity_ < 20.0f || humidity_ > 90.0f) {
        ESP_LOGE(TAG, "DHT11 humidity out of range: %.1f%%", humidity_);
        data_valid_ = false;
        return false;
    }
    
    data_valid_ = true;
    ESP_LOGI(TAG, "DHT11 read success: Temperature=%.1f°C, Humidity=%.1f%%", temperature_, humidity_);
    return true;
}

bool DHT11::CheckTemperatureThreshold(float threshold, bool above_threshold) {
    if (!data_valid_) {
        return false;
    }
    
    if (above_threshold) {
        return temperature_ >= threshold;
    } else {
        return temperature_ <= threshold;
    }
}

bool DHT11::CheckHumidityThreshold(float threshold, bool above_threshold) {
    if (!data_valid_) {
        return false;
    }
    
    if (above_threshold) {
        return humidity_ >= threshold;
    } else {
        return humidity_ <= threshold;
    }
} 