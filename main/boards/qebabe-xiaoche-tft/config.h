#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Simplex I2S: MAX98357A 功放 + I2S MEMS 麦克风（与云台小智同接法）
#define AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
// GPIO47 已让给 TOF050C 的 I2C SDA（触摸说话键取消，按键未接线）
// #define TOUCH_BUTTON_GPIO       GPIO_NUM_47
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_40
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39

// ---------- ST7789 240x240 SPI TFT ----------
#define DISPLAY_SDA GPIO_NUM_42   // SPI MOSI (模块上的 SDA/DIN 脚)
#define DISPLAY_SCL GPIO_NUM_41   // SPI SCLK (模块上的 SCL/SCK 脚)
#define DISPLAY_DC  GPIO_NUM_8
#define DISPLAY_CS  GPIO_NUM_9
#define DISPLAY_RES GPIO_NUM_10
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_SWAP_XY  false
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_17
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// A MCP Test: Control a lamp
#define LAMP_GPIO GPIO_NUM_18

// Motor control pins for TC1508 module (N20 motors as wheels)
#define MOTOR_LF_GPIO GPIO_NUM_12   // Left Forward
#define MOTOR_LB_GPIO GPIO_NUM_13   // Left Backward
#define MOTOR_RF_GPIO GPIO_NUM_14   // Right Forward
#define MOTOR_RB_GPIO GPIO_NUM_21   // Right Backward

#endif // _BOARD_CONFIG_H_
