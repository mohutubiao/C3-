#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "settings.h"
#include "config.h"
#include "power_save_timer.h"
#include "power_manager.h"
#include "mcp_server.h"
#include <esp_netif.h>

#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#define TAG "XminiC3Board"

class XminiC3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    // OLED 强类型指针：屏幕初始化失败时 display_ 会退化成 NoDisplay，
    //   这时候不能再 static_cast 成 OledDisplay（那是另一套对象布局）。
    //   用这个指针判空就安全了。
    OledDisplay* oled_ = nullptr;
    Button boot_button_;
    PowerSaveTimer* power_save_timer_ = nullptr;
    PowerManager* power_manager_ = nullptr;

    void InitializePowerSaveTimer() {
        // ⚠️ 第一个参数 cpu_max_freq 必须是 **-1**，不能传 160。
        //   传 160 的话，power_save_timer.cc 入睡时会做三件事：
        //     EnableWakeWordDetection(false)  ← 关唤醒词
        //     codec->EnableInput(false)        ← 关麦克风输入
        //     esp_pm_configure(light_sleep)    ← 进轻睡眠
        //   结果就是"待机几分钟后叫不醒"——麦克风都关了，喊破喉咙也没反应。
        //   传 -1 则只回调 OnEnterSleepMode（这里只关屏幕），唤醒词一直在线。
        //   代价是不降 CPU 频率，但 OLED 才是耗电大头，关屏已省下大部分。
        power_save_timer_ = new PowerSaveTimer(-1, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // Print I2C bus info
        if (i2c_master_probe(codec_i2c_bus_, 0x18, 1000) != ESP_OK) {
            while (true) {
                ESP_LOGE(TAG, "Failed to probe I2C bus, please check if you have installed the correct firmware");
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        }
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(codec_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        oled_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = oled_;
        // 恢复上次选的待机壁纸（NVS；原来只在内存里，重启就回退到"时钟"）
        LoadWallpaper();
    }

    // ── 待机壁纸的 NVS 持久化 ──────────────────────────────────────────
    //   Settings 的析构函数会 nvs_commit()，所以临时对象写一次就够了。
    void SaveWallpaper(int wp) {
        Settings("display", true).SetInt("wallpaper", wp);
    }
    void LoadWallpaper() {
        if (!oled_) return;
        int wp = Settings("display", true).GetInt("wallpaper", 0);
        if (wp < 0 || wp >= OledDisplay::kWallpaperCount) wp = 0;
        oled_->SetWallpaper(wp);
        ESP_LOGI(TAG, "待机壁纸(恢复): %s", OledDisplay::WallpaperName(wp));
    }

    void InitializeButtons() {
        // BOOT 是这台设备唯一的按键，当前分工（2026-10-25 重排）：
        //   单击 = 开关对话（开机未连网时改为进配网）
        //   双击 = 对话时屏幕显示什么（文字 / 表情）
        //   长按 = 待机时屏幕显示什么（时钟 / 星球 / 星空 / …）
        // 已删除：三击切壁纸（难按，且历史上因名字数组越界崩过）、按住说话 PTT。
        boot_button_.OnClick([this]() {
            // ⚠️ 这个 WakeUp 原来挂在 OnPressDown 上，删掉对讲后那个回调没了，
            //   挪到单击里，保证"按一下先醒"还在 —— 否则省电态下首次按键可能没反应。
            if (power_save_timer_) power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        // 双击 BOOT：切换"文字 / 表情"（对话时屏幕上显示什么）
        //   切完立刻生效（SetChatDisplayMode 内部会置 dirty 重画）。
        //   目前只保存在内存，重启回到默认的表情模式。
        boot_button_.OnDoubleClick([this]() {
            if (power_save_timer_) power_save_timer_->WakeUp();
            if (oled_ == nullptr) return;
            bool next = !oled_->IsChatTextMode();
            oled_->SetChatDisplayMode(next);
            ESP_LOGI(TAG, "显示模式: %s", next ? "文字" : "表情");
        });
        // 长按 BOOT：切换**待机壁纸**（静态时钟 / 动态星球 / 星空 / …）
        //   ⚠️ 不是三击了 —— 三击难按，而且历史上因为名字数组越界崩过（见下）。
        //   ⚠️ 触发时机由 iot_button 的 BUTTON_LONG_PRESS_START 决定：
        //      阈值 = CONFIG_BUTTON_LONG_PRESS_TIME_MS = 1500ms
        //      （Button 构造只传了 GPIO，long_press_time=0 → 走 sdkconfig 默认值）
        //      按住到 1500ms 触发一次；松手**不会**再补发单击 —— 状态机直接进
        //      PRESS_LONG_PRESS_UP_CHECK，不经过 SINGLE_CLICK 那条分支，
        //      所以不会顺手把对话也开关一下。
        //   ⚠️ 名字统一走 OledDisplay::WallpaperName()（单点维护，越界返回 "?"）。
        //      历史教训：这里曾各写一份 kName[] 数组，只列了 3 个而
        //      NextWallpaper() 能返回到 5 —— kName[3] 越界读到数组后面的垃圾
        //      指针，ESP_LOGI 用 %s 解引用它 → 直接崩机。现在两份数组合并成
        //      一份（在 oled_display.cc），且 WallpaperName() 内部做下标检查。
        boot_button_.OnLongPress([this]() {
            if (power_save_timer_) power_save_timer_->WakeUp();
            if (oled_ == nullptr) return;
            const int wp = oled_->NextWallpaper();
            SaveWallpaper(wp);
            ESP_LOGI(TAG, "待机壁纸: %s", OledDisplay::WallpaperName(wp));
        });
    }

    void InitializeTools() {
        // 「查询 IP 地址」—— 想让设备**用语音**报出 IP，只能走这条路。
        //   ⚠️ 本机没有本地 TTS：所有语音都是服务端合成好再发回来的，设备端
        //      没法自己开口。做成 MCP 工具后，用户问"我的 IP 是多少"时大模型会
        //      调用它、拿到下面的字符串，然后自然会念出来 —— 这就是语音播报。
        //   （试过直接上报消息让服务端主动说，官方 server 不响应，行不通。）
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.get_ip",
            "查询设备当前的 IP 地址。当用户询问 IP 地址、网络地址、设备地址时调用。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                esp_netif_ip_info_t info;
                if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &info) == ESP_OK
                    && info.ip.addr != 0) {
                    char ip[16];
                    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
                    return std::string("设备当前的 IP 地址是 ") + ip + "。";
                }
                return std::string("设备当前没有连上 WiFi，暂时没有 IP 地址。");
            });
    }

    // 电池电量检测（ADC1 CH2 = GPIO2）。
    //   ⚠️ 参数来自原厂 WHA103 固件，不是猜的 —— 详见 power_manager.h 的说明。
    //   失败也不影响开机：GetBatteryLevel() 会返回 false，状态栏就不画电量。
    void InitializePowerManager() {
        power_manager_ = new PowerManager();
        power_manager_->Initialize();
    }

public:
    XminiC3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializePowerSaveTimer();
        InitializePowerManager();
        InitializeTools();

        // 避免使用错误的固件，把 EFUSE 操作放在最后
        // 把 ESP32C3 的 VDD SPI 引脚作为普通 GPIO 口使用
        esp_efuse_write_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO);
    }

    // 电量：接 PowerManager（ADC1 CH2 = GPIO2 读电池分压）。
    //   ⚠️ 基类默认是 return false（"没有电池"）—— 之前状态栏电量空白就是因为这个。
    //   检测到就返回真实百分比，检测不到返回 false，状态栏自动不画，不假装显示。
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!power_manager_) return false;
        return power_manager_->GetBatteryLevel(level, charging, discharging);
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

// ── 屏幕「文字 / 表情」模式：给 web_server 的弱符号落点 ──────────────────
//   web_server 是公共模块，不方便 include oled_display.h 依赖具体屏幕，
//   所以这里用 extern "C" 弱符号把能力暴露出去，网页 /api/display_mode 才能读写。
//   xmini-c3 用的就是 OledDisplay，直接 static_cast 即可。
extern "C" void SetChatDisplayMode(bool text_mode) {
    auto* oled = static_cast<OledDisplay*>(Board::GetInstance().GetDisplay());
    if (oled) oled->SetChatDisplayMode(text_mode);
}

extern "C" bool IsChatTextMode() {
    auto* oled = static_cast<OledDisplay*>(Board::GetInstance().GetDisplay());
    return oled ? oled->IsChatTextMode() : false;
}

// 待机壁纸（0..OledDisplay::kWallpaperCount-1）：给 web_server 的弱符号落点
//   ⚠️ 网页切的也要存 NVS，否则重启就回到"时钟"。
extern "C" void SetWallpaper(int wp) {
    auto* oled = static_cast<OledDisplay*>(Board::GetInstance().GetDisplay());
    if (!oled) return;
    oled->SetWallpaper(wp);
    Settings("display", true).SetInt("wallpaper", oled->GetWallpaper());
}

extern "C" int GetWallpaper() {
    auto* oled = static_cast<OledDisplay*>(Board::GetInstance().GetDisplay());
    return oled ? oled->GetWallpaper() : 0;
}

// 壁纸总数：网页用它做范围校验，避免以后加了新效果还要回来改 web_server.cc
extern "C" int GetWallpaperCount() {
    return OledDisplay::kWallpaperCount;
}

// 壁纸中文名：日志和网页共用一份（单点维护，越界返回 "?"）
extern "C" const char* GetWallpaperName(int wp) {
    return OledDisplay::WallpaperName(wp);
}

DECLARE_BOARD(XminiC3Board);
