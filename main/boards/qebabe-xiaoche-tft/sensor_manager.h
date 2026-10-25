// 小车传感器管理：TOF050C（VL53L0X 激光测距，I2C 地址 0x29）
// 飞线接法：SDA=GPIO36, SCL=GPIO11
#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

class SensorManager {
public:
    static SensorManager& GetInstance() {
        static SensorManager instance;
        return instance;
    }

    // 初始化 I2C 总线与 TOF050C，注册 MCP 工具。幂等，可重复调用。
    void Initialize();

    // 读取距离（毫米）。超时/无效返回 -1。
    int ReadDistanceMm();

    bool distance_ready() const { return distance_ready_; }

private:
    SensorManager() = default;

    bool i2c_ready_ = false;
    bool distance_ready_ = false;
};

#endif // SENSOR_MANAGER_H
