#include "mpu6050.h"
#include <esp_log.h>

#define TAG "MPU6050"

// MPU6050寄存器定义
#define MPU6050_PWR_MGMT_1   0x6B
#define MPU6050_ACCEL_XOUT_H 0x3B
#define MPU6050_GYRO_XOUT_H  0x43

MPU6050::MPU6050(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    : i2c_dev_(nullptr), i2c_bus_(i2c_bus), addr_(addr) {}

bool MPU6050::Initialize() {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr_,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };
    if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &i2c_dev_) != ESP_OK) {
        ESP_LOGE(TAG, "添加MPU6050到I2C总线失败");
        return false;
    }
    // 唤醒MPU6050
    return WriteReg(MPU6050_PWR_MGMT_1, 0x00);
}

bool MPU6050::WriteReg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(i2c_dev_, buf, 2, 100) == ESP_OK;
}

bool MPU6050::ReadRegs(uint8_t reg, uint8_t* buf, size_t len) {
    return i2c_master_transmit_receive(i2c_dev_, &reg, 1, buf, len, 100) == ESP_OK;
}

bool MPU6050::ReadAccelGyro(int16_t accel[3], int16_t gyro[3], int16_t& temp) {
    uint8_t data[14];
    if (!ReadRegs(MPU6050_ACCEL_XOUT_H, data, 14)) {
        ESP_LOGE(TAG, "读取MPU6050数据失败");
        return false;
    }
    accel[0] = (data[0] << 8) | data[1];
    accel[1] = (data[2] << 8) | data[3];
    accel[2] = (data[4] << 8) | data[5];
    temp    = (data[6] << 8) | data[7];
    gyro[0] = (data[8] << 8) | data[9];
    gyro[1] = (data[10] << 8) | data[11];
    gyro[2] = (data[12] << 8) | data[13];
    return true;
} 