// ESP32-C3-MINI1 小车板固件 (12 IO 极限精简版)
// 保留: SSD1306 OLED + I2S Simplex 声音 + 2 路差速电机
// 砍掉: BOOT 按键、TOUCH 按键、音量键、灯、SPI 屏背光、复杂情感动作
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "config.h"
#include "mcp_server.h"
#include "motor_controller.h"
#include "web_server.h"
#include "boards/common/button.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_netif.h>
#include <string>
#include <cstdio>   // snprintf（dance 序列拼串）

#define TAG "C3Board"

extern "C" void HandleMotorActionForApplication(int direction, int speed, int duration_ms, int priority);
extern "C" void HandleMotorActionForEmotion(const char* emotion);
extern "C" void HandleMotorActionForDance(uint8_t speed_percent);
// 带时长的舞蹈（"跳 5 秒"）：旧单参数接口保持不变，供 web_server 调用
extern "C" void HandleMotorActionForDanceFor(uint8_t speed_percent, uint32_t duration_ms);
// 语音动作忙窗口登记 + 全局速度上限（网页「🐢 速度」滑条，NVS 持久化）
extern "C" void MarkMotorBusyForApp(uint32_t duration_ms);
extern "C" int  GetGlobalMaxSpeedPct();

// 全局速度上限系数：AI 给的 speed_percent / throttle / steer 都先乘它。
//   ⚠️ 必须是自由函数，不能写成 RegisterMcpTools() 里的局部 lambda ——
//      各 MCP 工具的回调只捕获 [this]，捕获局部变量会报 "is not captured"。
static int ApplySpeedLimit(int v) {
    return v * GetGlobalMaxSpeedPct() / 100;
}

class CompactWifiBoard : public WifiBoard {
private:
    Button boot_button_;               // 板载 BOOT 按键（GPIO0：板载上拉 + 按下接地）
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Motor2WD* motor_ = nullptr;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = (i2c_port_t)0;
        bus_config.sda_io_num = DISPLAY_SDA_PIN;
        bus_config.scl_io_num = DISPLAY_SCL_PIN;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.trans_queue_depth = 0;
        bus_config.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = 0x3C;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        io_config.scl_speed_hz = 400 * 1000;
        esp_err_t err = esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "panel_io create failed (%s), skip OLED", esp_err_to_name(err));
            return;
        }

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = (uint8_t)DISPLAY_HEIGHT,
        };
        panel_config.vendor_config = &ssd1306_config;
        err = esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ssd1306 create failed (%s), skip OLED", esp_err_to_name(err));
            return;
        }

        // ⚠️ 关键：panel_init 会真的往 0x3C 写命令，无 OLED 时 I2C 无 ACK
        //    不能 ESP_ERROR_CHECK，否则 abort + 看门狗重启死循环
        err = esp_lcd_panel_init(panel_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED panel_init failed (%s) — no display on I2C bus. "
                          "降级为无显示模式（OledDisplay(nullptr, nullptr) 占位）",
                     esp_err_to_name(err));
            // 清理已分配的资源
            esp_lcd_panel_del(panel_);
            esp_lcd_panel_io_del(panel_io_);
            i2c_del_master_bus(display_i2c_bus_);
            panel_ = nullptr;
            panel_io_ = nullptr;
            display_i2c_bus_ = nullptr;
            // 仍创建一个 OledDisplay(nullptr, nullptr) 占位 —— 这样 application.cc
            // 调 display->SetEmotion / SetStatus / ShowNotification 不会 segfault
            // (OledDisplay 内部已支持 panel_=nullptr：PushFrame 立即返回，Lock
            //  也判 nullptr；其他方法都基于 fb_ 栈数组，panel 没参与)
            display_ = new OledDisplay(nullptr, nullptr,
                                       DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                       DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
            return;
        }

        // ⚠️ 关键 bug 修复：esp_lcd_panel_ssd1306 的 init() 函数（v5.5.x）只配置了
        //    charge pump / multiplex / COM pins，但**没有发 0xAF (DISP_ON)**！
        //    结果 RAM 写满了但屏幕一直黑屏。手动补一行打开显示。
        //    参数 true = DISP_ON；false = DISP_OFF（API 名字易混淆）
        esp_err_t on_ret = esp_lcd_panel_disp_on_off(panel_, true);
        if (on_ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_lcd_panel_disp_on_off returned %s", esp_err_to_name(on_ret));
        }

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    // ── 距离传感器探测（VL53L0X @ 0x29）──────────────────────────────
    //   ⚠️ ESP32-C3 只有 1 个 I2C 控制器（I2C_NUM_0），已被 OLED 占用，
    //      所以这里【必须】复用 display_i2c_bus_，绝不能新建总线 ——
    //      再调 i2c_new_master_bus(I2C_NUM_0) 会返回 ESP_ERR_INVALID_STATE。
    //      这正是"S3 板装距离传感器与屏幕共用总线冲突"最可能的成因。
    //   VL53L0X 与 SSD1306 地址不同（0x29 vs 0x3C），可共存于同一总线。
    //   本函数只验证"在不在"（读 MODEL_ID），真正的连续测距等模块到货再接。
    std::string ProbeDistanceSensor() {
        if (display_i2c_bus_ == nullptr) {
            return "{\"found\":false,\"err\":\"i2c bus not initialized\"}";
        }

        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = 0x29;      // VL53L0X 出厂默认 7 位地址
        dev_cfg.scl_speed_hz = 400 * 1000;

        i2c_master_dev_handle_t dev = nullptr;
        esp_err_t err = i2c_master_bus_add_device(display_i2c_bus_, &dev_cfg, &dev);
        if (err != ESP_OK) {
            return std::string("{\"found\":false,\"err\":\"add_device: ") + esp_err_to_name(err) + "\"}";
        }

        // VL53L0X 寄存器为 8 位地址：0xC0 = IDENTIFICATION__MODEL_ID，正常读回 0xEE
        uint8_t reg = 0xC0;
        uint8_t model_id = 0;
        err = i2c_master_transmit_receive(dev, &reg, 1, &model_id, 1, 100);

        // 只移除本设备，不销毁总线 —— OLED 仍在正常使用同一条总线
        i2c_master_bus_rm_device(dev);

        if (err != ESP_OK) {
            return std::string("{\"found\":false,\"err\":\"no ack at 0x29: ") + esp_err_to_name(err) + "\"}";
        }

        return std::string("{\"found\":true,\"addr\":\"0x29\",\"model_id\":")
             + std::to_string((int)model_id) + "}";
    }

    void InitializeMotor() {
        motor_ = new Motor2WD(
            (gpio_num_t)MOTOR_LF_GPIO, (gpio_num_t)MOTOR_LB_GPIO,
            (gpio_num_t)MOTOR_RF_GPIO, (gpio_num_t)MOTOR_RB_GPIO);
        motor_->InitPWM();
    }

    // 板载 BOOT 按键：一个键走完「唤醒 / 打断 / 回时钟」
    //   空闲（时钟界面） → 按下 = 唤醒，开始聆听
    //   说话中           → 按下 = 打断（AbortSpeaking，服务端停止下发音频）
    //   聆听中           → 按下 = 关闭聆听 → 状态自动回 idle → 恢复时钟界面
    // ⚠️ 这三段逻辑 Application::HandleToggleChatEvent() 里全都已经实现好了，
    //    所以按键只负责调 ToggleChatState()，这里**不要**自己去动
    //    EnableVoiceProcessing / ShowClock —— 状态机统一管，手动改会和 wake word 抢麦。
    void InitializeBootButton() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "BOOT 按下, 当前状态=%d", (int)app.GetDeviceState());
            app.ToggleChatState();
        });
    }

    void RegisterMcpTools() {
        auto& mcp = McpServer::GetInstance();

        // ── 距离传感器连通性查询 ──────────────────────────────────────
        //   VL53L0X 挂在 OLED 同一条 I2C 总线（GPIO8/9）上，地址 0x29。
        //   本工具当前只报告"芯片在不在"（model_id 应为 238 = 0xEE），
        //   真正的连续测距读数等模块到货、驱动接入后再补。
        mcp.AddTool("self.sensor.get_distance",
            "Probe the I2C bus shared with the OLED for a VL53L0X laser distance sensor. "
            "Returns {\"found\":true,\"addr\":\"0x29\",\"model_id\":238} when present "
            "(model_id 238 = 0xEE = VL53L0X), or found:false with an error reason.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                return ProbeDistanceSensor();
            });

        // 速度上限走文件顶部的 ApplySpeedLimit()（自由函数，无需捕获）

        mcp.AddTool("self.motor.move_forward",
            "Move forward. Args: speed_percent (0-100, default 100), duration_ms (default 5000)",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 5000, 100, 10000)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                int s = p["speed_percent"].value<int>();
                int d = p["duration_ms"].value<int>();
                MarkMotorBusyForApp((uint32_t)d);   // 登记忙窗口：防装饰动作掐断
                motor_->Execute(MOTOR_FORWARD, (uint8_t)ApplySpeedLimit(s), (uint32_t)d);
                return "forward";
            });

        mcp.AddTool("self.motor.move_backward",
            "Move backward. Args: speed_percent (0-100, default 100), duration_ms (default 5000)",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 5000, 100, 10000)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                int s = p["speed_percent"].value<int>();
                int d = p["duration_ms"].value<int>();
                MarkMotorBusyForApp((uint32_t)d);
                motor_->Execute(MOTOR_BACKWARD, (uint8_t)ApplySpeedLimit(s), (uint32_t)d);
                return "backward";
            });

        mcp.AddTool("self.motor.turn_left",
            "Turn left in place. Args: speed_percent (default 100), duration_ms (default 500)",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 500, 100, 5000)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                int s = p["speed_percent"].value<int>();
                int d = p["duration_ms"].value<int>();
                MarkMotorBusyForApp((uint32_t)d);
                motor_->Execute(MOTOR_TURN_LEFT, (uint8_t)ApplySpeedLimit(s), (uint32_t)d);
                return "left";
            });

        mcp.AddTool("self.motor.turn_right",
            "Turn right in place. Args: speed_percent (default 100), duration_ms (default 500)",
            PropertyList({
                Property("speed_percent", kPropertyTypeInteger, 100, 0, 100),
                Property("duration_ms", kPropertyTypeInteger, 500, 100, 5000)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                int s = p["speed_percent"].value<int>();
                int d = p["duration_ms"].value<int>();
                MarkMotorBusyForApp((uint32_t)d);
                motor_->Execute(MOTOR_TURN_RIGHT, (uint8_t)ApplySpeedLimit(s), (uint32_t)d);
                return "right";
            });

        mcp.AddTool("self.motor.stop", "Stop motors immediately",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                motor_->Stop();
                return "stopped";
            });

        // ── 摇杆差速：LLM 自己编排动作序列 ──
        //   steer: -100..+100 (左/右，±100 原地转)
        //   throttle: -100..+100 (前/后)
        //   duration_ms: 持续时间
        mcp.AddTool("self.motor.drive",
            "Differential steering like a car wheel. "
            "steer: -100..100 (left/right, ±100 = spin in place); "
            "throttle: -100..100 (backward/forward); "
            "duration_ms: 100..10000. "
            "Use this to compose complex sequences.",
            PropertyList({
                Property("steer", kPropertyTypeInteger, 0, -100, 100),
                Property("throttle", kPropertyTypeInteger, 0, -100, 100),
                Property("duration_ms", kPropertyTypeInteger, 500, 100, 10000)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                int s = p["steer"].value<int>();
                int t = p["throttle"].value<int>();
                int d = p["duration_ms"].value<int>();
                // 关键修复：登记忙窗口。AI 喊"前进 4 秒"走的就是这里，
                // 原来紧随其后的 200ms 表情 twitch 会从队列抢占并把它掐断，
                // 用户看到的就是"只动一下"、"转圈只转一点点"。
                MarkMotorBusyForApp((uint32_t)d);
                motor_->Drive(ApplySpeedLimit(s), ApplySpeedLimit(t), (uint32_t)d);
                return "driving";
            });

        // ── 动作序列：一次下发多段，固件内连续执行 ────────────────────
        //   为什么要它：LLM 连调多次 self.motor.drive 时，每次调用都要走
        //   一轮 WS 往返（服务端推理 + 传输 + 两次事件循环），段与段之间
        //   会明显停顿，车"走走停停"；而且每段都重新走一次 ramp 软启动。
        //   序列把整段压成一次调用，由固件按 10ms 精度背靠背执行。
        //   执行体在 Motor2WD::SequenceTaskEntry（独立低优先级任务），
        //   ⚠️ 绝不能在工具回调里同步循环 —— 工具是在 Application 主循环
        //   里执行的（见 mcp_server.cc:553 的 app.Schedule），阻塞会冻住
        //   音频采集/唤醒词检测/屏幕刷新。
        mcp.AddTool("self.motor.sequence",
            "Play a multi-step movement in ONE call, steps executed back-to-back with no gap. "
            "steps format: \"steer,throttle,ms;steer,throttle,ms;...\" "
            "steer: -100..100 (negative=left, positive=right, +/-100 = spin in place). "
            "throttle: -100..100 (positive=forward, negative=backward). "
            "ms: 100..5000 per step. Up to 24 steps, total <= 15000ms. "
            "Example circle: \"0,80,800;70,80,900;0,80,800;-70,80,900\". "
            "Example zigzag: \"-60,90,600;60,90,600;-60,90,600\". "
            "Example nod: \"0,50,250;0,-50,250;0,50,250\". "
            "Returns immediately and the car keeps moving on its own; "
            "call self.motor.stop to abort. "
            "PREFER this over calling self.motor.drive multiple times for any multi-step move.",
            PropertyList({
                Property("steps", kPropertyTypeString)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                std::string steps = p["steps"].value<std::string>();
                // 全局速度上限同样作用于序列（等比缩放，差速比不变）
                uint32_t total = motor_->PlaySequence(steps.c_str(), GetGlobalMaxSpeedPct());
                if (total == 0) {
                    return "rejected: bad format, over limit, or a sequence is already running";
                }
                // 忙窗口按整段总时长登记：否则序列跑一半就被 200ms 表情 twitch
                // 抢占（和之前"只动一下"是同一个机制）
                MarkMotorBusyForApp(total);
                return "sequence started (" + std::to_string(total) + " ms)";
            });

        // ── 预设积木盒：21 种动作，LLM 直接调用 ──
        //   action ∈ {forward, backward, spin_around, turn_left, turn_right,
        //              quick_forward, quick_backward, wiggle, dance, stop,
        //              wake_up, happy, sad, thinking, listening, speaking,
        //              excited, loving, angry, surprised, confused}
        mcp.AddTool("self.car.act",
            "Execute a preset action by name. "
            "Available actions: forward, backward, spin_around, turn_left, turn_right, "
            "quick_forward, quick_backward, wiggle, dance, stop, "
            "wake_up, happy, sad, thinking, listening, speaking, "
            "excited, loving, angry, surprised, confused. "
            "duration_ms (optional, 0 = action's own default): how long to run it. "
            "IMPORTANT: when the user says 'dance for 5 seconds' / 'walk forward 3 seconds', "
            "you MUST pass duration_ms=5000 / 3000 - otherwise the action uses its short "
            "built-in default (dance 1.1s, forward 1.5s) and the user will complain it was too short. "
            "dance with a duration loops the dance until the requested time. "
            "Some actions (e.g. dance, happy) also trigger an emotion expression on the face.",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("duration_ms", kPropertyTypeInteger, 0, 0, 15000)
            }),
            [this](const PropertyList& p) -> ReturnValue {
                std::string a = p["action"].value<std::string>();
                int want_ms = p["duration_ms"].value<int>();   // 0 = 用动作默认时长
                auto display = Board::GetInstance().GetDisplay();

                // 离散方向走快速通道
                //   ⚠️ spin_around 复用 MOTOR_TURN_LEFT（左轮后退 + 右轮前进 = 原地转），
                //      与 turn_left 唯一区别是时长；给足 1500ms 才转得成圈。
                //   want_ms > 0 时覆盖默认时长（"前进 3 秒"靠这个）。
                auto go = [&](MotorDir dir, int pct, int ms) {
                    uint32_t d = (want_ms > 0) ? (uint32_t)want_ms : (uint32_t)ms;
                    MarkMotorBusyForApp(d);   // 登记忙窗口
                    motor_->Execute(dir, (uint8_t)ApplySpeedLimit(pct), d);
                };
                if (a == "forward")        { go(MOTOR_FORWARD,    100, 1500); return "forward"; }
                if (a == "backward")       { go(MOTOR_BACKWARD,   100, 1500); return "backward"; }
                if (a == "turn_left")      { go(MOTOR_TURN_LEFT,  100,  500); return "left"; }
                if (a == "turn_right")     { go(MOTOR_TURN_RIGHT, 100,  500); return "right"; }
                if (a == "spin_around")    { go(MOTOR_TURN_LEFT,  100, 1500); return "spin"; }
                if (a == "quick_forward")  { go(MOTOR_FORWARD,    100,  300); return "q_forward"; }
                if (a == "quick_backward") { go(MOTOR_BACKWARD,   100,  300); return "q_backward"; }
                if (a == "stop")           { motor_->Stop(); return "stopped"; }

                // 情感动作：电机 + 表情 联动
                if (a == "wake_up")        { display->SetEmotion("wake");     HandleMotorActionForEmotion("wake");     return "wake_up"; }
                if (a == "happy")          { display->SetEmotion("happy");    HandleMotorActionForEmotion("happy");    return "happy"; }
                if (a == "sad")            { display->SetEmotion("sad");      HandleMotorActionForEmotion("sad");      return "sad"; }
                if (a == "thinking")       { display->SetEmotion("thinking"); HandleMotorActionForEmotion("thinking"); return "thinking"; }
                if (a == "listening")      { display->SetEmotion("listening");HandleMotorActionForEmotion("listening");return "listening"; }
                if (a == "speaking")       { display->SetEmotion("speaking"); HandleMotorActionForEmotion("speaking"); return "speaking"; }
                if (a == "excited")        { display->SetEmotion("excited");  HandleMotorActionForEmotion("excited");  return "excited"; }
                if (a == "loving")         { display->SetEmotion("loving");   HandleMotorActionForEmotion("loving");   return "loving"; }
                if (a == "angry")          { display->SetEmotion("angry");    HandleMotorActionForEmotion("angry");    return "angry"; }
                if (a == "surprised")      { display->SetEmotion("surprised");HandleMotorActionForEmotion("surprised");return "surprised"; }
                if (a == "confused")       { display->SetEmotion("confused"); HandleMotorActionForEmotion("confused"); return "confused"; }

                // 复合动作：多步序列
                if (a == "wiggle")         { display->SetEmotion("confused"); HandleMotorActionForEmotion("confused"); return "wiggle"; }
                if (a == "dance")          {
                    display->SetEmotion("excited");
                    HandleMotorActionForDanceFor(100, (uint32_t)(want_ms > 0 ? want_ms : 0));
                    return "dance";
                }

                return std::string("unknown action: ") + a;
            });

        mcp.AddTool("self.network.get_ip",
            "Get current WiFi IP address",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                esp_netif_ip_info_t ip;
                if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip) == ESP_OK) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
                    return std::string("IP: ") + buf;
                }
                return std::string("not connected");
            });
    }

public:
    CompactWifiBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        // ⚠️ 电机必须最先初始化：LF 原先是 GPIO0，它带板载上拉电阻，
        //    在程序接管之前这颗上拉会把 MX1508 的 IN1 拉高 → 左轮持续自转
        //    （2026-10-16 已把 LF 迁到 GPIO2，但这个顺序留着仍然更保险）。
        //    Motor2WD 构造函数会把 4 路强制输出低（能压过上拉），所以要让它
        //    尽早跑；若排在 display I2C 后面，一旦 I2C 初始化变慢/总线卡住，
        //    电机脚就一直没人接管 → 开机左轮一直转。
        InitializeMotor();
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        RegisterMcpTools();
        InitializeBootButton();
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
#endif
    }

    virtual Display* GetDisplay() override { return display_; }

    // 给 C 风格弱符号 HandleMotorActionForEmotion() 用
    Motor2WD* motor_for_emotion() { return motor_; }
};

DECLARE_BOARD(CompactWifiBoard);

// 弱符号：Application 在收到 emotion 字段时会调这个
// 极简映射：只对 4 种 emotion 做电机动作，其他全 STOP
extern "C" void HandleMotorActionForEmotion(const char* emotion) {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    auto motor = board->motor_for_emotion();
    if (!emotion || !motor) return;

    std::string e(emotion);
    if (e == "happy" || e == "loving" || e == "wake" || e == "wakeup") {
        motor->Execute(MOTOR_FORWARD, 70, 200);
    } else if (e == "sad" || e == "crying") {
        motor->Execute(MOTOR_BACKWARD, 50, 200);
    } else if (e == "angry" || e == "surprised" || e == "shocked") {
        // 两步：后撤 → 前冲。原来用 vTaskDelay(110) 同步保序，而本函数
        // 可能被 Application 主循环调用 → 每次生气都卡主循环 110ms。
        // 改成一次序列下发，立即返回（序列忙时退化为单步）。
        uint32_t total = motor->PlaySequence("0,-80,100;0,80,150");
        if (total) {
            MarkMotorBusyForApp(total);   // 不登记会被紧随的说话/表情反馈掐断
        } else {
            motor->Execute(MOTOR_BACKWARD, 80, 100);
        }
    } else {
        motor->Stop();
    }
}

// 舞蹈：四步一轮 → 前 300 / 原地左 250 / 原地右 250 / 后 300 = 1100ms
//   duration_ms = 0  → 只跳一轮（旧行为，web_server 那条路径）
//   duration_ms = 5000 → 循环拼段直到满 5 秒
//   ⚠️ 为什么必须支持时长：原来四步时长全写死，LLM 想让"跳 5 秒"却没有任何
//      参数可传，只能得到 1.1 秒 —— 用户感知就是"说 5 秒实际跳 1 秒"。
extern "C" void HandleMotorActionForDanceFor(uint8_t speed_percent, uint32_t duration_ms) {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    auto motor = board->motor_for_emotion();
    if (!motor) return;

    int s = ApplySpeedLimit((int)speed_percent);
    if (s < 1) s = 1;

    // duration_ms = 0 → 跳一轮（1100ms）；否则循环拼段直到凑满
    uint32_t target = (duration_ms == 0) ? 1100u : duration_ms;
    if (target > 15000u) target = 15000u;

    // 一轮四步：{steer, throttle 符号, 时长}
    static const int kSegSteer[4]   = {   0, -100,  100,    0 };
    static const int kSegThrSign[4] = {   1,    0,    0,   -1 };
    static const int kSegMs[4]      = { 300,  250,  250,  300 };

    char steps[768];
    int off = 0;
    uint32_t acc = 0;
    int idx = 0;
    while (acc < target && idx < 64) {
        uint32_t remain = target - acc;
        uint32_t ms = (uint32_t)kSegMs[idx & 3];
        if (ms > remain) ms = remain;
        if (ms < 100) break;          // 剩余不足一段 → 收工，避免尾段抖动

        int n = snprintf(steps + off, sizeof(steps) - (size_t)off, "%s%d,%d,%u",
                         (idx == 0) ? "" : ";",
                         kSegSteer[idx & 3],
                         kSegThrSign[idx & 3] * s,
                         (unsigned)ms);
        if (n <= 0 || off + n >= (int)sizeof(steps)) break;
        off += n;
        acc += ms;
        ++idx;
    }
    if (idx == 0) return;

    // 速度已由 ApplySpeedLimit 乘进 throttle，这里 limit 传 100 不再二次缩放
    uint32_t total = motor->PlaySequence(steps, 100);
    if (total) {
        // ⚠️ 必须登记忙窗口：dance 原来没登记，服务端随后的 emotion /
        //    说话反馈会走 QueueMotorAction → StopRealtimeMotorControl →
        //    motor->Stop() → AbortSequence()，把舞蹈当场腰斩。
        MarkMotorBusyForApp(total);
    } else {
        motor->Execute(MOTOR_FORWARD, (uint8_t)s, 300);   // 忙/解析失败兜底
    }
}

// web_server 调：极简舞蹈（保持单参数接口不变）
extern "C" void HandleMotorActionForDance(uint8_t speed_percent) {
    HandleMotorActionForDanceFor(speed_percent, 0);
}

// weak 符号：web_server 收到 steer/throttle 时调用（C3 板实现差速转向）
//   steer:    -100..+100 (左/右；+100 原地右转)
//   throttle: -100..+100 (前/后；+100 前进满速，0 停车)
// 优先级高于 application.cc 的 4 象限控制
extern "C" void HandleBoardSteerControl(int steer, int throttle) {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    auto motor = board->motor_for_emotion();
    if (!motor) return;
    motor->Drive(steer, throttle);
}

// weak 符号：web_server 收到 {t:"interrupt"} 时调用（与板载 BOOT 键同语义）
//   空闲/待机 → 唤醒进入聆听；说话中 → 打断（AbortSpeaking）；聆听中 → 关闭回 idle
//   走 Application::ToggleChatState()，内部用 MAIN_EVENT_TOGGLE_CHAT 事件组
//   异步派发到 main 任务处理，不阻塞 httpd 任务（响应延迟 < 5ms）
extern "C" void HandleBoardToggleChat() {
    auto& app = Application::GetInstance();
    app.ToggleChatState();
}

// 弱符号：web_server 收到 action 按钮时调用
//   motor_action: 1=forward, 2=backward, 3=turn_left, 4=turn_right（motor_controller.h 枚举）
//   speed: 0..100
//   duration_ms: 0=无限（不自动停），>0=执行后自动停
//   用 motor->Execute 而不是 Application 的 HandleMotorActionForApplication，是因为
//   Execute 只用 motor 自己的 invert flag，不应用 Application 的 swap_lr/invert 配置
//   （那些配置是给 APP 内部 4 象限控制用的，会和 motor 的方向反着来，导致按钮左右颠倒）
extern "C" void HandleMotorExecuteByEnum(int motor_action, int speed, int duration_ms) {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    auto motor = board->motor_for_emotion();
    if (!motor) return;
    if (speed <= 0) speed = 80;
    if (speed > 100) speed = 100;
    if (duration_ms < 0) duration_ms = 0;
    motor->Execute(static_cast<MotorDir>(motor_action), (uint8_t)speed, (uint32_t)duration_ms);
}

// web_server / application 调：立即停车（不经过 4 象限 PWM 系统）
extern "C" void HandleMotorStop() {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    auto motor = board->motor_for_emotion();
    if (motor) motor->Stop();
}

// 弱符号：web_server 调用来修改 motor 自身的接线修正
//   改完即生效（motor 自身 invert/swap flag），同时通过 NVS 持久化（在 application.cc）
extern "C" void SetMotorInvertLeft(bool v) {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    auto motor = board->motor_for_emotion();
    if (motor) motor->SetInvertLeft(v);
}
extern "C" void SetMotorInvertRight(bool v) {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    auto motor = board->motor_for_emotion();
    if (motor) motor->SetInvertRight(v);
}
extern "C" void SetMotorSwapLr(bool v) {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    auto motor = board->motor_for_emotion();
    if (motor) motor->SetSwapLr(v);
}
extern "C" bool GetMotorInvertLeft() {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    auto motor = board->motor_for_emotion();
    return motor ? motor->GetInvertLeft() : false;
}
extern "C" bool GetMotorInvertRight() {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    auto motor = board->motor_for_emotion();
    return motor ? motor->GetInvertRight() : false;
}
extern "C" bool GetMotorSwapLr() {
    auto board = static_cast<CompactWifiBoard*>(&Board::GetInstance());
    auto motor = board->motor_for_emotion();
    return motor ? motor->GetSwapLr() : false;
}
