#ifndef __WHA103_POWER_MANAGER_H__
#define __WHA103_POWER_MANAGER_H__

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_timer.h>
#include <esp_log.h>

// ─────────────────────────────────────────────────────────────────────
//  电池电量检测（ADC 采样）
//
//  ⚠️ 下面这些参数**不是猜的**，是从原厂 WHA103 成品固件里挖出来的。
//     在 factory-original-WHA103-20261019-full.bin 里能搜到这些字符串：
//       · adc_oneshot_read(adc_handle_, ADC_CHANNEL_2, &adc_value)
//       · "ADC value: %d average: %ld level: %ld"
//       · esp_timer_start_periodic(timer_handle_, 1000000)   ← 每秒一次
//       · /main/boards/WHA103/power_manager.h                ← 原版源码路径
//     ESP32-C3 上 ADC_CHANNEL_2 对应 **GPIO2**。
//     也就是说这块成品板上 GPIO2 实际接的是电池分压检测电路 ——
//     我们 fork 用的 xmini-c3 的 config.h 把 GPIO2 标成了 BUILTIN_LED_GPIO，
//     那是抄配置抄来的（原厂板型是独立的 WHA103Board，不是 xmini-c3）。
//
//  ⚠️ 换算阈值 kAdcEmpty / kAdcFull **还没标定**：原厂固件里这两个是代码里的
//     数值常量（不是字符串，二进制里挖不出来）。先填典型值，烧进去看串口打的
//     "ADC value: N" 真实读数再校准 —— 日志格式特意跟原厂保持一致，方便直接对比。
// ─────────────────────────────────────────────────────────────────────
class PowerManager {
public:
    PowerManager() = default;

    void Initialize() {
        adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id  = ADC_UNIT_1,
            .clk_src  = (adc_oneshot_clk_src_t)0,   // 0 = 默认时钟源
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        if (adc_oneshot_new_unit(&unit_cfg, &adc_handle_) != ESP_OK) {
            ESP_LOGE(TAG, "ADC1 初始化失败，电量不可用");
            return;
        }

        // 12dB 衰减 → 量程约 0~3.3V（电池经板载分压后落在这个范围内）
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten    = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_oneshot_config_channel(adc_handle_, ADC_CHANNEL_2, &chan_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "ADC 通道配置失败，电量不可用");
            return;
        }

        // 每秒读一次（跟原厂一致：1000000us）
        esp_timer_create_args_t timer_args = {
            .callback        = TimerCallback,
            .arg             = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name            = "battery_check_timer",
        };
        if (esp_timer_create(&timer_args, &timer_handle_) == ESP_OK) {
            esp_timer_start_periodic(timer_handle_, 1000000);
        }
        ready_ = true;
        ESP_LOGI(TAG, "电池检测已启动：ADC1 CH2 (=GPIO2)，每秒采样");
    }

    // true = 有真实电量数据（level 填 0..100）
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) {
        charging    = false;    // 这块板没有充电检测引脚
        discharging = true;
        if (!has_data_) return false;
        level = level_;
        return true;
    }

private:
    static void TimerCallback(void* arg) {
        static_cast<PowerManager*>(arg)->ReadBatteryAdcData();
    }

    void ReadBatteryAdcData() {
        if (!ready_) return;
        int raw = 0;
        if (adc_oneshot_read(adc_handle_, ADC_CHANNEL_2, &raw) != ESP_OK) return;

        // 滑动平均（原厂日志里也有 average 这一项）
        // 8 次滑动平均：实测瞬时值有约 ±130 的抖动（2782~2911），
        //   滤波太浅的话电量百分比会跟着跳，看着像坏了。
        avg_ = (sample_count_ == 0) ? raw : (avg_ * 7 + raw) / 8;
        sample_count_++;

        int lvl = (int)((avg_ - kAdcEmpty) * 100 / (kAdcFull - kAdcEmpty));
        if (lvl < 0)   lvl = 0;
        if (lvl > 100) lvl = 100;
        level_    = lvl;
        has_data_ = true;

        ESP_LOGI(TAG, "ADC value: %d average: %ld level: %ld", raw, avg_, (long)level_);
    }

    static constexpr const char* TAG = "PowerManager";

    // 标定依据（2026-10-21 实测）：
    //   插着 USB 时读到 ADC ≈ 2840（瞬时 2782~2911），此时电池接近满电。
    //   设满电 4.2V → 2840，按线性比例推空电 3.0V → 2840×(3.0/4.2) ≈ 2029。
    //   ⚠️ 这是**第一次标定**，还需要再验证一次：拔掉 USB、纯电池供电时看读数，
    //      若那时明显低于 2840 而电量仍显示很高，就把 kAdcFull 往下调。
    static constexpr long kAdcEmpty = 2050;
    static constexpr long kAdcFull  = 2900;

    adc_oneshot_unit_handle_t adc_handle_   = nullptr;
    esp_timer_handle_t        timer_handle_ = nullptr;
    bool ready_         = false;
    bool has_data_      = false;
    int  level_         = 0;
    long avg_           = 0;
    int  sample_count_  = 0;
};

#endif  // __WHA103_POWER_MANAGER_H__
