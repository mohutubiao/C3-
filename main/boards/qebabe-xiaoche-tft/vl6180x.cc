// VL6180X (TOF050C 内核) ESP-IDF 驱动实现
#include "vl6180x.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#define TAG "Vl6180x"

static constexpr int kI2cTimeoutMs = 100;

// 16 位寄存器地址
namespace {
constexpr uint16_t REG_IDENTIFICATION__MODEL_ID        = 0x000;
constexpr uint16_t REG_SYSTEM__FRESH_OUT_OF_RESET      = 0x016;
constexpr uint16_t REG_SYSTEM__INTERRUPT_CLEAR         = 0x015;
constexpr uint16_t REG_SYSRANGE__START                 = 0x018;
constexpr uint16_t REG_RANGE_SCALER                    = 0x096;
constexpr uint16_t REG_RESULT__INTERRUPT_STATUS_GPIO   = 0x04F;
constexpr uint16_t REG_RESULT__RANGE_STATUS            = 0x04D;
constexpr uint16_t REG_RESULT__RANGE_VAL               = 0x062;

// 各缩放档位对应 RANGE_SCALER 寄存器值（下标 = 缩放因子）
constexpr uint16_t kScalerValues[4] = {0, 253, 127, 84};
}  // namespace

bool Vl6180x::readReg8(uint16_t reg, uint8_t& value) {
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_transmit_receive(dev_, addr, 2, &value, 1, kI2cTimeoutMs) == ESP_OK;
}

bool Vl6180x::writeReg8(uint16_t reg, uint8_t value) {
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value};
    return i2c_master_transmit(dev_, buf, 3, kI2cTimeoutMs) == ESP_OK;
}

void Vl6180x::setScaling(uint8_t scaling) {
    if (scaling < 1) scaling = 1;
    if (scaling > 3) scaling = 3;
    // RANGE_SCALER 为 16 位寄存器
    uint8_t buf[4] = {(uint8_t)(REG_RANGE_SCALER >> 8), (uint8_t)(REG_RANGE_SCALER & 0xFF),
                      (uint8_t)(kScalerValues[scaling] >> 8), (uint8_t)(kScalerValues[scaling] & 0xFF)};
    i2c_master_transmit(dev_, buf, 4, kI2cTimeoutMs);
    scaling_ = scaling;
}

bool Vl6180x::init() {
    // 1. 型号 ID 读取 + 软复位重试（兼容芯片可能处于异常状态）
    uint8_t model_id = 0;
    for (int i = 0; i < 6; ++i) {
        if (readReg8(REG_IDENTIFICATION__MODEL_ID, model_id) && model_id == 0xB4) {
            break;
        }
        // 软复位尝试：写 0x0000 = 0x0000（对正版只读寄存器无害）
        uint8_t rst_pkt[4] = {0x00, 0x00, 0x00, 0x00};
        i2c_master_transmit(dev_, rst_pkt, 4, kI2cTimeoutMs);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (model_id != 0xB4) {
        ESP_LOGE(TAG, "model ID=0x%02X (VL6180X 应为 0xB4)", model_id);
        return false;
    }
    ESP_LOGI(TAG, "检测到 VL6180X (model ID=0x%02X)", model_id);

    uint8_t fresh = 0;
    readReg8(REG_SYSTEM__FRESH_OUT_OF_RESET, fresh);

    // 厂商官方 demo（Pololu VL6180X 库）流程：仅当 fresh==1（芯片首次上电）时写强制调谐表。
    // 注意本模块 fresh 一直读 0，此时官方 demo 直接跳过强制表——保持同样行为。
    if (fresh == 1) {
        const struct { uint16_t reg; uint8_t val; } kMandatory[] = {
            {0x0207, 0x01}, {0x0208, 0x01}, {0x0096, 0x00},
            {0x0097, 0xFD},  // RANGE_SCALER = 253 (1x)
            {0x00E3, 0x01}, {0x00E4, 0x03}, {0x00E5, 0x02},
            {0x00E6, 0x01}, {0x00E7, 0x03}, {0x00F5, 0x02}, {0x00D9, 0x05},
            {0x00DB, 0xCE}, {0x00DC, 0x03}, {0x00DD, 0xF8}, {0x009F, 0x00},
            {0x00A3, 0x3C}, {0x00B7, 0x00}, {0x00BB, 0x3C}, {0x00B2, 0x09},
            {0x00CA, 0x09}, {0x0198, 0x01}, {0x01B0, 0x17}, {0x01AD, 0x00},
            {0x00FF, 0x05}, {0x0100, 0x05}, {0x0199, 0x05},
            {0x01A6, 0x1B}, {0x01AC, 0x3E}, {0x01A7, 0x1F}, {0x0030, 0x00},
        };
        for (const auto& s : kMandatory) {
            if (!writeReg8(s.reg, s.val)) {
                ESP_LOGE(TAG, "写强制配置失败 @0x%03X", s.reg);
                return false;
            }
        }
        writeReg8(REG_SYSTEM__FRESH_OUT_OF_RESET, 0x00);
        ESP_LOGI(TAG, "首次上电: 已写入强制调谐表");
    } else {
        ESP_LOGI(TAG, "fresh=%d (非首次上电), 按厂商 demo 跳过强制调谐表", fresh);
    }

    // configureDefault()（AN4545 推荐公开寄存器，与厂商 demo 逐项一致）
    const struct { uint16_t reg; uint8_t val; } kRecommended[] = {
        {0x010A, 0x30},  // READOUT__AVERAGING_SAMPLE_PERIOD = 48
        {0x003F, 0x46},  // SYSALS__ANALOGUE_GAIN = 6
        {0x0031, 0xFF},  // SYSRANGE__VHV_REPEAT_RATE = 255
        {0x0040, 0x00},  // SYSALS__INTEGRATION_PERIOD 高字节 (0x0063 = 99)
        {0x0041, 0x63},  // SYSALS__INTEGRATION_PERIOD 低字节
        {0x002E, 0x01},  // SYSRANGE__VHV_RECALIBRATE: 手动触发一次校准
        {0x001B, 0x09},  // SYSRANGE__INTERMEASUREMENT_PERIOD (100ms)
        {0x003E, 0x31},  // SYSALS__INTERMEASUREMENT_PERIOD (500ms)
        {0x0014, 0x24},  // SYSTEM__INTERRUPT_CONFIG_GPIO: 新样本就绪中断
        {0x001C, 0x31},  // SYSRANGE__MAX_CONVERGENCE_TIME (49ms)
        {0x002A, 0x00},  // INTERLEAVED_MODE__ENABLE = 0
    };
    for (const auto& s : kRecommended) {
        if (!writeReg8(s.reg, s.val)) {
            ESP_LOGE(TAG, "写推荐配置失败 @0x%03X", s.reg);
            return false;
        }
    }

    // 缩放 1：DEMO 默认配置（最大量程 ~200mm），读数等于原始值
    setScaling(1);

    // 终极诊断：单次测距 2 秒，每 200ms 转储 int_stat + range_status + range_val + range_raw
    //   - int_stat.bit2 不变 → 测量引擎从未启动（VCSEL 激光器 / 芯片测量部分损坏）
    //   - int_stat.bit2 变了但 range_val=0xFF/0 或 range_status 非 0 → 引擎跑过但报错误
    writeReg8(REG_SYSTEM__INTERRUPT_CLEAR, 0x07);
    writeReg8(REG_SYSRANGE__START, 0x01);
    uint8_t final_ist = 0, final_rs = 0, final_rv = 0, final_rr = 0;
    for (int i = 0; i < 10; ++i) {  // 2 秒
        vTaskDelay(pdMS_TO_TICKS(200));
        uint8_t ist = 0, rs = 0, rv = 0, rr = 0;
        readReg8(REG_RESULT__INTERRUPT_STATUS_GPIO, ist);
        readReg8(REG_RESULT__RANGE_STATUS, rs);
        readReg8(REG_RESULT__RANGE_VAL, rv);
        readReg8(0x064, rr);
        ESP_LOGW(TAG, "诊断[%d] int=0x%02X status=0x%02X val=0x%02X raw=0x%02X",
                 i, ist, rs, rv, rr);
        if ((ist & 0x07) == 0x04) {
            final_ist = ist; final_rs = rs; final_rv = rv; final_rr = rr;
        }
    }
    writeReg8(REG_SYSTEM__INTERRUPT_CLEAR, 0x07);
    if (final_ist == 0) {
        ESP_LOGE(TAG, "2 秒内 int_stat 恒为 0：测量引擎未启动 → 模块损坏或克隆芯片");
    } else {
        ESP_LOGE(TAG, "引擎跑过但报错误：status=0x%02X val=0x%02X", final_rs, final_rv);
    }
    return false;  // 终态：报告诊断结果
}

int Vl6180x::readRangeSingleMillimeters() {
    last_range_status_ = 0xFF;

    // 清除历史中断
    if (!writeReg8(REG_SYSTEM__INTERRUPT_CLEAR, 0x07)) return -1;
    // 启动单次测距
    if (!writeReg8(REG_SYSRANGE__START, 0x01)) return -1;

    // 轮询测距完成（最多 500ms）
    for (int i = 0; i < 250; ++i) {
        uint8_t status = 0;
        if (!readReg8(REG_RESULT__INTERRUPT_STATUS_GPIO, status)) return -1;
        if ((status & 0x07) == 0x04) {
            uint8_t range_val = 0;
            if (!readReg8(REG_RESULT__RANGE_VAL, range_val)) return -1;
            readReg8(REG_RESULT__RANGE_STATUS, last_range_status_);
            // 清中断，为下次测量做准备
            writeReg8(REG_SYSTEM__INTERRUPT_CLEAR, 0x07);
            return (int)range_val * scaling_;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    // 超时诊断：打印中断状态/启动位/测距状态/距离值
    {
        uint8_t ist = 0, start = 0, rs = 0, rv = 0;
        readReg8(REG_RESULT__INTERRUPT_STATUS_GPIO, ist);
        readReg8(REG_SYSRANGE__START, start);
        readReg8(REG_RESULT__RANGE_STATUS, rs);
        readReg8(REG_RESULT__RANGE_VAL, rv);
        ESP_LOGW(TAG, "测距超时 (int_stat=0x%02X start=0x%02X range_stat=0x%02X range_val=0x%02X)",
                 ist, start, rs, rv);
    }
    writeReg8(REG_SYSTEM__INTERRUPT_CLEAR, 0x07);
    return -1;
}
