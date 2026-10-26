#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

// C3 精简版 OledDisplay —— 不依赖 LVGL 子模块
// - 用 esp_lcd_panel_draw_bitmap 直接写 1KB 1bpp framebuffer
// - 待机显示 NTP 时钟 + IP（参考 yyxc.ino 的 drawClock 布局）
// - 唤醒后显示 RoboEyes 风格眼睛（6 种情绪，静态图）
// - 50ms 定时器推一帧

#include "display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <cstdint>
#include <string>

// RoboEyes 前向声明（完整定义放在 oled_display.cc，头文件不必依赖那个 GPL 库）
class OledGfx;                          // RoboEyes 用的极简 Adafruit-GFX 兼容后端
template <typename T> class RoboEyes;   // FluxGarage RoboEyes 模板，前向声明即可持有指针

class OledDisplay : public Display {
private:
    friend class OledGfx;      // RoboEyes 的图形后端要能调这里的绘图函数

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    // 1bpp framebuffer: 128 * 64 / 8 = 1024 bytes
    static constexpr int kWidth  = 128;
    static constexpr int kHeight = 64;
    static constexpr int kFbSize = kWidth * kHeight / 8;

    // 顶部常驻状态栏高度（16 行）。
    //   这块 SSD1306 是**黄蓝双色屏**：最上面 16 行物理上就是黄色，下面 48 行蓝色。
    //   所以这块天生是"黄底黑字"的状态栏 —— 画 WiFi + 时钟，所有界面都保留，
    //   内容区（眼睛/时钟/文字）一律从 y = kStatusBarH 开始，不许侵入。
    static constexpr int kStatusBarH = 16;
    uint8_t fb_[kFbSize] = {0};

    SemaphoreHandle_t lock_ = nullptr;
    esp_timer_handle_t refresh_timer_ = nullptr;

    // 显示状态
    std::atomic<bool> dirty_{true};           // framebuffer 有改动需要推
    std::atomic<bool> animated_mode_{false};   // true=表情模式, false=时钟模式
    std::atomic<int>  emotion_idx_{0};         // 当前表情 0-5
    std::atomic<uint32_t> last_change_ms_{0};  // 上次状态改变时间
    std::atomic<bool> clock_synced_{false};    // NTP 同步后置 true
    std::atomic<bool> wifi_connected_{false};
    std::atomic<bool> music_mode_{false};      // true=播放音乐（整屏 ♫）
    std::atomic<bool> speaking_{false};        // true=TTS 正在说话（嘴跟着轻微开合）

    // ── "文字 / 表情"切换 ──
    //   对话时屏幕上显示什么：false=表情（眼睛），true=文字（助手回复内容）。
    //   ⚠️ 只作用于**对话中**（animated_mode_=true）；待机时一律走时钟界面。
    //   原来根本没有文字模式 —— SetChatMessage() 是直接把文字丢掉的
    //   （注释写着"C3 屏太小"），所以文字模式是这次新加的。
    std::atomic<bool> chat_text_mode_{false};
    char chat_text_[192] = {0};                // 助手回复（文字模式要显示的内容）

    // 电池电量百分比；**-1 = 没有电池检测硬件**，状态栏只画个空电池框。
    //   ⚠️ xmini-c3 这块板子上找不到任何 battery / ADC / vbat（板级目录里没有），
    //   所以默认 -1，不会假装显示电量。以后接了分压电路，板级调
    //   SetBatteryPercent() 填进来，状态栏自动就显示真实电量了。
    std::atomic<int> battery_pct_{-1};

    // ── 待机壁纸（长按 BOOT 循环切换，网页控制页也能直接选）──
    //   0 = 静态时钟（1 次/秒，最省电）
    //   1 = 动态星球（8fps，逐像素光照，最震撼也最耗电）
    //   2 = 星空      3 = 星际穿越    4 = 指针时钟    5 = 数字雨
    //   6 = 等离子    7 = 隧道        8 = 波浪        9 = 弹球
    //  10 = 生命游戏 11 = 立方体
    //  编号以 kWallpaperCount 为界，新增效果只需：加一个 Render*()、在
    //  RenderTick 里加分支、把名字补进 kWallpaperNames、并把计数 +1。
    std::atomic<int> wallpaper_{0};
    float planet_angle_ = 0.0f;   // 通用动画相位（自转 / 流动 / 演化）
    int64_t last_planet_us_ = 0;  // 上一帧时间戳 —— 动态壁纸靠它控帧率

    // 星际穿越壁纸（1）的星点：x,y 是相对中心的偏移，z 是深度（越小越近）
    struct Star { float x, y, z; };
    Star stars_[56] = {};
    bool stars_ready_ = false;

    // 生命游戏壁纸（5）的网格：内容区 128x48 → 64x24 个细胞，每细胞 2x2 像素
    static constexpr int kLifeW = 64;
    static constexpr int kLifeH = 24;
    uint8_t life_cells_[kLifeW * kLifeH] = {0};

    // 数字雨壁纸（4）：每列头部的位置（行单位，带小数）
    static constexpr int kRainCols = 21;   // 128 / 6 ≈ 21 列
    float rain_y_[21] = {0};
    bool  rain_ready_ = false;

    // 弹球壁纸（9）：位置 / 速度 + 环形拖尾缓冲
    static constexpr int kBallTrail = 14;
    float ball_x_ = 18.0f, ball_y_ = 32.0f;
    float ball_vx_ = 2.1f, ball_vy_ = 1.5f;
    int16_t ball_tx_[kBallTrail] = {0};
    int16_t ball_ty_[kBallTrail] = {0};
    int   ball_head_  = 0;
    bool  ball_ready_ = false;

    // 生命游戏壁纸（10）：life_cells_ 是当前代，life_next_ 是演算用的草稿。
    //   必须双缓冲 ——  Conway 规则要求"所有细胞同时更新"，就地改写会把
    //   左边刚算出来的新一代当成邻居喂给右边，图案立刻退化。
    uint8_t life_next_[kLifeW * kLifeH] = {0};
    bool     life_seeded_  = false;
    int      life_stale_   = 0;      // 连续多少代"没变化或太稀疏"
    uint32_t life_last_ms_ = 0;      // 上次演化时刻（演化 ~6 代/秒，绘制跟随刷新率）

    // 立方体壁纸（11）：绕 X / Y 轴的两个旋转角
    float cube_ax_ = 0.6f, cube_ay_ = 0.35f;

    char status_text_[32] = {0};              // 状态文字（表情下方一行）
    char song_name_[32] = {0};                // 歌名（音乐界面顶部显示）
    int  song_w_ = 0;                         // 歌名绘制宽度（SetSongName 时算好）

    // ── 整屏信息页（配网信息 / 激活验证码）──
    //   ⚠️ 这两个页面必须**抢占**壁纸、表情、音乐：
    //     配网和激活都是"设备卡住等你操作"的状态，此时还在放壁纸/眨眼睛，
    //     用户会以为机器坏了。visible 为真时 RenderTick 直接只画这一页。
    //   ⚠️ special_drawn_ 记录"上一帧画的是信息页"，用于退出信息页时
    //     强制全屏重画 —— 否则残留的文字会叠在壁纸上，变得一团糟。
    std::atomic<bool> cfg_info_visible_{false};   // 配网信息页
    std::atomic<bool> act_code_visible_{false};   // 激活验证码页（优先级更高）
    std::atomic<bool> special_drawn_{false};      // 上一帧是否为信息页
    char cfg_ssid_[40] = {0};                     // AP 名称（如 "Xiaozhi-XXXX"）
    char cfg_url_[40]  = {0};                     // 配网地址（已去掉 http:// 前缀）
    char act_code_[16] = {0};                     // 激活验证码（6 位数字）
    void RenderWifiConfigInfo();                  // 整屏配网信息
    void RenderActivationCode();                  // 整屏验证码（大号）
    int  MeasureUtf8String(const char* s);        // 中文/ASCII 混合字符串宽度（用于居中）

    // ── RoboEyes：直接复用 yyxc 那套 FluxGarage RoboEyes ──
    //   眼睛的大小/形状/情绪/动画 100% 照搬，本项目只补一个极简的图形后端。
    //   RoboEyes 全程只用到 4 个方法：clearDisplay / display / fillRoundRect / fillTriangle
    OledGfx* gfx_ = nullptr;
    RoboEyes<OledGfx>* eyes_ = nullptr;
    void InitRoboEyes();                               // 构造时创建 RoboEyes 实例
    void ApplyRoboEmotion(int idx);                    // emotion idx → mood / position / 动画

    // ── RoboEyes 动画状态机 ──
    int   gaze_x_      = 0;        // 视线水平偏移 -3..+3
    int   gaze_y_      = 0;        // 视线垂直偏移 -2..+2
    int   blink_phase_ = 0;        // 0=睁, 1=上半闭, 2=全闭, 3=下半闭
    int   blink_cnt_   = 0;        // 当前 blink_phase 剩余帧数
    int   next_blink_ms_ = 3000;   // 下次眨眼时间 (相对启动)
    uint32_t boot_ms_  = 0;        // 启动时间基准 (esp_timer_get_time()/1000)
    // 上次渲染的秒（0..59，-1=未渲染）。
    //   时钟界面**每秒**重绘一次：要有冒号闪烁 + 秒进度条的动效，就必须秒级刷新
    //   （原来是一次/分钟，屏幕是死的）。
    int      last_clock_sec_ = -1;

    // 6 个表情（每只眼睛 24x32 竖向圆角矩形，bit 排列横向 MSB-first 1bpp）
    // 表情索引: 0=neutral 1=happy 2=sad 3=thinking 4=listening 5=speaking
    // 当前所有情绪共用同一张位图
    // 眼睛已改为程序化绘制（DrawEyeShape），不再需要 6 张各自为政的眼型位图

    // ---------- 基础画图 ----------
    void Clear();
    void SetPixel(int x, int y, bool on);
    void DrawRect(int x, int y, int w, int h, bool on);
    void DrawFillRect(int x, int y, int w, int h, bool on);
    void DrawLine(int x0, int y0, int x1, int y1, bool on);
    void DrawHLine(int x, int y, int w, bool on);  // 横向水平线（快）
    void DrawBitmap(int x, int y, int w, int h, const uint8_t* data);
    void DrawChar6x8(int x, int y, char c, bool on = true);
    int  DrawString6x8(int x, int y, const char* s, bool on = true);
    void DrawChar3x5(int x, int y, char c, bool on = true);
    int  DrawString3x5(int x, int y, const char* s, bool on = true);
    // 缩放字体：scale=2 → 10x14 字 / cell 12x16；用于大号时间
    void DrawCharScaled(int x, int y, char c, int scale, bool on = true);
    int  DrawStringScaled(int x, int y, const char* s, int scale, bool on = true);

    // ---------- 界面 ----------
    void DrawStatusBar();    // 顶部 16px 常驻状态栏：WiFi + 时间 + 电量（所有界面都画）
    void RenderClock();      // 待机: 静态时钟（大号 HH:MM + IP）
    void RenderPlanet();     // 壁纸1: 光照星球 + 土星环 + 卫星 + 流星
    void RenderStarfield();  // 壁纸2: 星空（闪烁星点 + 偶尔流星）
    void RenderWarp();       // 壁纸3: 星际穿越（星点从中心辐射飞出）
    void RenderAnalog();     // 壁纸4: 模拟指针时钟（表盘 + 时分秒针）
    void RenderMatrixRain(); // 壁纸5: 数字雨（字符列向下流动）
    void RenderPlasma();     // 壁纸6: 等离子（Bayer 有序抖动出"灰度"）
    void RenderTunnel();     // 壁纸7: 3D 隧道（旋转方框由远及近飞出）
    void RenderWave();       // 壁纸8: 正弦干涉（三条异频正弦叠成水波）
    void RenderBounce();     // 壁纸9: 弹球（物理反弹 + 拖尾）
    void RenderLife();       // 壁纸10: 康威生命游戏（2x2 像素一个细胞）
    void RenderCube();       // 壁纸11: 3D 线框立方体（双轴旋转 + 透视）
    void SeedLife();         // 生命游戏：随机播种
    void StepLife();         // 生命游戏：演化一代（双缓冲）
    void RenderChatText();   // 文字模式: 内容区显示助手回复（自动换行）
    void RenderFace();       // 表情: 两只眼睛
    void DrawMouth(int cx, int y, int idx, uint32_t rel_ms, bool talking);
    int  DrawUtf8String(int x, int y, const char* s);         // 中文/ASCII 混合（14px 普惠字体）
    int  DrawUtf8Char(int x, int y, uint32_t uni);            // 单个字符（UTF-8 已解码）
    void DrawEllipseFill(int cx, int cy, int a, int b);       // 实心椭圆
    void DrawEyeShape(int cx, int cy, int w, int h);           // 眼睛：统一椭圆（形状固定）
    void DrawRoundRectFill(int x, int y, int w, int h, int r, bool on = true);  // 实心圆角矩形
    void DrawTriangleFill(int x0,int y0,int x1,int y1,int x2,int y2, bool on = true);  // 实心三角形（眼皮）
    void DrawMusicNote(int x, int y);                         // ♫ 双音符 48x40
    void PushFrame();        // framebuffer → SSD1306

    static void RefreshTimerCb(void* arg);

    // Display 基类
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

public:
    OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                int width, int height, bool mirror_x, bool mirror_y);
    ~OledDisplay();

    // 主动刷新（外部也可以调，比如 NTP 同步后）
    void ForceRefresh() { dirty_ = true; }

    // 通知（基类 Display 也有同名虚函数，这里加 override 并接收 synced 参数）
    void OnClockSynced(bool synced) override {
        clock_synced_ = synced;
        // 从未同步→已同步：强制立即渲染（不等下一秒）
        if (synced) last_clock_sec_ = -1;
        dirty_ = true;
    }
    void OnWifiState(bool connected) override {
        wifi_connected_ = connected;
        // 从断网→联网：强制立即渲染（不等下一秒）
        if (connected) last_clock_sec_ = -1;
        dirty_ = true;
    }

    // Display 基类接口
    virtual void SetStatus(const char* status) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetAnimatedEmotionMode(bool enable) override;
    virtual bool IsAnimatedEmotionMode() const override { return animated_mode_; }

    // 文字 / 表情切换（对话时屏幕上显示什么）：true=文字，false=表情
    void SetChatDisplayMode(bool text_mode);
    bool IsChatTextMode() const { return chat_text_mode_; }

    // 文字模式下要把对话内容要过来 —— 否则 application 那边认为"动画模式
    //   不显示文字"，压根不会下发 assistant 消息，屏幕上永远只有三个点。
    bool WantsChatText() const override { return chat_text_mode_; }

    // 电量 0..100；传 -1 = 没有电池检测（状态栏画空框）
    void SetBatteryPercent(int pct) { battery_pct_ = pct; dirty_ = true; }

    // 壁纸总数（新增效果时只改这一个常量；网页 / 长按循环 / 名称表都以它为准）
    static constexpr int kWallpaperCount = 12;

    // 壁纸中文名（越界返回 "?"，绝不返回野指针 —— 历史上 %s 解引用越界
    // 数组直接把设备打崩过）
    static const char* WallpaperName(int wp);

    // 待机壁纸：0..kWallpaperCount-1
    void SetWallpaper(int wp);
    int  NextWallpaper();                              // 循环切下一个，返回新编号
    int  GetWallpaper() const { return wallpaper_; }
    virtual void UpdateAnimatedEmotion() override;   // 实际是 timer 回调里推

    // 音乐模式：整屏居中大音符（服务端 AI 歌曲会带 "% play_music..." 标记）
    virtual void SetMusicMode(bool on) override;
    virtual void SetSongName(const char* name) override;  // 歌名（音乐界面顶部）
    virtual void SetSpeaking(bool on) override;           // 说话中 → 嘴跟着动
    void RenderMusic();                             // 整屏 ♫，替代表情/时钟
    void RenderTick();                              // 统一渲染入口（timer + 主循环）

    // ── 两个"整屏信息页"（优先级高于壁纸/表情/音乐）──
    //   配网信息：连哪个热点 + 打开哪个地址（参照 C3 小车那版移植过来）
    virtual void SetWifiConfigInfo(const char* ssid, const char* url) override;
    //   激活验证码：大号显示，用户照着输入到 APP / 控制台
    virtual void SetActivationCode(const char* code) override;
};

#endif // OLED_DISPLAY_H
