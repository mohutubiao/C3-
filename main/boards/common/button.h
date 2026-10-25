#ifndef BUTTON_H_
#define BUTTON_H_

#include <driver/gpio.h>
#include <iot_button.h>
#include <button_types.h>
#include <button_adc.h>
#include <button_gpio.h>
#include <functional>

class Button {
public:
    Button(button_handle_t button_handle);
    Button(gpio_num_t gpio_num, bool active_high = false, uint16_t long_press_time = 0, uint16_t short_press_time = 0, bool enable_power_save = false);
    ~Button();

    void OnPressDown(std::function<void()> callback);
    void OnPressUp(std::function<void()> callback);
    void OnLongPress(std::function<void()> callback);
    void OnClick(std::function<void()> callback);
    void OnDoubleClick(std::function<void()> callback);
    void OnMultipleClick(std::function<void()> callback, uint8_t click_count = 3);

protected:
    gpio_num_t gpio_num_;
    button_handle_t button_handle_ = nullptr;

    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_long_press_;
    std::function<void()> on_click_;
    std::function<void()> on_double_click_;
    std::function<void()> on_multiple_click_;

    // ⚠️ BUG 修复：多击次数配置**不能放在栈上**。
    //   原来 OnMultipleClick() 里是 `button_event_args_t event_args = {...}`，
    //   然后把这个**局部变量的地址**交给 iot_button_register_cb()；
    //   函数一返回，这块栈内存就被回收了 —— 按键驱动后来判断"点了几次"
    //   时读的是野指针，行为未定义，实测三击会直接把设备搞重启。
    //   改成成员变量，跟 Button 同生命周期，地址一直有效。
    button_event_args_t multiple_click_args_ = {};
};

#if CONFIG_SOC_ADC_SUPPORTED
class AdcButton : public Button {
public:
    AdcButton(const button_adc_config_t& adc_config);
};
#endif

class PowerSaveButton : public Button {
public:
    PowerSaveButton(gpio_num_t gpio_num) : Button(gpio_num, false, 0, 0, true) {
    }
};

#endif // BUTTON_H_
