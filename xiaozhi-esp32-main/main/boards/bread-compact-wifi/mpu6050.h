#ifndef MPU6050_H
#define MPU6050_H

#include <driver/i2c_master.h>
#include <stdint.h>

class MPU6050 {
public:
    MPU6050(i2c_master_bus_handle_t i2c_bus, uint8_t addr = 0x68);
    bool Initialize();
    bool ReadAccelGyro(int16_t accel[3], int16_t gyro[3], int16_t& temp);

private:
    i2c_master_dev_handle_t i2c_dev_;
    i2c_master_bus_handle_t i2c_bus_;
    uint8_t addr_;
    bool WriteReg(uint8_t reg, uint8_t value);
    bool ReadRegs(uint8_t reg, uint8_t* buf, size_t len);
};

#endif // MPU6050_H 