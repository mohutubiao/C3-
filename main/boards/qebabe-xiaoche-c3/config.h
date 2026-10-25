#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// 强制 Simplex（麦克风和功放两组独立 I2S，共 6 根引脚）
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

// ── 麦克风侧（MicroDeer 模块丝印 WS / SCK / SD）─────────────
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_4    // → 模块 SCK
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_5    // → 模块 WS
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6    // → 模块 SD

// ── 功放侧（MicroDeer 模块丝印 BCLK / LRCLK / DIN）────────
// ⚠️ GPIO18/19 = USB D-/D+ 物理数据线，绝对不能用（用了 USB 直接掉线）
//    GPIO20/21 = UART0 TX/RX（console），需在 sdkconfig 关掉 secondary
//    console 后让给 I2S（日志走 USB CDC 不受影响）
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_10   // → 模块 BCLK
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_20   // → 模块 LRCLK（需关 UART console）
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_21   // → 模块 DIN

#endif

// OLED SSD1306 0.96 寸 4 脚（I2C, 地址 0x3C）
//   模块自带 4.7K 上拉（市售 4 脚 OLED 基本都带），
//   同时也满足 GPIO8/GPIO9 的 strapping 启动高电平要求
#define DISPLAY_SDA_PIN GPIO_NUM_9
#define DISPLAY_SCL_PIN GPIO_NUM_8
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

// 4 路差速电机（HW-358 / MX1508 双 H 桥，4 个方向脚直接控制，无 ENA/ENB）
//
// ✅ 2026-10-16 结论：LF 已从 GPIO0 迁到 GPIO2（解决关机左轮自转）
//   实验依据：关机状态下用镊子把 GPIO0 短接到 GND，左轮立刻不转
//     → 证明「不是」MX1508 的输入悬空判高（否则 GPIO1/3/7 那边也会转），
//       而是 GPIO0 这条线被某个上拉/残压撑在高电平，IN1=1 / IN2=0 → 左轮正转。
//     → GPIO2 没有板载上拉，断电后是纯悬空，会被 MX1508 输入端的下拉拉低
//       → IN1=IN2=低 → MX1508 真值表的 Coast（输出断开）→ 不转。
//   ⚠️ 代价：GPIO2 是 C3 的 strapping 脚，复位期间内部上拉，开机瞬间 IN1 会
//      有一百多毫秒的高电平 → 左轮"抖一下"。若不能接受，就给 IN1 加一个 10k
//      下拉电阻到 GND，或把驱动板电源改成开关控制。
//   ⚠️ 固件与接线必须同步：MX1508 的 IN1 杜邦线要从 GPIO0 拔下来插到 GPIO2。
//      若暂时不改线，把下面的 2 改回 0，否则左轮完全不动。
#define MOTOR_LF_GPIO GPIO_NUM_2   // Left Forward  (MX1508 IN1)
#define MOTOR_LB_GPIO GPIO_NUM_1   // Left Backward (MX1508 IN2)
#define MOTOR_RF_GPIO GPIO_NUM_3   // Right Forward (MX1508 IN3)
#define MOTOR_RB_GPIO GPIO_NUM_7   // Right Backward(MX1508 IN4)

// 板载 BOOT 按键：靠板载上拉保持高，按下接地 → 低电平有效（active_low）
//   一个键管三件事（逻辑全在 Application::HandleToggleChatEvent() 里，按键只管触发）：
//     空闲 / 时钟界面 → 按下 = 唤醒，开始聆听
//     说话中          → 按下 = 打断（AbortSpeaking，服务端停止下发音频）
//     聆听中          → 按下 = 关闭聆听 → 状态回 idle → 自动恢复时钟界面
// ⚠️ 为什么是 GPIO0：LF 迁到 GPIO2 后它是唯一的空脚，而且它自带板载上拉
//    —— 那颗上拉既是"关机左轮自转"的元凶，也正是按键需要的上拉。
// ⚠️ GPIO9 虽然是多数 C3 板的 BOOT 脚，但这里已被 OLED 的 I2C SCL 占用：
//    拿它当按键，按下瞬间就把 SCL 拉低 → I2C 卡死，绝对不能用。
// ⚠️ 按下没反应 = 板子 BOOT 其实在别的脚，改这一行的 GPIO 号即可。
#define BOOT_BUTTON_GPIO GPIO_NUM_0

// 引脚占用一览（12 IO 全部分配，无富余）：
//   电机差速 : GPIO 1, 2, 3, 7     ← LF 已从 GPIO0 迁到 GPIO2（见上方说明）
//   麦克风   : GPIO 4, 5, 6
//   功放     : GPIO 10, 20, 21
//   OLED I2C : GPIO 8, 9
//   BOOT 按键: GPIO 0
//
// ⚠️ GPIO0 带板载上拉（断电后仍把该脚撑在高电平，就是"关机左轮自转"的元凶），
//    所以它【只能】做按键这类"开机才有意义"的输入，绝不能驱动电机
//    或任何"关机后必须为低"的负载。当按键反而正好：上拉 + 按下接地。
//
// GPIO2 已用于电机 LF：它是 strapping 脚但只有内部弱上拉，作 PWM 输出运行时安全；
//   复位瞬间会被内部上拉拉高（约 135ms，PWM 接管后即被驱动为低）。
//   若要彻底消除这 135ms，给 L298N 电源加 RC 延时或 MOS 管即可。
// 避开：GPIO 18, 19 (USB D-/D+，占用后 USB 掉线)
//       GPIO 20/21 原为 UART0 console，已在 sdkconfig 切到 USB CDC 后让给 I2S

#endif // _BOARD_CONFIG_H_