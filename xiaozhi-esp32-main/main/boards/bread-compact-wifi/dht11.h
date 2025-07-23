#ifndef DHT11_H
#define DHT11_H

#include <driver/gpio.h>

#define DHT11_START_SIGNAL_US 18000

class DHT11 {
public:
    explicit DHT11(gpio_num_t pin);
    ~DHT11();
    bool Read();
    float GetTemperature() const { return temperature_; }
    float GetHumidity() const { return humidity_; }
    bool CheckTemperatureThreshold(float threshold, bool above_threshold = true);
    bool CheckHumidityThreshold(float threshold, bool above_threshold = true);

private:
    gpio_num_t pin_;
    float temperature_;
    float humidity_;
    bool data_valid_;
    bool ReadBit();
    bool ReadByte(uint8_t& byte);
    bool ReadData(uint8_t data[5]);
};

#endif // DHT11_H 