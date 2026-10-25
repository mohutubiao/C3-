#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// 如果使用 Duplex I2S 模式，请注释下面一行
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#else

#define AUDIO_I2S_GPIO_WS GPIO_NUM_4
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_5
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_7

#endif


#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_47
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_40
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39

#define DISPLAY_SDA_PIN GPIO_NUM_41
#define DISPLAY_SCL_PIN GPIO_NUM_42
#define DISPLAY_WIDTH   128

#if CONFIG_OLED_SSD1306_128X32
#define DISPLAY_HEIGHT  32
#elif CONFIG_OLED_SSD1306_128X64
#define DISPLAY_HEIGHT  64
#elif CONFIG_OLED_SH1106_128X64
#define DISPLAY_HEIGHT  64
#define SH1106
#else
#error "未选择 OLED 屏幕类型"
#endif

#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true


/* ==================== 步进电机配置 ==================== */
/** 步进电机IN1引脚 (ULN2003输入1)
 *  已调整为安全的通用GPIO，避免占用UART0/I2S等特殊引脚 */
/* 使用数字常量避免静态分析对 GPIO_NUM_* 宏的误报 */ 
#define STEPPER_IN1_GPIO 8
/** 步进电机IN2引脚 (ULN2003输入2) */
#define STEPPER_IN2_GPIO 19
/** 步进电机IN3引脚 (ULN2003输入3) */
#define STEPPER_IN3_GPIO 20
/** 步进电机IN4引脚 (ULN2003输入4) */
#define STEPPER_IN4_GPIO 3

// A MCP Test: Control a lamp
#define LAMP_GPIO GPIO_NUM_18

#endif // _BOARD_CONFIG_H_
