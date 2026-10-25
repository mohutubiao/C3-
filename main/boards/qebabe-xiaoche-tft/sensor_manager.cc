#include "sensor_manager.h"
#include "vl6180x.h"

#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mcp_server.h"

#define TAG "SensorManager"

// TOF050C (VL6180X) I2C 地址
static constexpr uint8_t kTofAddr = 0x29;

// 飞线接法：TOF050C SDA -> GPIO47, SCL -> GPIO11, SHUT -> GPIO1
// 注意：本板是八线 PSRAM (N16R8)，GPIO35/36/37 被 PSRAM 占用，不能用！
static constexpr int kTofSda = 47;
static constexpr int kTofScl = 11;

// TOF050C 第 6 脚 SHUT（VL6180X 的 GPIO0/CE 芯片使能脚，低电平关断）。
// 手册要求必须上拉为高；厂商 STM32 demo 会先拉低再拉高做一次干净复位。
// 悬空会导致芯片上电复位时序不完整（FRESH_OUT_OF_RESET 恒为 0，测量引擎不工作）。
static constexpr int kTofShut = 1;

namespace {

i2c_master_bus_handle_t g_tof_bus = nullptr;
Vl6180x* g_tof = nullptr;

// 在指定 SDA/SCL 上创建持久 I2C 总线
i2c_master_bus_handle_t create_bus(int port, int sda, int scl) {
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = port;
    cfg.sda_io_num = (gpio_num_t)sda;
    cfg.scl_io_num = (gpio_num_t)scl;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        return nullptr;
    }
    return bus;
}

}  // namespace

void SensorManager::Initialize() {
    if (i2c_ready_) {
        return;
    }
    i2c_ready_ = true;

    ESP_LOGI(TAG, "初始化 TOF050C(VL6180X): SDA=GPIO%d SCL=GPIO%d SHUT=GPIO%d",
             kTofSda, kTofScl, kTofShut);

    // 1. 复位时序（复刻厂商 STM32 demo 的 SetChipEnable）：
    //    拉低 → 芯片关断；拉高 → 释放并使能，芯片完成一次干净的上电复位
    gpio_config_t shut_cfg = {};
    shut_cfg.pin_bit_mask = 1ULL << kTofShut;
    shut_cfg.mode = GPIO_MODE_OUTPUT;
    shut_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    shut_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    shut_cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&shut_cfg) == ESP_OK) {
        gpio_set_level((gpio_num_t)kTofShut, 0);
        vTaskDelay(pdMS_TO_TICKS(20));   // 保持关断，让芯片彻底复位
        gpio_set_level((gpio_num_t)kTofShut, 1);
        vTaskDelay(pdMS_TO_TICKS(100));  // 等芯片启动（手册：上电后需 >1ms）
        ESP_LOGI(TAG, "SHUT 复位时序完成 (低 20ms → 高)");
    } else {
        ESP_LOGW(TAG, "SHUT(GPIO%d) 配置失败，芯片使能可能悬空", kTofShut);
    }

    g_tof_bus = create_bus(I2C_NUM_0, kTofSda, kTofScl);
    if (g_tof_bus == nullptr) {
        ESP_LOGE(TAG, "TOF050C: 创建 I2C 总线失败");
        return;
    }

    i2c_device_config_t tof_cfg = {};
    tof_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    tof_cfg.device_address = kTofAddr;
    tof_cfg.scl_speed_hz = 10000;  // 飞线较长且仅内部弱上拉，降速保证信号完整性
    i2c_master_dev_handle_t tof_dev = nullptr;
    if (i2c_master_bus_add_device(g_tof_bus, &tof_cfg, &tof_dev) != ESP_OK) {
        ESP_LOGE(TAG, "TOF050C: 注册 I2C 设备失败");
        return;
    }

    g_tof = new Vl6180x(tof_dev);
    if (g_tof->init()) {
        distance_ready_ = true;
        // 上电自检：读一次距离验证端到端测距
        int test_mm = g_tof->readRangeSingleMillimeters();
        ESP_LOGI(TAG, "TOF050C 测距就绪 (自检: %dmm, 状态=0x%02X)",
                 test_mm, g_tof->rangeStatus());
    } else {
        ESP_LOGE(TAG, "TOF050C 初始化失败 (无应答？检查 VCC/GND/接线顺序)");
        // 诊断：扫描总线，看是否有任何设备应答（区分断线/没供电 vs 地址不对）
        i2c_device_config_t probe_cfg = {};
        probe_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        probe_cfg.scl_speed_hz = 10000;
        int found = 0;
        for (uint8_t addr = 1; addr < 127; ++addr) {
            probe_cfg.device_address = addr;
            i2c_master_dev_handle_t probe_dev = nullptr;
            if (i2c_master_bus_add_device(g_tof_bus, &probe_cfg, &probe_dev) == ESP_OK) {
                uint8_t dummy = 0;
                if (i2c_master_transmit(probe_dev, &dummy, 1, 50) == ESP_OK) {
                    ESP_LOGW(TAG, "I2C 扫描: 地址 0x%02X 有应答", addr);
                    ++found;
                }
                i2c_master_bus_rm_device(probe_dev);
            }
        }
        if (found == 0) {
            ESP_LOGE(TAG, "I2C 扫描: 总线上无任何设备。检查 SDA/SCL 接线、VCC(3.3V)、GND 共地");
        }
    }

    // ---- 注册 MCP 工具，让 AI 能查询传感器 ----
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool(
        "self.sensor.get_distance",
        "Get the distance to the nearest obstacle in front of the robot using the laser ToF sensor.\n"
        "Returns distance in millimeters (mm). -1 means sensor unavailable or read failure, "
        "8191 means no obstacle within range (max ~200mm for this sensor).",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            int mm = ReadDistanceMm();
            return std::string("{\"distance_mm\": ") + std::to_string(mm) + "}";
        });

    ESP_LOGI(TAG, "传感器初始化完成 (distance=%d)",
             distance_ready_ ? 1 : 0);
}

int SensorManager::ReadDistanceMm() {
    if (!distance_ready_ || g_tof == nullptr) {
        return -1;
    }
    int mm = g_tof->readRangeSingleMillimeters();
    if (mm < 0) {
        return -1;  // 通信失败/超时
    }
    // range_status != 0 表示无有效目标（超量程/无信号），按"无障碍"处理
    if (g_tof->rangeStatus() != 0) {
        return 8191;
    }
    return mm;
}
