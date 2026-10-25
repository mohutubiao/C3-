// VL6180X (TOF050C 内核) ESP-IDF 驱动
// 注意：VL6180X 与 VL53L0X 协议完全不兼容 —— 16 位寄存器地址、不同的测距流程
// 寄存器定义依据 ST VL6180X datasheet (DocID026471) 与 AN4545
#ifndef VL6180X_ESP_H
#define VL6180X_ESP_H

#include <driver/i2c_master.h>
#include <stdint.h>

class Vl6180x {
public:
    explicit Vl6180x(i2c_master_dev_handle_t dev)
        : dev_(dev), scaling_(1), last_range_status_(0) {}

    // 校验型号 ID (0xB4) 并写入 ST 官方推荐配置
    bool init();

    // 缩放因子 1/2/3：1=高精度(约10cm内) 2=中程 3=最远(约30cm)
    void setScaling(uint8_t scaling);
    uint8_t scaling() const { return scaling_; }

    // 单次测距，返回毫米（已乘缩放因子）
    // 返回 -1 表示通信失败或超时
    // 无障碍物时返回值 >= 0 但 rangeStatus() != 0
    int readRangeSingleMillimeters();

    // 上次测距的状态码：0=有效测距, 0x0B/0x0C 等表示无信号/超量程
    uint8_t rangeStatus() const { return last_range_status_; }

private:
    // VL6180X 全部为 16 位寄存器地址
    bool readReg8(uint16_t reg, uint8_t& value);
    bool writeReg8(uint16_t reg, uint8_t value);

    i2c_master_dev_handle_t dev_;
    uint8_t scaling_;
    uint8_t last_range_status_;
};

#endif  // VL6180X_ESP_H
