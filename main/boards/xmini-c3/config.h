#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_10
#define AUDIO_I2S_GPIO_WS GPIO_NUM_6
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_8
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_7
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_5

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_11
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_3
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_4
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR

// ⚠️ GPIO2 在这块成品板上是**电池电压检测脚**，不能当 LED 用 ——
//  原厂 WHA103 固件就是用 ADC_CHANNEL_2（=GPIO2）读电量的（固件里能搜到
//  adc_oneshot_read(adc_handle_, ADC_CHANNEL_2, ...)）。
//  若把 GPIO2 配置成推挽输出去点灯，会把电池分压的电平拉死，ADC 读到的是
//  LED 的高低电平而不是电池电压 —— 电量就永远不准。
//  所以 LED 挪到 GPIO1（板上没接东西，只是给 Led 接口留个去处，不影响别的）。
#define BUILTIN_LED_GPIO        GPIO_NUM_1
#define BOOT_BUTTON_GPIO        GPIO_NUM_9

#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

#endif // _BOARD_CONFIG_H_
