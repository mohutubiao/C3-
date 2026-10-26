// C3 精简版 OledDisplay 实现 —— 不依赖 LVGL
// Framebuffer: 128*64 / 8 = 1024 字节
// 每个字节 8 个像素垂直方向（bit 0 = top, bit 7 = bottom）
// SSD1306 页面模式按页写，这里按列组织 1bpp 像素

#include "oled_display.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include "board.h"

// 借用工程里现成的普惠中文字库（14px / 1bpp）来画中文歌名。
// OLED 自带的 6x8 字体只有 ASCII，画不了汉字；这个字库本来就是 lvgl 显示用的、
// 已经编进固件了，这里引用它只是让链接器别把它裁掉，不额外增加 flash 占用。
#include "lvgl.h"
#include "font/lv_font_fmt_txt.h"   // 直接读字库数据表（不用 lvgl 运行时）

#define TAG "OledDisplay"

// 6x8 ASCII 字体（0x20~0x7E 完整可打印字符）
// 每个字符 6 字节，bit 1=亮
// 简化版：内嵌 96 字符表（参考 Roboto 6x8 公共域字体）
// 实际使用时，文件太大；这里用一个 6x8 工具子集（数字/字母/标点）
// 为了减少体积，我们用 5x7 字体 + 留 1px 间距的 6x8 网格

namespace {

// 6x8 基础 ASCII 字体子集（数字 0-9 + 大写 A-Z + 部分符号 + 小写 a-z）
// 数据按行排列：8 行 × 1 字节 = 8 字节/字符；这里为了紧凑用 6 字节/字符
// 标准 6x8 字体（5x7 + 1px 间距）
// 仅含我们用得到的字符
constexpr uint8_t kFont5x7[][5] = {
    // '0' (0x30)
    {0x3E, 0x51, 0x49, 0x45, 0x3E},
    // '1' (0x31)
    {0x00, 0x42, 0x7F, 0x40, 0x00},
    // '2' (0x32)
    {0x42, 0x61, 0x51, 0x49, 0x46},
    // '3' (0x33)
    {0x21, 0x41, 0x45, 0x4B, 0x31},
    // '4' (0x34)
    {0x18, 0x14, 0x12, 0x7F, 0x10},
    // '5' (0x35)
    {0x27, 0x45, 0x45, 0x45, 0x39},
    // '6' (0x36)
    {0x3C, 0x4A, 0x49, 0x49, 0x30},
    // '7' (0x37)
    {0x01, 0x71, 0x09, 0x05, 0x03},
    // '8' (0x38)
    {0x36, 0x49, 0x49, 0x49, 0x36},
    // '9' (0x39)
    {0x06, 0x49, 0x49, 0x29, 0x1E},
    // ':' (0x3A)
    {0x00, 0x36, 0x36, 0x00, 0x00},
    // '-' (0x2D)
    {0x08, 0x08, 0x08, 0x08, 0x08},
    // '.' (0x2E)
    {0x00, 0x00, 0x60, 0x60, 0x00},
    // ' ' (0x20)
    {0x00, 0x00, 0x00, 0x00, 0x00},
    // 'A' (0x41)
    {0x7E, 0x11, 0x11, 0x11, 0x7E},
    // 'B' (0x42)
    {0x7F, 0x49, 0x49, 0x49, 0x36},
    // 'C' (0x43)
    {0x3E, 0x41, 0x41, 0x41, 0x22},
    // 'D' (0x44)
    {0x7F, 0x41, 0x41, 0x22, 0x1C},
    // 'E' (0x45)
    {0x7F, 0x49, 0x49, 0x49, 0x41},
    // 'F' (0x46)
    {0x7F, 0x09, 0x09, 0x09, 0x01},
    // 'G' (0x47)
    {0x3E, 0x41, 0x49, 0x49, 0x7A},
    // 'H' (0x48)
    {0x7F, 0x08, 0x08, 0x08, 0x7F},
    // 'I' (0x49)
    {0x00, 0x41, 0x7F, 0x41, 0x00},
    // 'J' (0x4A)
    {0x20, 0x40, 0x41, 0x3F, 0x01},
    // 'K' (0x4B)
    {0x7F, 0x08, 0x14, 0x22, 0x41},
    // 'L' (0x4C)
    {0x7F, 0x40, 0x40, 0x40, 0x40},
    // 'M' (0x4D)
    {0x7F, 0x02, 0x04, 0x02, 0x7F},
    // 'N' (0x4E)
    {0x7F, 0x04, 0x08, 0x10, 0x7F},
    // 'O' (0x4F)
    {0x3E, 0x41, 0x41, 0x41, 0x3E},
    // 'P' (0x50)
    {0x7F, 0x09, 0x09, 0x09, 0x06},
    // 'Q' (0x51)
    {0x3E, 0x41, 0x51, 0x21, 0x5E},
    // 'R' (0x52)
    {0x7F, 0x09, 0x19, 0x29, 0x46},
    // 'S' (0x53)
    {0x46, 0x49, 0x49, 0x49, 0x31},
    // 'T' (0x54)
    {0x01, 0x01, 0x7F, 0x01, 0x01},
    // 'U' (0x55)
    {0x3F, 0x40, 0x40, 0x40, 0x3F},
    // 'V' (0x56)
    {0x1F, 0x20, 0x40, 0x20, 0x1F},
    // 'W' (0x57)
    {0x3F, 0x40, 0x38, 0x40, 0x3F},
    // 'X' (0x58)
    {0x63, 0x14, 0x08, 0x14, 0x63},
    // 'Y' (0x59)
    {0x07, 0x08, 0x70, 0x08, 0x07},
    // 'Z' (0x5A)
    {0x61, 0x51, 0x49, 0x45, 0x43},
    // 'a' (0x61)
    {0x20, 0x54, 0x54, 0x54, 0x78},
    // 'b' (0x62)
    {0x7F, 0x48, 0x44, 0x44, 0x38},
    // 'c' (0x63)
    {0x38, 0x44, 0x44, 0x44, 0x20},
    // 'd' (0x64)
    {0x38, 0x44, 0x44, 0x48, 0x7F},
    // 'e' (0x65)
    {0x38, 0x54, 0x54, 0x54, 0x18},
    // 'f' (0x66)
    {0x08, 0x7E, 0x09, 0x01, 0x02},
    // 'g' (0x67)
    {0x0C, 0x52, 0x52, 0x52, 0x3E},
    // 'h' (0x68)
    {0x7F, 0x08, 0x04, 0x04, 0x78},
    // 'i' (0x69)
    {0x00, 0x44, 0x7D, 0x40, 0x00},
    // 'j' (0x6A)
    {0x20, 0x40, 0x44, 0x3D, 0x00},
    // 'k' (0x6B)
    {0x7F, 0x10, 0x28, 0x44, 0x00},
    // 'l' (0x6C)
    {0x00, 0x41, 0x7F, 0x40, 0x00},
    // 'm' (0x6D)
    {0x7C, 0x04, 0x18, 0x04, 0x78},
    // 'n' (0x6E)
    {0x7C, 0x08, 0x04, 0x04, 0x78},
    // 'o' (0x6F)
    {0x38, 0x44, 0x44, 0x44, 0x38},
    // 'p' (0x70)
    {0x7C, 0x14, 0x14, 0x14, 0x08},
    // 'q' (0x71)
    {0x08, 0x14, 0x14, 0x18, 0x7C},
    // 'r' (0x72)
    {0x7C, 0x08, 0x04, 0x04, 0x08},
    // 's' (0x73)
    {0x48, 0x54, 0x54, 0x54, 0x20},
    // 't' (0x74)
    {0x04, 0x3F, 0x44, 0x40, 0x20},
    // 'u' (0x75)
    {0x3C, 0x40, 0x40, 0x20, 0x7C},
    // 'v' (0x76)
    {0x1C, 0x20, 0x40, 0x20, 0x1C},
    // 'w' (0x77)
    {0x3C, 0x40, 0x30, 0x40, 0x3C},
    // 'x' (0x78)
    {0x44, 0x28, 0x10, 0x28, 0x44},
    // 'y' (0x79)
    {0x0C, 0x50, 0x50, 0x50, 0x3C},
    // 'z' (0x7A)
    {0x44, 0x64, 0x54, 0x4C, 0x44},
};

// 字符 → 字体索引
constexpr int kFontIdxSpace = 13;
constexpr int kFontIdxDash  = 11;
constexpr int kFontIdxDot   = 12;
constexpr int kFontIdxColon = 10;

int FontIdxForChar(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 14 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 40 + (c - 'a');
    if (c == ':') return kFontIdxColon;
    if (c == '-') return kFontIdxDash;
    if (c == '.') return kFontIdxDot;
    return kFontIdxSpace;
}

} // namespace

// ── 新增壁纸用的两张小表 ──────────────────────────────────────────────
//  ⚠️ 单独开一个匿名 namespace：它们是"工具常量"，放在基础画图之前，
//     等离子 / 波浪 / 立方体都要用，但不能污染上面的字体 namespace。
namespace {

// 64 点正弦表，值域 0..63（= 31.5 + 31.5*sin）。
//   ⚠️ 为什么不用 sinf()：等离子一帧要算 128×48 = 6144 个像素、每像素 4 次
//      sin —— 单核 C3 上 24k 次 libm 调用直接把唤醒词检测饿死。查表把
//      每次 sin 变成一次数组读取，一帧成本降到几万条整数指令。
constexpr uint8_t kSin64[64] = {
    32,35,38,41,44,46,49,51,54,56,58,59,61,62,62,63,
    63,63,62,62,61,59,58,56,54,51,49,46,44,41,38,35,
    32,28,25,22,19,17,14,12, 9, 7, 5, 4, 2, 1, 1, 0,
     0, 0, 1, 1, 2, 4, 5, 7, 9,12,14,17,19,22,25,28
};

// 4×4 Bayer 有序抖动矩阵。
//   ⚠️ 单色屏每个像素只有亮/灭，等离子那种"连续灰度"必须用抖动来骗眼睛：
//      把像素值和这个位置的阈值比，大于阈值才点亮 —— 亮点的疏密就成了灰阶。
//      用有序（而非随机）矩阵是因为它形成的纹理是规则的、静止的，画面才不会满屏噪点。
constexpr uint8_t kBayer4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
};

} // namespace

// ----------------------- 基础画图 -----------------------

void OledDisplay::Clear() {
    memset(fb_, 0, kFbSize);
    dirty_ = true;
}

void OledDisplay::SetPixel(int x, int y, bool on) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    int byte = (y / 8) * kWidth + x;
    int bit  = y % 8;
    if (on) fb_[byte] |= (1 << bit);
    else    fb_[byte] &= ~(1 << bit);
}

void OledDisplay::DrawHLine(int x, int y, int w, bool on) {
    for (int i = 0; i < w; i++) SetPixel(x + i, y, on);
}

void OledDisplay::DrawRect(int x, int y, int w, int h, bool on) {
    DrawHLine(x, y, w, on);
    DrawHLine(x, y + h - 1, w, on);
    for (int i = 1; i < h - 1; i++) {
        SetPixel(x, y + i, on);
        SetPixel(x + w - 1, y + i, on);
    }
}

void OledDisplay::DrawFillRect(int x, int y, int w, int h, bool on) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            SetPixel(x + i, y + j, on);
}

void OledDisplay::DrawLine(int x0, int y0, int x1, int y1, bool on) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        SetPixel(x0, y0, on);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void OledDisplay::DrawBitmap(int x, int y, int w, int h, const uint8_t* data) {
    // 数据格式：行优先，每行 (w/8) 字节，每像素 1 bit，**MSB-first**（bit7 = 每字节最左像素）
    // ⚠️ 关键 bug 修复：原代码用 (data[byte] >> bit) & 1（LSB-first）读，
    //    而 kEye* 位图是 MSB-first 编码的 —— 每位字节内左右翻转，圆形被打散成
    //    "鱼/蝴蝶结"碎片。用 Python 解码验证：MSB-first 下 kEyeNeutral 是标准圆形，
    //    LSB-first 下就是屏幕上看到的碎片图案。
    const int bytes_per_row = w / 8;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int byte = j * bytes_per_row + (i / 8);
            int bit  = 7 - (i % 8);
            bool on = (data[byte] >> bit) & 1;
            SetPixel(x + i, y + j, on);
        }
    }
}

void OledDisplay::DrawChar6x8(int x, int y, char c, bool on) {
    int idx = FontIdxForChar(c);
    const uint8_t* glyph = kFont5x7[idx];
    // 5x7 字体画在 (x, y) 处，底部对齐 y+7
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if ((bits >> row) & 1) SetPixel(x + col, y + row, on);
        }
    }
}

int OledDisplay::DrawString6x8(int x, int y, const char* s, bool on) {
    int cx = x;
    while (*s) {
        DrawChar6x8(cx, y, *s, on);
        cx += 6;  // 5px 宽 + 1px 间距
        s++;
    }
    return cx - x;
}

// 缩放版 5x7 字体（每个像素 = scale x scale 块）
// scale=2 → 10x14 字，cell 12x16；scale=3 → 15x21 字，cell 18x24
// 用于待机时钟的大时间显示
void OledDisplay::DrawCharScaled(int x, int y, char c, int scale, bool on) {
    int idx = FontIdxForChar(c);
    const uint8_t* glyph = kFont5x7[idx];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if ((bits >> row) & 1) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        SetPixel(x + col * scale + dx, y + row * scale + dy, on);
                    }
                }
            }
        }
    }
}

int OledDisplay::DrawStringScaled(int x, int y, const char* s, int scale, bool on) {
    int cx = x;
    while (*s) {
        DrawCharScaled(cx, y, *s, scale, on);
        cx += 6 * scale;  // 5px 字 + 1px 间距，整体缩放
        s++;
    }
    return cx - x;
}

// 3x5 数字小字体（紧凑型，仅数字和冒号）
namespace {
const uint8_t kFont3x5_digits[10][3] = {
    {0x7, 0x5, 0x7},  // 0
    {0x2, 0x7, 0x2},  // 1
    {0x5, 0x7, 0x5},  // 2 (实际 0x5, 0x5, 0x5 = 全亮不好)
    {0x5, 0x7, 0x5},  // 3
    {0x7, 0x2, 0x7},  // 4
    {0x7, 0x5, 0x5},  // 5
    {0x7, 0x5, 0x5},  // 6
    {0x1, 0x1, 0x7},  // 7
    {0x7, 0x5, 0x7},  // 8
    {0x7, 0x5, 0x7},  // 9
};
const uint8_t kColon[3] = {0x0, 0x2, 0x0};
} // namespace

void OledDisplay::DrawChar3x5(int x, int y, char c, bool on) {
    const uint8_t* g = nullptr;
    if (c >= '0' && c <= '9') g = kFont3x5_digits[c - '0'];
    else if (c == ':')        g = kColon;
    if (!g) return;
    for (int col = 0; col < 3; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 3; row++) {  // 5 行但只用前 3 行
            if ((bits >> row) & 1) SetPixel(x + col, y + row, on);
        }
    }
}

int OledDisplay::DrawString3x5(int x, int y, const char* s, bool on) {
    int cx = x;
    while (*s) { DrawChar3x5(cx, y, *s, on); cx += 4; s++; }
    return cx - x;
}

// ----------------------- 推送 framebuffer -----------------------

void OledDisplay::PushFrame() {
    if (!panel_) return;
    // SSD1306 1bpp 模式：esp_lcd_panel_draw_bitmap 每字节 8 个垂直像素
    esp_lcd_panel_draw_bitmap(panel_, 0, 0, kWidth, kHeight, fb_);
    dirty_ = false;
}

bool OledDisplay::Lock(int timeout_ms) {
    if (!lock_) return true;
    return xSemaphoreTakeRecursive(lock_, timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void OledDisplay::Unlock() {
    if (lock_) xSemaphoreGiveRecursive(lock_);
}

// 统一的渲染入口：20Hz timer 和主循环(UpdateAnimatedEmotion)都必须走这里。
// ⚠️ 之前有两条渲染路径且逻辑不一致：主循环那条漏了 music_mode_ 分支，
//    直接用 animated_mode_ 判断 → 放音乐时主循环每 tick 把表情画回去，
//    和 timer 画的音符交替出现，看起来就是"隐约能看到表情"。
void OledDisplay::RenderTick() {
    // ── 整屏信息页：优先级高于一切（音乐 / 壁纸 / 表情）──
    //   "配网中"和"待激活"都是**设备停下来等你操作**的状态。这时候还在放星球、
    //   眨眼睛，用户会以为机器坏了，或者根本不知道该看哪里。
    //   ⚠️ 验证码优先于配网信息：已经连上网但还没激活时，用户要的是那串数字。
    if (act_code_visible_.load() || cfg_info_visible_.load()) {
        if (dirty_) {
            Clear();                    // 整屏重画：清掉壁纸/眼睛的残留像素
            if (act_code_visible_.load()) RenderActivationCode();
            else                          RenderWifiConfigInfo();
            PushFrame();                // 内部会把 dirty_ 清掉
        }
        special_drawn_ = true;          // 记住这一帧画的是信息页
        return;                         // 不画状态栏：整屏都是这一页
    }
    // 刚从信息页退出来 → 强制全屏重画一次，否则残留文字会叠在壁纸上
    if (special_drawn_.exchange(false)) dirty_ = true;

    if (music_mode_) {
        // 🎵 音乐：整屏大音符，替代表情/时钟（音乐结束自动恢复）
        RenderMusic();
    } else if (animated_mode_) {
        // 对话中：看用户选的是"文字"还是"表情"
        //   文字模式内容本身不变（不像眼睛要 20Hz 做眨眼动画），但状态栏的时间
        //   要跟着走，所以照样每帧重画 —— 就十几行像素，开销可以忽略。
        if (chat_text_mode_.load()) RenderChatText();
        else                        RenderFace();
    } else if (wallpaper_.load() == 1) {
        // ── 壁纸 1 = 动态星球：8fps ──
        //   ⚠️ 这是全场最重的画面（逐像素光照：每个像素要反算法线 + 逆旋转
        //      + 算漫反射，一帧上千次浮点运算），所以帧率给得最低：
        //      20fps → 12fps → 8fps 是实测调出来的，降到 8fps 后
        //      单核 C3 上才不会把唤醒词检测（AFE 音频任务）和 httpd 饿死。
        //      星球自转本来就慢，8fps 转起来依然连贯。
        const int64_t now = esp_timer_get_time();
        if (now - last_planet_us_ >= 125000) {
            last_planet_us_ = now;
            RenderPlanet();
        }
    } else if (wallpaper_.load() == 2) {
        // ── 壁纸 2 = 星空：同样 12fps（画面简单，但流星要连贯）──
        int64_t now = esp_timer_get_time();
        if (now - last_planet_us_ >= 80000) {
            last_planet_us_ = now;
            RenderStarfield();
        }
    } else if (wallpaper_.load() == 3) {
        // ── 壁纸 3 = 星际穿越：12fps ──
        //   星点从中心辐射飞出，靠**运动**造纵深 —— 在这块单色屏上比 3D 球体好看得多
        const int64_t now = esp_timer_get_time();
        if (now - last_planet_us_ >= 80000) {
            last_planet_us_ = now;
            RenderWarp();
        }
    } else if (wallpaper_.load() == 4) {
        // ── 壁纸 4 = 指针时钟：每秒刷一次就够（秒针一秒跳一格）──
        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_sec != last_clock_sec_) {
            last_clock_sec_ = tm.tm_sec;
            RenderAnalog();
        }
    } else if (wallpaper_.load() == 5) {
        // ── 壁纸 5 = 数字雨：10fps（字符较密，慢一点更耐看）──
        const int64_t now = esp_timer_get_time();
        if (now - last_planet_us_ >= 100000) {
            last_planet_us_ = now;
            RenderMatrixRain();
        }
    } else if (wallpaper_.load() == 6) {
        // ── 壁纸 6 = 等离子：10fps ──
        //   全场第二重的画面（6144 个像素逐个算 4 次查表 + Bayer 抖动），
        //   所以跟星球一样给最低帧率；查表已经把成本压到整数运算级。
        const int64_t now = esp_timer_get_time();
        if (now - last_planet_us_ >= 100000) {
            last_planet_us_ = now;
            RenderPlasma();
        }
    } else if (wallpaper_.load() == 7) {
        // ── 壁纸 7 = 隧道：12fps ──
        const int64_t now = esp_timer_get_time();
        if (now - last_planet_us_ >= 80000) {
            last_planet_us_ = now;
            RenderTunnel();
        }
    } else if (wallpaper_.load() == 8) {
        // ── 壁纸 8 = 波浪：12fps ──
        const int64_t now = esp_timer_get_time();
        if (now - last_planet_us_ >= 80000) {
            last_planet_us_ = now;
            RenderWave();
        }
    } else if (wallpaper_.load() == 9) {
        // ── 壁纸 9 = 弹球：~16fps（反弹要跟手，慢了拖尾会断成虚线）──
        const int64_t now = esp_timer_get_time();
        if (now - last_planet_us_ >= 60000) {
            last_planet_us_ = now;
            RenderBounce();
        }
    } else if (wallpaper_.load() == 10) {
        // ── 壁纸 10 = 生命游戏：绘制 12fps，演化在 RenderLife 内部再降到 ~6 代/秒 ──
        //   演化比绘制慢是故意的：一代要扫 64×24×8 个邻居，太快会白烧 CPU，
        //   而图案本身每秒变 6 次已经足够"活"。
        const int64_t now = esp_timer_get_time();
        if (now - last_planet_us_ >= 80000) {
            last_planet_us_ = now;
            RenderLife();
        }
    } else if (wallpaper_.load() == 11) {
        // ── 壁纸 11 = 立方体：~16fps（旋转要顺滑）──
        const int64_t now = esp_timer_get_time();
        if (now - last_planet_us_ >= 60000) {
            last_planet_us_ = now;
            RenderCube();
        }
    } else {
        // ── 待机主题 = 静态时钟：**每秒**重绘一次 ──
        //   秒级刷新是为了让冒号闪动（原来 1 次/分钟时屏幕完全是死的）。
        //   NTP 还没同步时 time=0 → sec=0，跟 last_clock_sec_=-1 比起来仍会触发一次
        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_sec != last_clock_sec_) {
            last_clock_sec_ = tm.tm_sec;
            RenderClock();
        }
    }

    // ── 状态栏常驻：画在最上层，时钟 / 表情 / 音乐界面都带 ──
    DrawStatusBar();

    if (dirty_) PushFrame();
}

void OledDisplay::RefreshTimerCb(void* arg) {
    auto* self = static_cast<OledDisplay*>(arg);
    if (!self->Lock(0)) return;
    self->RenderTick();
    self->Unlock();
}

// ----------------------- 顶部常驻状态栏 -----------------------
//
// ⚠️ 画在 y=0..15 是有原因的：这块 SSD1306 是**黄蓝双色屏**，顶部 16 行物理上
//   就是黄色（下面 48 行蓝色）。这块天生就是"黄底黑字"的状态栏 —— 不用画底色、
//   不用反显，视觉上还天然跟下面的内容分区。
// 内容：左边 WiFi 信号图标（3 格，没网画 ✕），右边电量。
//   ⚠️ 时间**不放在这里** —— 主区域有大号时钟（时间在时钟位置，状态栏只管状态）。
// 由 RenderTick() 在最后统一调用，所以时钟 / 表情 / 文字 / 音乐界面都会带。
void OledDisplay::DrawStatusBar() {
    // ⚠️ 先把整条状态栏清成黑底：这块必须完全干净。
    //   眼睛动画偶尔会顶到最上面几行，不垫底的话那些像素会透过状态栏露出来，
    //   看着就像"表情占了状态栏"。清完再画图标，双保险。
    DrawFillRect(0, 0, kWidth, kStatusBarH, false);

    // WiFi 图标：3 格由矮到高、底部对齐 y=11
    const int bx = 2;
    const int base = 11;
    if (wifi_connected_.load()) {
        DrawFillRect(bx,     base - 1, 2, 2, true);   // 第 1 格（最矮）
        DrawFillRect(bx + 3, base - 3, 2, 4, true);   // 第 2 格
        DrawFillRect(bx + 6, base - 5, 2, 6, true);   // 第 3 格（最高）
    } else {
        // 没网画 ✕：比"三个空格子"更能一眼看出是断网，而不是信号弱
        DrawLine(bx,     base - 5, bx + 7, base, true);
        DrawLine(bx + 7, base - 5, bx,     base, true);
    }

    // ── 显示模式（WiFi 右边）：对话时屏幕显示"文字"还是"动画表情"──
    //   文字模式画三横（像文本行），表情模式画两只小眼睛 + 嘴。
    //   放在状态栏是因为它跟设备走 —— 不管在哪个界面都能一眼看出当前是哪种，
    //   切换时也有个明确落点（用户要的"给个反馈"）。
    const int mi_x = 13;
    if (chat_text_mode_.load()) {
        DrawFillRect(mi_x, 5, 6, 1, true);          // 三横 = 文字
        DrawFillRect(mi_x, 7, 6, 1, true);
        DrawFillRect(mi_x, 9, 6, 1, true);
    } else {
        DrawFillRect(mi_x,     6, 2, 2, true);      // 左眼
        DrawFillRect(mi_x + 4, 6, 2, 2, true);      // 右眼
        DrawFillRect(mi_x + 1, 9, 4, 1, true);      // 嘴
    }

    // ── 中间：时间 HH:MM ──
    //   NTP 没同步时不画：免得显示假的 00:00 误导人
    if (clock_synced_.load()) {
        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        DrawString6x8((kWidth - 5 * 6) / 2, 4, buf, true);
    }

    // ── 右边：电量 ──
    //   ⚠️ 先问板级要真实电量（Board::GetBatteryLevel）。拿不到就**整个不画** ——
    //   这台 xmini-c3 没有电池检测电路（基类默认 return false），所以平时是空的；
    //   宁可空着，也不画个假百分比误导人。
    int  level = 0;
    bool charging = false, discharging = false;
    bool have_batt = Board::GetInstance().GetBatteryLevel(level, charging, discharging);
    if (!have_batt) {
        int ext = battery_pct_.load();           // 退路：外部显式设进来的百分比
        if (ext >= 0) { level = ext; have_batt = true; }
    }
    if (have_batt) {
        const int bat_w = 14, bat_h = 8;
        const int bat_x = kWidth - bat_w - 2;
        const int bat_y = 4;
        DrawRect(bat_x, bat_y, bat_w - 2, bat_h, true);                    // 外壳
        DrawFillRect(bat_x + bat_w - 2, bat_y + 2, 2, bat_h - 4, true);    // 正极头
        int pct = level;
        if (pct > 100) pct = 100;
        if (pct > 0) {
            int inner = (pct * (bat_w - 4)) / 100;
            if (inner > 0) DrawFillRect(bat_x + 2, bat_y + 2, inner, bat_h - 4, true);
        }
    }

    // 分隔线：黄色状态栏（0..15）和下面内容区的分界
    DrawHLine(0, 15, kWidth, true);
}

// ----------------------- 待机时钟 -----------------------

void OledDisplay::RenderClock() {
    Clear();

    // 开机/联网阶段：文字 2x 放大并居中（原来 6x8 小字挤在左上角，几乎看不清）
    if (!wifi_connected_) {
        const char* t = "WiFi...";
        int tw = (int)strlen(t) * 6 * 2;                  // 2x 缩放后的宽度
        DrawStringScaled((kWidth - tw) / 2, 24, t, 2, true);
        return;
    }

    if (!clock_synced_) {
        const char* t = "NTP...";
        int tw = (int)strlen(t) * 6 * 2;
        DrawStringScaled((kWidth - tw) / 2, 24, t, 2, true);
        return;
    }

    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);

    char buf[32];

    // ⚠️ 顶部 y=0..15 归状态栏（DrawStatusBar 画 WiFi + 电量），主区域从 16 起。
    //   用户明确不要日期，所以中间就是**大号时间**。

    // ── 大号 HH:MM（2x 缩放，每字符 12px 宽）y=22 ──
    //   冒号单独画：偶数秒亮、奇数秒灭，让待机画面"活"起来
    const bool colon_on = (tm.tm_sec % 2) == 0;
    char hh[4], mm[4];
    snprintf(hh, sizeof(hh), "%02d", tm.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", tm.tm_min);

    // 总宽 = HH(24) + ':'(12) + MM(24) = 60 → 居中起点 34
    const int cell = 6 * 2;
    const int x0 = (kWidth - cell * 5) / 2;
    DrawStringScaled(x0,           22, hh, 2, true);
    DrawStringScaled(x0 + cell * 2, 22, colon_on ? ":" : " ", 2, true);
    DrawStringScaled(x0 + cell * 3, 22, mm, 2, true);

    // IP 地址 — y=46
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip) == ESP_OK) {
        snprintf(buf, sizeof(buf), "IP " IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(buf, sizeof(buf), "IP ---");
    }
    // ⚠️ 秒进度条按用户要求去掉了，IP 顺势下移到 y=52 让画面更居中。
    int w = strlen(buf) * 6;
    DrawString6x8((kWidth - w) / 2, 52, buf, true);
}

// ----------------------- 待机：3D 旋转星球 -----------------------
//
// 参考 esp32_3d_engine 的思路做的**线框球体（wireframe sphere）**：
//   128x64 单色屏既没灰度也没颜色，用"经纬线网格 + 自转"表现立体感最合适。
//   只画正面（z>0 深度剔除），转起来看着就像一个实心球在自转。
// 开销：每帧约 150 个顶点、几十条短线，160MHz 的 C3 算这点量绰绰有余；
//   真正费电的是 20fps 的整屏 I2C 传输 —— 所以做成主题可切换（planet_mode_）。
void OledDisplay::RenderPlanet() {
    Clear();

    const float r  = 19.0f;                          // 星球半径
    const int   cx = kWidth / 2;
    const int   cy = (kStatusBarH + kHeight) / 2;    // 内容区垂直居中
    const float PI2 = 6.2831853f;

    const float ay = planet_angle_;                  // 绕 Y 轴自转
    const float ax = 0.45f;                          // 倾角（这样能看见极点）

    const float sa = sinf(ax), ca = cosf(ax);
    const float sy = sinf(ay), cyr = cosf(ay);

    // ── 1. 背景星空 ──
    //   位置固定、按相位轮流闪。要是每帧随机取点，看着就是满屏雪花乱跳，很廉价。
    static const uint8_t kStars[][2] = {
        {  6, 21}, { 17, 18}, { 31, 23}, { 46, 19}, { 93, 20},
        {105, 18}, {119, 22}, { 11, 33}, {123, 32}, { 21, 56},
        { 43, 59}, { 97, 58}, {113, 51}, {  7, 45}, {117, 43},
    };
    const int kStarN = (int)(sizeof(kStars) / sizeof(kStars[0]));
    const int phase  = (int)(planet_angle_ * 5.0f) % 3;
    for (int i = 0; i < kStarN; ++i) {
        if ((i % 3) != phase) DrawFillRect(kStars[i][0], kStars[i][1], 1, 1, true);
    }

    // 被星球本体挡住：在球背面(z<0) 且 落在球的投影圆内
    auto hidden = [&](int x, int y, float z) {
        if (z > 0.0f) return false;
        const int dx = x - cx, dy = y - cy;
        return (dx * dx + dy * dy) < (int)(r * r);
    };

    // ── 2. 星环（土星环）──
    //   内外两圈 + 中间撒碎块，让它有厚度感而不是一根细线。
    //   ⚠️ 分前后两趟画，这是"环"的层次感所在：
    //     后半段先画（会被星球盖掉一半），前半段**最后**画（压在星球前面）。
    const float ring_in  = r * 1.45f;
    const float ring_out = r * 1.95f;
    const int   ring_seg = 72;

    auto ring_pt = [&](float ang, float rr, int& ox, int& oy, float& oz) {
        const float x0 = rr * cosf(ang);
        const float z0 = rr * sinf(ang);
        const float y1 = -z0 * sa;                   // 环平面绕 X 倾斜 → 投影成椭圆
        const float z1 =  z0 * ca;
        const float x2 =  x0 * cyr + z1 * sy;        // 跟着视角绕 Y
        const float z2 = -x0 * sy  + z1 * cyr;
        ox = cx + (int)(x2 + 0.5f);
        oy = cy + (int)(y1 + 0.5f);
        oz = z2;
    };

    auto draw_ring_edge = [&](float rr, bool front) {
        for (int i = 0; i < ring_seg; ++i) {
            const float a1 = (float)i * PI2 / ring_seg;
            const float a2 = (float)(i + 1) * PI2 / ring_seg;
            int x1, y1, x2, y2; float z1, z2;
            ring_pt(a1, rr, x1, y1, z1);
            ring_pt(a2, rr, x2, y2, z2);
            if ((z1 > 0.0f) != front) continue;                   // 只要这一半
            if (hidden(x1, y1, z1) || hidden(x2, y2, z2)) continue;
            DrawLine(x1, y1, x2, y2, true);
        }
    };
    auto draw_ring_dust = [&](bool front) {
        for (int i = 0; i < ring_seg; i += 2) {
            int x, y; float z;
            ring_pt((float)i * PI2 / ring_seg,
                    ring_in + (ring_out - ring_in) * ((i % 4) ? 0.35f : 0.7f), x, y, z);
            if ((z > 0.0f) != front) continue;
            if (hidden(x, y, z)) continue;
            DrawFillRect(x, y, 1, 1, true);
        }
    };

    // 环的后半段（先画，等下会被星球盖掉一部分）
    draw_ring_edge(ring_in,  false);
    draw_ring_edge(ring_out, false);
    draw_ring_dust(false);

    // ── 3. 星球本体：实心圆盘 + 挖出大陆（矢量画法）──
    //   ⚠️ 上一版是**逐像素光照**（每个像素反算法线 + 漫反射），实测两个致命问题：
    //     ① 128x64 单色屏二值化后全是噪点，看着很脏（用户反馈"丑得要命"）
    //     ② 每帧上千次浮点运算把 CPU 占满，唤醒词检测抢不到 CPU →
    //        "星球状态下喊不醒、按键也迟钝"，这是**功能性问题**，不是观感问题
    //   改成矢量画法：先填实心圆盘，再用黑色"挖"出大陆和纬线。
    //   图形干净锐利，计算量只有原来的十分之一，CPU 大量余量还给音频。

    // 4x4 Bayer 有序抖动矩阵：把 0..1 的亮度转成**规则网点**
    //   ⚠️ 为什么不用固定阈值二值化（前两版"丑得要命"的元凶）：
    //     固定阈值会让明暗边界变成一条生硬的硬边，边界附近的像素非黑即白且逐帧
    //     跳变，看着就是一层脏噪点。有序抖动产生的是**规则排列的点阵**
    //     （跟报纸上的半色调照片一个原理），眼睛会自动积分成灰度，
    //     干净、有实体感，而且图案固定、每帧一致，不会闪。
    static const int kBayer[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5}
    };

    // 大陆中心（世界坐标单位向量）—— 球面上的"纹理"，跟着自转一起转
    static const float kLand[][3] = {
        {-0.35f,  0.30f,  0.89f},
        { 0.80f,  0.15f,  0.58f},
        { 0.10f, -0.60f, -0.79f},
        {-0.75f, -0.30f,  0.59f},
        { 0.30f,  0.85f, -0.43f},
    };

    // 逐像素光照 + 有序抖动
    //   海洋底色暗（0.40），陆地亮（1.00），再乘上漫反射 —— 背光面自然全黑，
    //   亮面就显出黑白分明的大陆轮廓，转起来能看见大陆在球面上移动。
    const int   ir = (int)r;
    const float lx = -0.50f, ly = -0.55f, lz = 0.67f;   // 光源方向（世界空间，固定）
    for (int dy = -ir; dy <= ir; ++dy) {
        for (int dx = -ir; dx <= ir; ++dx) {
            const float nx = (float)dx / r;
            const float ny = (float)dy / r;
            const float d2 = nx * nx + ny * ny;
            if (d2 > 1.0f) continue;                            // 球外
            const float nz = sqrtf(1.0f - d2);                    // 正面半球

            // 逆变换：视角法线 → 世界法线（先逆 X 倾斜，再逆 Y 自转）
            const float y1 = ny * ca + nz * sa;
            const float z1 = -ny * sa + nz * ca;
            const float wx = nx * cyr - z1 * sy;
            const float wz = nx * sy  + z1 * cyr;
            const float wy = y1;

            float diff = wx * lx + wy * ly + wz * lz;
            if (diff < 0.0f) diff = 0.0f;

            float base = 0.40f;                                 // 海洋（暗）
            for (int i = 0; i < 5; ++i) {
                if (wx * kLand[i][0] + wy * kLand[i][1] + wz * kLand[i][2] > 0.90f) {
                    base = 1.0f;                                  // 陆地（亮）
                    break;
                }
            }

            const float lum   = base * (0.16f + 0.84f * diff);
            const int   bayer = kBayer[(cy + dy) & 3][(cx + dx) & 3];
            if ((int)(lum * 16.0f) > bayer) {
                DrawFillRect(cx + dx, cy + dy, 1, 1, true);
            }
        }
    }

    // 球的**亮面轮廓**：只描被光照到的那半圈，球体边缘立刻清晰了，
    //   而且背光那一侧不画线 —— 正好是"晨昏线"的观感。
    {
        const float lx2 = 0.0f, ly2 = 0.0f;   // 屏幕上灯的方位（左上偏）
        for (int i = 0; i < 64; ++i) {
            const float a  = (float)i * 6.2831853f / 64.0f;
            const float ex = cosf(a), ey = sinf(a);
            if (ex * (lx2 - 0.6f) + ey * (ly2 - 0.6f) > -0.15f) continue;   // 只画向光侧
            DrawFillRect(cx + (int)(r * ex + 0.5f),
                         cy + (int)(r * ey + 0.5f), 1, 1, true);
        }
    }

    // 环的前半段（最后画 → 压在星球前面，这才是环该有的层次）
    draw_ring_edge(ring_in,  true);
    draw_ring_edge(ring_out, true);
    draw_ring_dust(true);

    // ── 4. 卫星：绕着转的小亮点，转速比星球快，画面才"活" ──
    {
        const float sat_a = planet_angle_ * -2.2f;
        const float sat_r = r * 1.75f;
        const float x0 = sat_r * cosf(sat_a);
        const float z0 = sat_r * sinf(sat_a);
        const float y1 = -z0 * sa;
        const float z1 =  z0 * ca;
        const float x2 =  x0 * cyr + z1 * sy;
        const float z2 = -x0 * sy  + z1 * cyr;
        const int sx = cx + (int)(x2 + 0.5f);
        const int sy = cy + (int)(y1 + 0.5f);
        // 转到星球背后就藏起来（被挡住），转到前面才画出来
        const int ddx = sx - cx, ddy = sy - cy;
        if (z2 > 0.0f || (ddx * ddx + ddy * ddy) > (int)(r * r)) {
            DrawFillRect(sx - 1, sy - 1, 2, 2, true);
        }
    }

    // ── 5. 流星：每隔几秒划过一道，静止的画面才有惊喜 ──
    {
        const float mp = fmodf(planet_angle_ * 1.4f, 6.0f) / 6.0f;   // 0..1 循环
        if (mp < 0.15f) {                                             // 只在 15% 周期出现
            const float t = mp / 0.15f;                               // 划过进度 0..1
            const int mx = (int)(26.0f + t * 78.0f);
            const int my = (int)(24.0f + t * 22.0f);
            for (int k = 0; k < 9; ++k) {                             // 拖尾
                DrawFillRect(mx - k, my - k / 3, 1, 1, true);
            }
        }
    }

    // 自转推进
    planet_angle_ += 0.05f;
    if (planet_angle_ > PI2) planet_angle_ -= PI2;
}

// ── 待机壁纸切换（长按 BOOT / 网页都走这里）──
//   名称表：索引 = 壁纸编号。⚠️ 新增效果必须在这里补名字，否则
//   WallpaperName() 会返回 "?"（不会崩，但日志/网页上显示不出来）。
static const char* const kWallpaperNames[] = {
    "时钟", "星球", "星空", "星际穿越", "指针时钟", "数字雨",
    "等离子", "隧道", "波浪", "弹球", "生命游戏", "立方体"
};

const char* OledDisplay::WallpaperName(int wp) {
    const int n = (int)(sizeof(kWallpaperNames) / sizeof(kWallpaperNames[0]));
    // 双保险：编译期常量 kWallpaperCount 与名称表长度不一致时按短的走，
    // 宁可少一个也不能越界（历史上这里越界解引用野指针直接崩过机）。
    static_assert(kWallpaperCount <= 64, "wallpaper count out of range");
    if (wp < 0 || wp >= n || wp >= kWallpaperCount) return "?";
    return kWallpaperNames[wp];
}

void OledDisplay::SetWallpaper(int wp) {
    if (wp < 0) wp = 0;
    if (wp >= kWallpaperCount) wp = kWallpaperCount - 1;
    wallpaper_ = wp;
    last_clock_sec_ = -1;    // 切回静态时钟时立刻重画，不用等下一秒
    last_planet_us_ = 0;    // 动态壁纸：下一帧立刻画，不用等间隔
    dirty_ = true;
}

int OledDisplay::NextWallpaper() {
    int next = wallpaper_.load() + 1;
    if (next >= kWallpaperCount) next = 0;
    SetWallpaper(next);
    return next;
}

// ── 壁纸 3：星际穿越 ──
//   3D 星场：星点从画面中心向外辐射飞出，越近越大（透视）。
//   ⚠️ 这是**最适合这块屏**的效果 —— 单色 128x64 表现"立体"很吃力（前面两版
//   球体的教训），但表现**运动和纵深**非常擅长：星点靠位置变化就能造出速度感，
//   计算量只有每帧几十次乘除，完全不影响唤醒词检测。
void OledDisplay::RenderWarp() {
    Clear();

    const int cx = kWidth / 2;
    const int cy = (kStatusBarH + kHeight) / 2;
    const float PI2   = 6.2831853f;
    const float fov   = 55.0f;
    const float depth = 130.0f;

    // 首次进来撒一把星：用确定性伪随机（避免每次 rand 的开销，也不闪）
    if (!stars_ready_) {
        for (int i = 0; i < 56; ++i) {
            stars_[i].x = (float)((i * 37) % 171) - 85.0f;
            stars_[i].y = (float)((i * 53) %  97) - 48.0f;
            stars_[i].z = 1.0f + (float)((i * 29) % 100) * (depth - 1.0f) / 100.0f;
        }
        stars_ready_ = true;
    }

    for (int i = 0; i < 56; ++i) {
        float z = stars_[i].z - 2.0f;                      // 朝观察者推进
        if (z <= 1.0f) {                                    // 飞出画面 → 放回最远处
            z = depth;
            stars_[i].x = (float)(((i * 37) + (int)(planet_angle_ * 50)) % 171) - 85.0f;
            stars_[i].y = (float)(((i * 53) + (int)(planet_angle_ * 30)) %  97) - 48.0f;
        }
        stars_[i].z = z;

        const float k = fov / z;                          // 透视：远的密集、近的散开
        const int sx = cx + (int)(stars_[i].x * k);
        const int sy = cy + (int)(stars_[i].y * k);
        if (sx < 0 || sx >= kWidth || sy < kStatusBarH || sy >= kHeight) continue;

        // 近的星点画 2x2，远的是 1x1 —— 单色屏靠大小差做纵深
        if (z < 45.0f) DrawFillRect(sx, sy, 2, 2, true);
        else           DrawFillRect(sx, sy, 1, 1, true);
    }

    planet_angle_ += 0.05f;
    if (planet_angle_ > PI2) planet_angle_ -= PI2;
}

// ── 壁纸 2：星空 ──
//   比星球省得多（没有逐像素光照），但同样有动感：星点按相位闪 + 偶尔流星。
//   底部保留 IP —— 不然待机时想找设备反而找不到，那就本末倒置了。
void OledDisplay::RenderStarfield() {
    Clear();

    const float PI2 = 6.2831853f;

    static const uint8_t kStars[][2] = {
        {  8, 22}, { 20, 19}, { 34, 26}, { 48, 21}, { 62, 18},
        { 76, 22}, { 90, 19}, {104, 24}, {118, 20}, { 14, 34},
        { 30, 38}, { 52, 33}, { 70, 40}, { 88, 35}, {106, 39},
        {120, 33}, { 18, 48}, { 40, 52}, { 60, 47}, { 82, 54},
        {100, 48}, {114, 52}, { 26, 58}, { 68, 59}, { 96, 57},
    };
    const int kStarN = (int)(sizeof(kStars) / sizeof(kStars[0]));
    const int phase  = (int)(planet_angle_ * 4.0f) % 3;
    for (int i = 0; i < kStarN; ++i) {
        if ((i % 3) != phase) DrawFillRect(kStars[i][0], kStars[i][1], 1, 1, true);
    }

    esp_netif_ip_info_t ip;
    char buf[24];
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip) == ESP_OK) {
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(buf, sizeof(buf), "---");
    }
    int w = (int)strlen(buf) * 6;
    DrawString6x8((kWidth - w) / 2, 56, buf, true);

    // 流星
    const float mp = fmodf(planet_angle_ * 1.4f, 6.0f) / 6.0f;
    if (mp < 0.15f) {
        const float t = mp / 0.15f;
        const int mx = (int)(26.0f + t * 78.0f);
        const int my = (int)(22.0f + t * 22.0f);
        for (int k = 0; k < 9; ++k) {
            DrawFillRect(mx - k, my - k / 3, 1, 1, true);
        }
    }

    planet_angle_ += 0.05f;
    if (planet_angle_ > PI2) planet_angle_ -= PI2;
}

// ── 壁纸 4：模拟指针时钟 ──
//   经典表盘：外圈 + 12 个刻度 + 时针/分针/秒针。
//   ⚠️ 秒针每秒跳一格，所以 1 次/秒刷新就够 —— 是**动态壁纸里最省电的**。
void OledDisplay::RenderAnalog() {
    Clear();

    const int cx = kWidth / 2;
    const int cy = (kStatusBarH + kHeight) / 2;
    const int r  = 21;
    const float PI2 = 6.2831853f;

    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    // 外圈：中点圆算法，一次画 8 个对称点
    {
        int x = r, y = 0, err = 1 - r;
        while (x >= y) {
            DrawFillRect(cx + x, cy + y, 1, 1, true);
            DrawFillRect(cx + y, cy + x, 1, 1, true);
        DrawFillRect(cx - y, cy + x, 1, 1, true);
            DrawFillRect(cx - x, cy + y, 1, 1, true);
            DrawFillRect(cx - x, cy - y, 1, 1, true);
            DrawFillRect(cx - y, cy - x, 1, 1, true);
            DrawFillRect(cx + y, cy - x, 1, 1, true);
            DrawFillRect(cx + x, cy - y, 1, 1, true);
            y++;
            if (err < 0) err += 2 * y + 1;
            else { x--; err += 2 * (y - x) + 1; }
        }
    }

    // 12 个刻度：整点长一点，其余短一点
    for (int i = 0; i < 12; ++i) {
        const float a = (float)i * PI2 / 12.0f;
        const int   r0 = ((i % 3) == 0) ? (r - 5) : (r - 3);
        const int   r1 = r - 2;
        DrawLine(cx + (int)(r0 * sinf(a) + 0.5f), cy - (int)(r0 * cosf(a) + 0.5f),
                cx + (int)(r1 * sinf(a) + 0.5f), cy - (int)(r1 * cosf(a) + 0.5f), true);
    }

    // 三根指针。屏幕 y 轴朝下，所以 y 取 -cos，让 0 点指向正上方
    auto hand = [&](float a, int len) {
        DrawLine(cx, cy,
                 cx + (int)(len * sinf(a) + 0.5f),
                 cy - (int)(len * cosf(a) + 0.5f), true);
    };
    const float sec_a = (float)tm_now.tm_sec * PI2 / 60.0f;
    const float min_a = ((float)tm_now.tm_min + tm_now.tm_sec / 60.0f) * PI2 / 60.0f;
    const float hr_a  = ((float)(tm_now.tm_hour % 12) + tm_now.tm_min / 60.0f) * PI2 / 12.0f;
    hand(hr_a,  r - 10);      // 时针（最短）
    hand(min_a, r - 5);        // 分针
    hand(sec_a, r - 2);        // 秒针（最长最细）

    DrawFillRect(cx - 1, cy - 1, 2, 2, true);   // 轴心

    // ⚠️ 日期拆到左右两边，不再居中放在正下方。
    //   表盘圆心 y=40、半径 21 → 圆占 y=19..61，而 6x8 字模画在 y=56 意味着
    //   字符实际覆盖 y=56..63，正好压住表盘下半圈和 6 点方向的刻度。
    //   分开贴两侧后，圆的底部完全露出来，整只表才像个表。
    static const char* kWday[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};

    char dbuf[16];                                   // 左：月-日
    snprintf(dbuf, sizeof(dbuf), "%02d-%02d", tm_now.tm_mon + 1, tm_now.tm_mday);
    DrawString6x8(2, 56, dbuf, true);

    const char* wd = kWday[tm_now.tm_wday];          // 右：星期
    DrawString6x8(kWidth - 2 - (int)strlen(wd) * 6, 56, wd, true);
}

// ── 壁纸 5：数字雨（黑客帝国）──
//   字符列向下流动，头部亮、尾部逐渐稀疏 —— 单色屏没法真淡出，用"跳着画"来模拟。
//   只画 6 行字符（内容区 48px），开销很小。
void OledDisplay::RenderMatrixRain() {
    Clear();

    static const char kGlyphs[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ#$%&*+=<>?@";
    const int kGlyphN = (int)(sizeof(kGlyphs) - 1);
    const int col_w = 6, row_h = 8;
    const int cols = kRainCols;
    const int rows = (kHeight - kStatusBarH) / row_h;   // 6 行

    if (!rain_ready_) {
        for (int c = 0; c < cols; ++c) {
            rain_y_[c] = (float)((c * 7) % (rows + 6)) - 3.0f;
        }
        rain_ready_ = true;
    }

    const int head_phase = (int)(planet_angle_ * 12.0f);
    for (int c = 0; c < cols; ++c) {
        rain_y_[c] += 0.22f;
        if (rain_y_[c] >= rows + 5) rain_y_[c] = -4.0f;

        for (int k = 0; k < 5; ++k) {                 // 头部 + 4 格尾迹
            const int y = (int)rain_y_[c] - k;
            if (y < 0 || y >= rows) continue;
            // 尾巴越往后越容易跳过 → 看起来像淡出
            if (k > 1 && ((y + c + head_phase) % (k + 1)) != 0) continue;
            const char ch = kGlyphs[(c * 5 + y * 3 + head_phase) % kGlyphN];
            char s[2] = { ch, 0 };
            DrawString6x8(c * col_w, kStatusBarH + y * row_h, s, true);
        }
    }

    planet_angle_ += 0.05f;
    if (planet_angle_ > 6.2831853f) planet_angle_ -= 6.2831853f;
}

// ── 壁纸 6：等离子（Plasma）──
//   参考 GitHub 上的经典 SSD1306 等离子 demo（FWeinb/ESP_Plasma，AdySan 原作）：
//   四个不同频率、不同方向的正弦叠加出流动云雾，再用 **Bayer 有序抖动**把
//   连续灰度压成 1bit —— 这是单色屏上唯一能表现"渐变"的手段。
//   ⚠️ 相对参考实现的关键改动：原版每像素调 sin()（在 ESP8266 上都卡），
//   这里全部换成 kSin64 查表 + 纯整数运算，才能在单核 C3 上和唤醒词检测共存。
void OledDisplay::RenderPlasma() {
    Clear();

    const int cx0 = kWidth / 2;                   // 64
    const int cy0 = (kStatusBarH + kHeight) / 2;  // 40
    const int base = (int)(planet_angle_ * 26.0f);
    // 四个相位用不同速度推进 → 四层花纹互相"打架"，看不出周期
    const int p1 = base & 63;
    const int p2 = (base * 3) & 63;
    const int p3 = (base / 2) & 63;
    const int p4 = (63 - ((base * 2) & 63)) & 63;

    for (int y = kStatusBarH; y < kHeight; ++y) {
        const int dy = y - cy0;
        const int rowv = kSin64[((y * 3) + p2) & 63];   // 只随 y 变化 → 提到行外算一次
        for (int x = 0; x < kWidth; ++x) {
            const int dx = x - cx0;
            // 横向行波 + 纵向行波 + 斜向行波 + 环形波纹（平方距离 &63 天然形成同心圆）
            const int v = kSin64[((x >> 1) + p1) & 63]
                        + rowv
                        + kSin64[(((x + y) / 3) + p3) & 63]
                        + kSin64[(((dx * dx + dy * dy) >> 4) + p4) & 63];
            // v ∈ 0..252，阈值展开到 8..248：亮点疏密 = 灰阶
            if (v > ((int)kBayer4[y & 3][x & 3] * 16 + 8)) SetPixel(x, y, true);
        }
    }

    planet_angle_ += 0.05f;
    if (planet_angle_ > 6.2831853f) planet_angle_ -= 6.2831853f;
}

// ── 壁纸 7：3D 隧道 ──
//   一排旋转的方框由远及近飞出画面 —— 参考 dsanborn16/esp32-oled-animations
//   的 Tunnel。⚠️ 这里没有逐像素算 atan2/透视（那在 C3 上太贵），而是直接
//   画 8 个"旋转矩形"：深度 z 决定大小、角度随 z 递增，视觉上就是螺旋隧道。
//   一帧只有 32 条直线，比等离子还省。
void OledDisplay::RenderTunnel() {
    Clear();

    const int cx = kWidth / 2;
    const int cy = (kStatusBarH + kHeight) / 2;   // 40
    const float frac = fmodf(planet_angle_ * 0.9f, 1.0f);
    const float spin = planet_angle_ * 0.55f;

    for (int i = 0; i < 8; ++i) {
        const float z = (float)i + 1.4f - frac;    // 由远(z 大)及近(z 小)
        if (z < 0.79f) continue;                   // 已经飞过镜头
        // 58 是标定出来的：46 时最大的一圈只占 70×28（画面正中一小坨，
        // 完全不像"在隧道里飞"），改 58 后最外圈刚好撑满内容区再被裁掉。
        const float r = 58.0f / z;                 // 透视：近大远小
        const float a = spin + z * 0.30f;          // 越远转得越开 → 螺旋感

        int px[5], py[5];
        for (int k = 0; k < 4; ++k) {
            const float ang = a + k * 1.5707963f;
            // ⚠️ 横向拉宽、纵向压扁：内容区是 128×48（约 2.7:1），方框必须跟着
            //   变形，否则看起来是口"竖井"而不是隧道。
            px[k] = cx + (int)(r * cosf(ang) * 1.75f + 0.5f);
            py[k] = cy + (int)(r * sinf(ang) * 0.62f + 0.5f);
        }
        px[4] = px[0];
        py[4] = py[0];
        // 越近的框画得越粗 —— 单色屏没有灰度，靠"线宽"也能造纵深
        const bool near = (z < 3.0f);
        for (int k = 0; k < 4; ++k) {
            DrawLine(px[k], py[k], px[k + 1], py[k + 1], true);
            if (near) DrawLine(px[k] + 1, py[k] + 1, px[k + 1] + 1, py[k + 1] + 1, true);
        }
    }

    planet_angle_ += 0.05f;
    if (planet_angle_ > 6.2831853f) planet_angle_ -= 6.2831853f;
}

// ── 壁纸 8：正弦干涉（Wave Pattern）──
//   三条频率 / 相位 / 幅度都不同的正弦横向铺开，互相干涉出"水面波纹"。
//   参考 dsanborn16/esp32-oled-animations 的 Wave Pattern。
//   ⚠️ 单色屏画不出灰度，所以不铺满屏画场强，只画曲线本身 —— 更干净也更省。
void OledDisplay::RenderWave() {
    Clear();

    const int cy   = (kStatusBarH + kHeight) / 2;   // 40
    const int base = (int)(planet_angle_ * 22.0f);

    for (int w = 0; w < 3; ++w) {
        const int amp  = 15 - w * 4;         // 15 / 11 / 7
        const int ph   = base * (1 + w) + w * 17;
        const int yoff = (w - 1) * 7;        // -7 / 0 / +7：三条错开，别糊成一团
        int prev = -1;
        for (int x = 0; x < kWidth; ++x) {
            const int idx = ((x * (2 + w)) / 3 + ph) & 63;
            const int y   = cy + ((kSin64[idx] - 32) * amp) / 32 + yoff;
            // 相邻列落差 > 1 时补竖线，否则陡峭处会断成虚线点阵
            if (prev >= 0 && (y > prev + 1 || y < prev - 1)) {
                const int a = (y < prev) ? y : prev;
                const int b = (y < prev) ? prev : y;
                for (int yy = a; yy <= b; ++yy) SetPixel(x, yy, true);
            } else {
                SetPixel(x, y, true);
            }
            prev = y;
        }
    }

    planet_angle_ += 0.05f;
    if (planet_angle_ > 6.2831853f) planet_angle_ -= 6.2831853f;
}

// ── 壁纸 9：弹球（Bouncing Ball）──
//   经典屏保：球在内容区里物理反弹，拖着一条逐渐变细的尾巴。
//   参考 dsanborn16/esp32-oled-animations 的 Bouncing Ball（它带重力 + 阻尼，
//   这里去掉重力改成匀速反弹 —— 有重力的话球很快贴底不动，当壁纸看会"死掉"）。
void OledDisplay::RenderBounce() {
    Clear();

    const int left = 3, right = kWidth - 4;
    const int top  = kStatusBarH + 3, bot = kHeight - 4;

    if (!ball_ready_) {
        ball_x_ = 20.0f;  ball_y_ = 34.0f;
        ball_vx_ = 2.1f;  ball_vy_ = 1.5f;
        ball_head_ = 0;
        for (int i = 0; i < kBallTrail; ++i) { ball_tx_[i] = -1; ball_ty_[i] = -1; }
        ball_ready_ = true;
    }

    ball_x_ += ball_vx_;
    ball_y_ += ball_vy_;
    if (ball_x_ < left)  { ball_x_ = left;  ball_vx_ =  fabsf(ball_vx_); }
    if (ball_x_ > right) { ball_x_ = right; ball_vx_ = -fabsf(ball_vx_); }
    if (ball_y_ < top)   { ball_y_ = top;   ball_vy_ =  fabsf(ball_vy_); }
    if (ball_y_ > bot)   { ball_y_ = bot;   ball_vy_ = -fabsf(ball_vy_); }

    ball_head_ = (ball_head_ + 1) % kBallTrail;
    ball_tx_[ball_head_] = (int16_t)ball_x_;
    ball_ty_[ball_head_] = (int16_t)ball_y_;

    // 从最老的一帧画到最新的一帧：越老的点越小，形成"流星尾"
    for (int i = 0; i < kBallTrail; ++i) {
        const int j = (ball_head_ + 1 + i) % kBallTrail;
        const int tx = ball_tx_[j], ty = ball_ty_[j];
        if (tx < 0) continue;
        const int r = (i < 4) ? 1 : ((i < 9) ? 2 : 3);
        DrawFillRect(tx - r + 1, ty - r + 1, r * 2 - 1, r * 2 - 1, true);
    }

    // 球本体：用三段矩形拼一个近似的实心圆（比逐像素判距离省得多）。
    //   ⚠️ 直径给到 9px（原来是 7）：128×48 的屏上 7px 的球只剩 2% 覆盖率，
    //   远看就是一个小点，撑不起整屏壁纸。
    const int bx = (int)ball_x_, by = (int)ball_y_;
    DrawFillRect(bx - 3, by - 3, 7, 7, true);
    DrawFillRect(bx - 4, by - 2, 9, 5, true);
    DrawFillRect(bx - 2, by - 4, 5, 9, true);
}

// ── 壁纸 10：康威生命游戏 ──
//   内容区 128×48 划成 64×24 个细胞，每细胞 2×2 像素。
//   ⚠️ 演化必须用双缓冲：Conway 规则要求"所有细胞同时更新"，就地改写会把左边
//      刚算出的新一代当成邻居喂给右边，图案瞬间退化成一条斜线。
//   ⚠️ 边界取环绕（环面拓扑），否则贴边的图案会被"墙"切死，很快全灭。
void OledDisplay::SeedLife() {
    for (int i = 0; i < kLifeW * kLifeH; ++i) {
        life_cells_[i] = ((esp_random() & 7) < 3) ? 1 : 0;   // ~37% 初始密度
    }
    life_seeded_ = true;
    life_stale_  = 0;
}

void OledDisplay::StepLife() {
    const int N = kLifeW * kLifeH;
    int pop = 0, diff = 0;

    for (int gy = 0; gy < kLifeH; ++gy) {
        const int y0 = (gy == 0) ? (kLifeH - 1) : (gy - 1);
        const int y1 = (gy == kLifeH - 1) ? 0 : (gy + 1);
        const int r0 = y0 * kLifeW, rm = gy * kLifeW, r1 = y1 * kLifeW;
        for (int gx = 0; gx < kLifeW; ++gx) {
            const int x0 = (gx == 0) ? (kLifeW - 1) : (gx - 1);
            const int x1 = (gx == kLifeW - 1) ? 0 : (gx + 1);
            const int n = life_cells_[r0 + x0] + life_cells_[r0 + gx] + life_cells_[r0 + x1]
                        + life_cells_[rm + x0]                        + life_cells_[rm + x1]
                        + life_cells_[r1 + x0] + life_cells_[r1 + gx] + life_cells_[r1 + x1];
            const int cur = life_cells_[rm + gx];
            const int nx  = cur ? ((n == 2 || n == 3) ? 1 : 0) : ((n == 3) ? 1 : 0);
            life_next_[rm + gx] = (uint8_t)nx;
        }
    }

    for (int i = 0; i < N; ++i) {
        if (life_cells_[i] != life_next_[i]) diff++;
        pop += life_next_[i];
    }
    memcpy(life_cells_, life_next_, N);

    // 图案"死"了就重新播种：要么完全静止（diff==0，全是静物/空盘），
    //   要么只剩零星几点（pop 太小，屏幕上几乎空白）。
    //   连续 3 代判定成立才重播，避免把正常的振荡子误杀。
    if (diff == 0 || pop < 25) {
        if (++life_stale_ >= 3) SeedLife();
    } else {
        life_stale_ = 0;
    }
}

void OledDisplay::RenderLife() {
    Clear();

    if (!life_seeded_) SeedLife();

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (now - life_last_ms_ >= 160) {      // ~6 代/秒：一代要扫 64×24×8 个邻居
        life_last_ms_ = now;
        StepLife();
    }

    for (int gy = 0; gy < kLifeH; ++gy) {
        const int py = kStatusBarH + gy * 2;
        const int row = gy * kLifeW;
        for (int gx = 0; gx < kLifeW; ++gx) {
            if (life_cells_[row + gx]) DrawFillRect(gx * 2, py, 2, 2, true);
        }
    }
}

// ── 壁纸 11：3D 线框立方体 ──
//   8 个顶点绕 X/Y 双轴旋转、透视投影后连 12 条棱。
//   ⚠️ 只画棱不画面（单色屏没有明暗，实心面只会糊成一坨），顶点加粗 2×2
//      让"骨架"更清楚 —— 这是这类 demo 在 128×64 上唯一能看的形态。
void OledDisplay::RenderCube() {
    Clear();

    const int cx = kWidth / 2;
    const int cy = (kStatusBarH + kHeight) / 2;   // 40

    cube_ax_ += 0.031f;                 // 绕 X
    cube_ay_ += 0.017f;                 // 绕 Y（两轴速度不同 → 轨迹不重复）
    if (cube_ax_ > 6.2831853f) cube_ax_ -= 6.2831853f;
    if (cube_ay_ > 6.2831853f) cube_ay_ -= 6.2831853f;

    const float sa = sinf(cube_ax_), ca = cosf(cube_ax_);
    const float sb = sinf(cube_ay_), cb = cosf(cube_ay_);

    static const float kV[8][3] = {
        {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}
    };
    static const int kE[12][2] = {
        {0,1},{1,2},{2,3},{3,0},        // 后面
        {4,5},{5,6},{6,7},{7,4},        // 前面
        {0,4},{1,5},{2,6},{3,7}         // 四条连接棱
    };

    int px[8], py[8];
    for (int i = 0; i < 8; ++i) {
        const float x = kV[i][0], y = kV[i][1], z = kV[i][2];
        const float y1 = y * ca - z * sa;         // 绕 X 轴
        const float z1 = y * sa + z * ca;
        const float x2 = x * cb + z1 * sb;        // 绕 Y 轴
        const float z2 = -x * sb + z1 * cb;
        // 视距 3.4 保证分母恒正（|z2| ≤ 1.74），不会除零也不会翻面。
        //   ⚠️ 2.0 / 1.0 这两个系数是数值标定出来的：遍历整圈旋转取
        //   max|x2*k|≈21.3、max|y1*k|≈21.2，乘上系数后最大外扩约 43×21，
        //   即 85×42 —— 刚好撑满 128×48 的内容区又不出血。
        //   原来用 1.5 / 0.62 只有 64×26，在这么宽的屏上小得像个图标。
        const float k  = 36.0f / (3.4f + z2);
        px[i] = cx + (int)(x2 * k * 2.00f + 0.5f);
        py[i] = cy + (int)(y1 * k * 1.00f + 0.5f);
    }

    for (int i = 0; i < 12; ++i) {
        DrawLine(px[kE[i][0]], py[kE[i][0]], px[kE[i][1]], py[kE[i][1]], true);
    }
    for (int i = 0; i < 8; ++i) {
        DrawFillRect(px[i] - 1, py[i] - 1, 2, 2, true);
    }
}

// ----------------------- 文字模式 -----------------------
//
// 对话时把助手的回复直接显示在屏幕上（替代眼睛表情）—— 这是"文字/表情切换"
// 里文字那一半。原来 SetChatMessage() 是把文字直接丢掉的，屏幕上看不到任何
// 对话内容，所以这个模式是这次新加的。
//
// 布局：顶部 16px 归状态栏，内容区只剩 y=16..63 共 48px
//   → 14px 普惠字体 + 2px 行距 = 16px 一行 → 正好排 3 行
void OledDisplay::RenderChatText() {
    Clear();

    const int line_h = 16;                 // 14px 字高 + 2px 行距
    const int top    = kStatusBarH + 1;    // 让开顶部状态栏
    const int left   = 2;
    const int right  = kWidth - 2;

    if (chat_text_[0] == '\0') {
        // 还没有对话内容：给个提示，免得整屏空白以为死机了
        const char* hint = "...";
        int w = (int)strlen(hint) * 6;
        DrawString6x8((kWidth - w) / 2, top + 10, hint, true);
        return;
    }

    int x = left;
    int y = top;
    const uint8_t* p = (const uint8_t*)chat_text_;

    while (*p) {
        // 解出一个 UTF-8 字符
        uint32_t uni = 0;
        int len = 1;
        if      ((*p & 0x80) == 0) { uni = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { uni = ((uint32_t)(p[0] & 0x1F) <<  6) | (p[1] & 0x3F); len = 2; }
        else if ((*p & 0xF0) == 0xE0) { uni = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
        else if ((*p & 0xF8) == 0xF0) { uni = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); len = 4; }
        else { p++; continue; }                       // 非法字节，跳过

        if (uni == '\n') {                            // 文本里自带的换行
            x = left;
            y += line_h;
            p += len;
            if (y > kHeight - 14) break;
            continue;
        }

        // 预估字宽判断要不要换行（ASCII 半角约 7px，CJK 全角约 14px）
        const int est = (uni < 0x2E80) ? 7 : 14;
        if (x + est > right) {                        // 这行放不下了
            x = left;
            y += line_h;
            if (y > kHeight - 14) break;              // 3 行都满了 → 截断
        }

        int adv = DrawUtf8Char(x, y, uni);
        x += (adv > 0) ? adv : est;
        p += len;

        if (y > kHeight - 14) break;
    }
}

// ----------------------- 表情 -----------------------

// ══════════════════════════════════════════════════════════════════════════════
//  RoboEyes 适配层 —— 眼睛直接复用 yyxc 那套 FluxGarage RoboEyes
//
//  那个库全程只用到 4 个绘图方法 + millis()/random() 两个 Arduino 运行时函数，
//  所以补齐这两样和一个极简后端，就能原封不动用它的眼睛：
//  大小(28x28) / 形状(圆角矩形) / 情绪(mood + position + 各种动画) 跟 yyxc 完全一致。
//
//  ⚠️ 这两个宏必须留到本文件末尾：RoboEyes 是模板，成员函数在被调用处才实例化，
//     提前 #undef 会编译失败。esp_random() 不受影响（标识符不同）。
//  ⚠️ random(n) 里必须判 0：库里会对可能为 0 的变化量取模，%0 在 ESP32 上直接异常
//
//  头文件用项目自带的 vendored 版 third_party/roboeyes/ —— display/roboeyes_adapter.cc
//  （给 LVGL 用的）用的就是这一份，还比 Arduino 库那版新（多了 LOVING/SURPRISED/SLEEPY）。
//  ⚠️ FluxGarage RoboEyes 是 GPL-3.0。
//  ⚠️ 它的成员初始化顺序有问题（spaceBetweenDefault 声明在使用它的 eyeLxDefault 之后）
//     → 构造时是 UB，所以 main/CMakeLists.txt 里对 oled_display.cc 也加了
//     -Wno-maybe-uninitialized -Wno-reorder，跟官方对 adapter 的处理一致。
//     UB 造成的"默认坐标算错"由 InitRoboEyes() 手动重设坐标覆盖掉。
// ══════════════════════════════════════════════════════════════════════════════
typedef uint8_t byte;
#define millis()   ((unsigned long)(esp_timer_get_time() / 1000ULL))
#define random(n)  ((unsigned long)((n) ? (esp_random() % (unsigned long)(n)) : 0UL))
#include "third_party/roboeyes/src/FluxGarage_RoboEyes.h"

// RoboEyes 的图形后端。清屏/推屏由 RenderTick 统一管（本项目只有一条渲染入口），
// 所以 clearDisplay / display 故意留空
class OledGfx {
public:
    OledGfx(OledDisplay* d) : disp_(d) {}
    void clearDisplay() {}
    void display()      {}
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
        disp_->DrawRoundRectFill(x, y, w, h, r, color != 0);
    }
    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color) {
        disp_->DrawTriangleFill(x0, y0, x1, y1, x2, y2, color != 0);
    }
private:
    OledDisplay* disp_;
};

void OledDisplay::InitRoboEyes() {
    delete eyes_;  eyes_ = nullptr;
    delete gfx_;   gfx_  = nullptr;
    gfx_  = new OledGfx(this);
    eyes_ = new RoboEyes<OledGfx>(*gfx_);
    // 128x40 + 50fps。⚠️ 高度故意传 40 而不是 64：
    //   RoboEyes 的 screenHeight 只用来算 idle 随机张望的范围
    //   （getScreenConstraint_Y = screenHeight - 眼睛高），传 64 会让眼睛跑到
    //   y=28 直接压到嘴上；传 40 就把它的活动范围锁死在上半屏。
    eyes_->begin(128, 40, 50);
    eyes_->setWidth(26, 26);
    // ⚠️ 眼睛高度从默认的 36 收到 20：顶部 16px 让给了常驻状态栏（kStatusBarH），
    //   内容区只剩 y=16..63 共 48 行。保持 36 高的话眼睛会一路伸到 y=52，直接压在嘴上。
    //   20 高 + 顶部 y=20 → 眼睛占 20..40，嘴 44..54，状态文字 56 起，都排得开。
    //   （上一版 24 高 / y=18，实测眼睛还是会顶到状态栏下沿，所以再缩再下移。）
    eyes_->setHeight(20, 20);
    // ⚠️ begin() 和 setWidth()/setHeight() 都**不重算**默认坐标：那些默认值在
    //    构造函数里已经按初始的 36x36 算死了。不手动摆位置会出两个问题：
    //      ① 改宽度后整体偏左   ② 垂直按 64 行居中(y=14)，会盖住嘴
    //    所以这里显式摆正：宽 26 + 间距 10 → 居中起点 x=33（右眼 33+26+10=69）；
    //    顶部 y=20（原来是 4；现在要让开状态栏的 0..15，再多留 4px 余量）
    eyes_->eyeLxDefault = eyes_->eyeLx = eyes_->eyeLxNext = 33;
    eyes_->eyeLyDefault = eyes_->eyeLy = eyes_->eyeLyNext = kStatusBarH + 6;
    eyes_->eyeRxDefault = eyes_->eyeRx = eyes_->eyeRxNext = 69;   // 33 + 26 + 10
    eyes_->eyeRyDefault = eyes_->eyeRy = eyes_->eyeRyNext = kStatusBarH + 6;
    eyes_->setAutoblinker(ON);
    eyes_->setMood(DEFAULT);
    ApplyRoboEmotion(emotion_idx_.load());
}

// emotion idx → RoboEyes 的 mood / 视线 / 动画
// ⚠️ 每个分支都全量重设所有开关（照 yyxc 的 drawFace 写法）：
//    否则上一个情绪的 flicker / curiosity / sweat 会残留到下一个情绪上
void OledDisplay::ApplyRoboEmotion(int idx) {
    if (!eyes_) return;
    RoboEyes<OledGfx>* e = eyes_;
    e->setWidth(26, 26);
    e->setSweat(false);
    e->setCuriosity(false);
    e->setHFlicker(false, 0);
    e->setVFlicker(false, 0);
    // ⚠️ idle 默认一律关闭：idle 会每隔几秒把眼睛目标位置设成
    //    random(getScreenConstraint_X()) = random(0..62)，也就是眼睛自己跑到屏幕
    //    左边或右边"乱瞟"——用户反馈"默认眼睛向右看"就是它干的。
    //    用户要的默认状态是「在中间、看着你」，所以这里统一关掉。
    //    以后想要"发呆乱看"的效果，再单独对某个情绪 setIdleMode(true, ...)。
    e->setIdleMode(false);
    switch (idx) {
        case 1:  // happy：笑眼 + 笑动画
            e->setMood(HAPPY);    e->setPosition(DEFAULT); e->setIdleMode(false);
            e->anim_laugh();      break;
        case 2:  // sad：眼皮下垂 + 往下看
            e->setMood(TIRED);    e->setPosition(S);       e->setIdleMode(false); break;
        case 3:  // thinking：往上看 + 好奇（外侧那只眼睛放大）
            e->setMood(DEFAULT);  e->setPosition(N);       e->setCuriosity(true); e->setIdleMode(false); break;
        case 4:  // listening：正视中间 + 外侧眼睛微微放大（专注听你说话，而不是看别处）
            e->setMood(DEFAULT);  e->setPosition(DEFAULT); e->setCuriosity(true); e->setIdleMode(false); break;
        case 5:  // speaking：轻微垂直抖动（像在讲话）
            e->setMood(DEFAULT);  e->setPosition(DEFAULT); e->setVFlicker(true, 3); e->setIdleMode(false); break;
        case 6:  // surprised：眼睛放大到 34 + 好奇
            e->setMood(DEFAULT);  e->setPosition(DEFAULT); e->setCuriosity(true);
            e->setWidth(32, 32);
            e->eyeLxNext = 25;    // 变宽后要重算居中：setPosition() 是按"当前"宽度算的，
                                  // 此时宽度还是 28 → 不加这行会偏右 6px
            break;
        case 7:  // angry：怒眉 + 横向抖动
            e->setMood(ANGRY);    e->setPosition(DEFAULT); e->setHFlicker(true, 2); e->setIdleMode(false); break;
        default: // neutral：默认圆眼，正视中间（不开 idle，见上方说明）
            e->setMood(DEFAULT);  e->setPosition(DEFAULT); e->setIdleMode(false); break;
    }
}

void OledDisplay::RenderFace() {
    Clear();

    int idx = emotion_idx_.load();
    if (idx < 0 || idx >= 8) idx = 0;      // 情绪从 6 种扩展到 8 种（新增 惊讶 / 生气）

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (boot_ms_ == 0) boot_ms_ = now_ms;
    uint32_t rel = now_ms - boot_ms_;

    // ── 眼睛：整套复用 yyxc 的 FluxGarage RoboEyes ──
    //   眨眼 / idle 随机张望 / 情绪动画全部在库里自己跑，这里什么都不用管，
    //   大小形状情绪表达和 yyxc 完全一致（begin/setWidth/setMood... 照搬）
    if (!eyes_) InitRoboEyes();
    if (eyes_) {
        // ⚠️ 眼睛不许钻到状态栏底下：setPosition(N)（"向上看"）和 setVFlicker
        //   都会把 eyeLy / eyeRy 往 y=0 方向推，不夹住就会顶进顶部那 16px 状态栏。
        // ⚠️ 除了当前位置 eyeLy/eyeRy，还必须夹**目标位置** eyeLyNext/eyeRyNext：
        //   RoboEyes 在 update() 里会把 eyeLy 往 eyeLyNext 挪（"向上看"就是设一个
        //   很高的目标值），挪完再画 —— 只夹当前值的话照样顶进状态栏。
        if (eyes_->eyeLy     < kStatusBarH) eyes_->eyeLy     = kStatusBarH;
        if (eyes_->eyeRy     < kStatusBarH) eyes_->eyeRy     = kStatusBarH;
        if (eyes_->eyeLyNext < kStatusBarH) eyes_->eyeLyNext = kStatusBarH;
        if (eyes_->eyeRyNext < kStatusBarH) eyes_->eyeRyNext = kStatusBarH;
        eyes_->update();
    }

    // ── 嘴：还是我们自己的实心小方块 ──
    //   RoboEyes 本身不带嘴（只有眼睛+眼皮+汗珠），所以嘴这块自己画，
    //   照用户给的参考图来的形状 + 说话时轻微开合
    //   ⚠️ y 从 47 改到 49：眼睛下移后占 18..42，嘴要让开眼睛底边
    //      （嘴说话时最高 11px → 44..54，下面状态文字 56 起，互不打架）
    DrawMouth(kWidth / 2, 49, idx, rel, speaking_.load());

    // 状态文字（底部一行，6x8 字符）
    if (status_text_[0]) {
        int w = strlen(status_text_) * 6;
        DrawString6x8((kWidth - w) / 2, 56, status_text_, true);
    }
}

// 嘴：按情绪索引画不同形状，让表情真正有区别
//   idx: 0 neutral 1 happy 2 sad 3 thinking 4 listening 5 speaking
// 嘴：所有情绪共用同一个「实心圆角矩形」—— 照用户给的参考图定形状。
//   ⚠️ 之前的 U 形笑弧 / ∩ 垂嘴 / 波浪线 / 圆圈 / 椭圆全部删除：
//      用户明确要求「所有的嘴都用这个，其他的删除，情绪用眼睛表达」。
//   只有说话时做轻微开合（否则说话时整张脸是静止的），形状本身不变。
void OledDisplay::DrawMouth(int cx, int y, int idx, uint32_t rel_ms, bool talking) {
    (void)idx;                        // 情绪不再影响嘴形
    int mw = 11;                      // 嘴宽：接近正方形的比例（参考图）
    int mh = 9;                       // 嘴高
    if (talking) {
        // 说话：小幅开合（周期约 330ms）——张开时变高变窄，像真的在张嘴
        //   ⚠️ 高度上限 11：嘴顶不要碰到眼睛底(40) —— y=47 时最大只到 42..52
        static const int kH[3] = {6, 9, 11};
        static const int kW[3] = {13, 11, 10};
        int t = (int)((rel_ms / 110) % 3);
        mh = kH[t];
        mw = kW[t];
    }
    DrawRoundRectFill(cx - mw / 2, y - mh / 2, mw, mh, 3);
}

// 实心圆角矩形（眼睛/嘴都用；角半径 r，自动收敛到合法范围）
void OledDisplay::DrawRoundRectFill(int x, int y, int w, int h, int r, bool on) {
    if (w <= 0 || h <= 0) return;
    if (r < 1) { DrawFillRect(x, y, w, h, on); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    DrawFillRect(x, y + r, w, h - 2 * r, on);             // 中间主体
    for (int j = 0; j < r; j++) {                          // 上下各 r 行圆角
        int dy = r - 1 - j;
        int dx = (int)(r - sqrtf((float)(r * r - dy * dy)) + 0.5f);
        if (dx < 0) dx = 0;
        if (2 * dx >= w) continue;
        DrawHLine(x + dx, y + j, w - 2 * dx, on);
        DrawHLine(x + dx, y + h - 1 - j, w - 2 * dx, on);
    }
}

// 实心三角形（扫描线填充）。RoboEyes 用它画上下眼睑——
// ⚠️ 传 BGCOLOR(0) 时是"把眼睛切掉一块"，所以必须支持 on=false，不能只会画白
void OledDisplay::DrawTriangleFill(int x0, int y0, int x1, int y1, int x2, int y2, bool on) {
    const int vx[3] = {x0, x1, x2};
    const int vy[3] = {y0, y1, y2};
    int minY = vy[0], maxY = vy[0];
    for (int k = 1; k < 3; k++) {
        if (vy[k] < minY) minY = vy[k];
        if (vy[k] > maxY) maxY = vy[k];
    }
    for (int y = minY; y <= maxY; y++) {
        int lo = 100000, hi = -100000;
        for (int k = 0; k < 3; k++) {
            int ay = vy[k], by = vy[(k + 1) % 3];
            int ax = vx[k], bx = vx[(k + 1) % 3];
            if (ay == by) continue;                        // 水平边：与该行无交点
            if (y < (ay < by ? ay : by) || y > (ay > by ? ay : by)) continue;
            int xi = ax + (y - ay) * (bx - ax) / (by - ay);
            if (xi < lo) lo = xi;
            if (xi > hi) hi = xi;
        }
        if (lo <= hi) DrawHLine(lo, y, hi - lo + 1, on);
    }
}

// 实心椭圆（逐行扫描填充）
void OledDisplay::DrawEllipseFill(int cx, int cy, int a, int b) {
    if (a <= 0 || b <= 0) return;
    for (int dy = -b; dy <= b; dy++) {
        float t = 1.0f - (float)(dy * dy) / (float)(b * b);
        if (t < 0) t = 0;
        int w = (int)(a * sqrtf(t));
        DrawHLine(cx - w, cy + dy, 2 * w + 1, true);
    }
}

// 眼睛：统一形状（饱满的圆角方块——指数 0.42 让它介于椭圆和矩形之间，更有神）
//   ⚠️ 故意没有 bend 弯曲参数：弯成线条会让眼睛形状跳变，看着不像同一张脸。
//   情绪和眨眼全部靠"这颗方块缩放到多高"来表达。
void OledDisplay::DrawEyeShape(int cx, int cy, int w, int h) {
    if (w < 2 || h < 1) return;
    const int half = w / 2;
    const float hh = h / 2.0f;
    for (int dx = -half; dx <= half; dx++) {
        float t = (float)dx / half;           // -1 .. 1
        float k = 1.0f - t * t;               // 中间 1，两端 0
        if (k < 0) k = 0;
        int col_h = (int)(hh * 2.0f * powf(k, 0.42f));
        if (col_h < 1) col_h = 1;
        DrawFillRect(cx + dx, cy - col_h / 2, 1, col_h, true);
    }
}

// ⚠️ 必须用 font_puhui_14_1（6650 字形，含汉字）！
//    font_puhui_basic_14_1 是"基础拉丁"字库，**一个汉字都没有**（只有 ASCII/希腊/西里尔）
//    → 中文歌名查不到字形、宽度算出来是 0，屏幕上什么都画不出来。
//    这才是"音乐播放没有中文名称"的真正原因（不是抓取逻辑的问题）。
//    注意 1688KB 是它的 C 源码文本体积，编译进固件的二进制数据只有 ~220KB，装得下。
extern const lv_font_t font_puhui_14_1;

// ⚠️ 绝不能调 lvgl 的 lv_font_get_glyph_bitmap()：它在 lv_font.c:55 会把
//    req_raw_bitmap 强制清 0 走 A8 转换，需要一个 draw_buf，而 C3 上 lvgl
//    根本没初始化（本文件是非 LVGL 的纯 framebuffer 实现）→ 传 NULL 直接崩。
//    所以这里只读字库那几张 const 表，零运行时依赖。
// unicode → glyph id（照 lvgl 的 get_glyph_dsc_id，覆盖字库实际用到的格式）
static uint32_t LookupGlyphId(const lv_font_fmt_txt_dsc_t* fd, uint32_t uni) {
    for (uint16_t i = 0; i < fd->cmap_num; i++) {
        const lv_font_fmt_txt_cmap_t* cm = &fd->cmaps[i];
        if (uni < cm->range_start) continue;
        uint32_t rcp = uni - cm->range_start;                  // 相对码点
        if (rcp >= cm->range_length) continue;

        if (cm->type == LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY) {
            return cm->glyph_id_start + rcp;
        }
        if (cm->type == LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL) {
            // ⚠️ FORMAT0 系列的 glyph_id_ofs_list 是 uint8_t*（SPARSE 系列才是 uint16_t*），
            //    弄错会把字形查串（见 lv_font_fmt_txt.h:99-102 的规范）
            const uint8_t* ol = (const uint8_t*)cm->glyph_id_ofs_list;
            // 偏移为 0 且不是区间首字符 → 这个字在字库里是缺的（lvgl 的判定方式）
            if (ol[rcp] == 0 && uni != cm->range_start) continue;
            return cm->glyph_id_start + ol[rcp];
        }
        if (cm->type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY ||
            cm->type == LV_FONT_FMT_TXT_CMAP_SPARSE_FULL) {
            // ⚠️ SPARSE 必须二分：中文那张 cmap 有 8000+ 项，
            //    线性扫一遍每次要几千次比较，20Hz 重绘歌名会明显吃 CPU
            //    （lvgl 自己也是用 bsearch）
            const uint16_t* ul = cm->unicode_list;
            int lo = 0, hi = (int)cm->list_length - 1, idx = -1;
            while (lo <= hi) {
                int mid = (lo + hi) >> 1;
                uint16_t v = ul[mid];
                if (v == rcp) { idx = mid; break; }
                if (v < rcp) lo = mid + 1; else hi = mid - 1;
            }
            if (idx < 0) continue;
            if (cm->type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY) {
                return cm->glyph_id_start + idx;
            }
            const uint16_t* ol = (const uint16_t*)cm->glyph_id_ofs_list;
            return cm->glyph_id_start + ol[idx];
        }
    }
    return 0;      // 0 = 没这个字（字库保留项，无墨迹）
}

// 字形的纵向相对偏移。只取相对量，就不用纠结 lvgl 各版本 ofs_y 的语义差异
static inline int GlyphTop(const lv_font_fmt_txt_glyph_dsc_t* gd) {
    return -(int)gd->box_h - (int)gd->ofs_y;
}

// 画一个字符：top = 字形顶部（由 DrawUtf8String 对齐后传进来）
int OledDisplay::DrawUtf8Char(int x, int top, uint32_t uni) {
    const lv_font_t* f = &font_puhui_14_1;
    const lv_font_fmt_txt_dsc_t* fd = (const lv_font_fmt_txt_dsc_t*)f->dsc;
    uint32_t gid = LookupGlyphId(fd, uni);
    if (!gid) return 0;
    const lv_font_fmt_txt_glyph_dsc_t* gd = &fd->glyph_dsc[gid];
    if (gd->box_w == 0 || gd->box_h == 0) return gd->adv_w >> 4;   // 空格等
    if (gd->box_w > 32 || gd->box_h > 32) return gd->adv_w >> 4;   // 异常数据兜底，防读越界

    const uint8_t* bmp = &fd->glyph_bitmap[gd->bitmap_index];
    // ⚠️ lv_font_conv 生成的 1bpp 位图是**连续位流**：像素 (i,j) 在 bit = j*box_w + i，
    //    行与行之间还会共用同一个字节（**不是**每行按字节对齐！）。
    //    证据：'!' 是 2x10 = 20 bit = 3 字节，而不是按行对齐的 10 字节。
    //    按"每行字节跨度"去解会得到一片随机点（字形彻底不可辨）。
    for (int j = 0; j < gd->box_h; j++) {
        for (int i = 0; i < gd->box_w; i++) {
            uint32_t bit = (uint32_t)j * gd->box_w + i;
            if ((bmp[bit >> 3] >> (7 - (bit & 7))) & 1) {
                SetPixel(x + gd->ofs_x + i, top + j, true);
            }
        }
    }
    return gd->adv_w >> 4;                            // adv_w 是 28.4 定点
}

// UTF-8 字符串（中文/ASCII 混合），返回实际占用宽度
int OledDisplay::DrawUtf8String(int x, int y, const char* s) {
    if (!s) return 0;
    const lv_font_t* f = &font_puhui_14_1;
    const lv_font_fmt_txt_dsc_t* fd = (const lv_font_fmt_txt_dsc_t*)f->dsc;
    if (fd->bpp != 1) return 0;            // 只处理 1bpp（本字库就是 1bpp）

    // 1) 解码 UTF-8（一次最多 32 字，够一行 128px 了）
    uint32_t unis[32];
    int n = 0;
    const uint8_t* p = (const uint8_t*)s;
    while (*p && n < 32) {
        uint32_t uni = 0;
        int len = 1;
        if      ((*p & 0x80) == 0) { uni = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { uni = ((uint32_t)(p[0] & 0x1F) <<  6) | (p[1] & 0x3F); len = 2; }
        else if ((*p & 0xF0) == 0xE0) { uni = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
        else if ((*p & 0xF8) == 0xF0) { uni = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); len = 4; }
        else { p++; continue; }                       // 非法字节，跳过
        unis[n++] = uni;
        p += len;
    }

    // 2) 取所有字顶部偏移的最小值，画的时候减掉 → 整行紧贴 y（不受 ofs_y 语义影响）
    int min_top = 0;
    bool have = false;
    for (int k = 0; k < n; k++) {
        uint32_t gid = LookupGlyphId(fd, unis[k]);
        if (!gid) continue;
        const lv_font_fmt_txt_glyph_dsc_t* gd = &fd->glyph_dsc[gid];
        if (gd->box_h == 0) continue;
        int t = GlyphTop(gd);
        if (!have || t < min_top) { min_top = t; have = true; }
    }

    // 3) 逐个画
    int pen = x;
    for (int k = 0; k < n; k++) {
        uint32_t gid = LookupGlyphId(fd, unis[k]);
        if (!gid) { pen += 7; continue; }             // 缺字：补个空格宽度
        const lv_font_fmt_txt_glyph_dsc_t* gd = &fd->glyph_dsc[gid];
        int top = (gd->box_h == 0) ? 0 : (GlyphTop(gd) - min_top);
        pen += DrawUtf8Char(pen, y + top, unis[k]);
    }
    return pen - x;
}

// 🎵 ♫ 双音符（48x40）：两个符头 + 两根符干 + 顶部横梁
//   之前画单个 ♪：符头椭圆孤零零在底部，很容易被看成「一张嘴」。
//   改成 ♫ 后两个符头 + 横梁的辨识度高得多，一眼就是音乐符号。
void OledDisplay::DrawMusicNote(int x, int y) {
    DrawEllipseFill(x + 13, y + 31, 9, 6);      // 左符头
    DrawEllipseFill(x + 35, y + 31, 9, 6);      // 右符头
    DrawFillRect(x + 20, y + 12, 5, 20, true);  // 左符干
    DrawFillRect(x + 42, y + 12, 5, 20, true);  // 右符干
    DrawFillRect(x + 20, y + 12, 27, 6, true);  // 顶部横梁（连两根符干）
}

// ----------------------- 接口 -----------------------

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y) {
    panel_io_ = panel_io;
    panel_ = panel;
    width_ = width;
    height_ = height;

    lock_ = xSemaphoreCreateRecursiveMutex();

    // 启动 10Hz 刷新定时器（100ms）
    //   ⚠️ 原为 20Hz / 50ms，改慢的原因（2026-10-17）：
    //      RefreshTimerCb 里会 RenderTick() → dirty_ 时 PushFrame()，
    //      而 PushFrame 走 I2C 400kHz 写 1024 字节 framebuffer，
    //      耗时 1024×9bit / 400000 ≈ 23ms，且是阻塞传输。
    //      这个回调跑在 esp_timer 任务里（优先级 22），远高于
    //      audio_input 任务（优先级 8）—— 于是每 50ms 就有约 23ms
    //      让唤醒词检测和音频采集完全停摆，I2C 占用率高达 46%。
    //      体感就是"唤醒反应慢"（唤醒瞬间若正在刷表情动画尤其明显）。
    //      改成 100ms 后占用率降到约 23%，唤醒最多快 23ms；
    //      表情动画仍有 10fps，肉眼可接受。
    //   ⚠️ 遗留：ESP-IDF 不建议在 esp_timer 回调里做长时间阻塞操作。
    //      这里只是降频缓解；彻底解法是把 I2C 传输挪到独立的低优先级任务，
    //      由 timer 只置标志位，任务侧再真正刷屏。
    esp_timer_create_args_t tcfg = {};
    tcfg.callback = &OledDisplay::RefreshTimerCb;
    tcfg.arg = this;
    tcfg.name = "oled_refresh";
    ESP_ERROR_CHECK(esp_timer_create(&tcfg, &refresh_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(refresh_timer_, 100 * 1000));

    // ⚠️ 关键 bug 修复：原代码完全忽略了 mirror_x/mirror_y 参数，config.h 里的
    //    DISPLAY_MIRROR_X/Y 是死代码。手动调 esp_lcd_panel_mirror() 让方向设置生效。
    //    注意：必须在 RenderFace/PushFrame 之前调，否则第一帧会按错误方向画。
    if (panel_) {
        esp_err_t mret = esp_lcd_panel_mirror(panel_, mirror_x, mirror_y);
        if (mret != ESP_OK) {
            ESP_LOGW(TAG, "esp_lcd_panel_mirror returned %s", esp_err_to_name(mret));
        } else {
            ESP_LOGI(TAG, "OLED mirror set: x=%d y=%d", mirror_x ? 1 : 0, mirror_y ? 1 : 0);
        }
    }

    // 首次点亮：默认显示待机时钟（WiFi/NTP 还没就绪时显示占位文字）
    // ⚠️ 改：原先直接 RenderFace() 显示眼睛，违反"开机应显示时钟"的设计。
    //    构造时 animated_mode_ = false（默认值），refresh timer 会走 RenderClock()。
    Clear();
    RenderClock();
    PushFrame();

    // 建好 RoboEyes（眼睛复用 yyxc 那套）。放在这里是因为构造函数结束时它的
    // 定义已经可见；RenderFace 里也有一道兜底判空
    InitRoboEyes();

    ESP_LOGI(TAG, "OledDisplay ready (128x64, 1bpp, no LVGL) - boot with clock");
}

OledDisplay::~OledDisplay() {
    if (refresh_timer_) {
        esp_timer_stop(refresh_timer_);
        esp_timer_delete(refresh_timer_);
    }
    delete eyes_;  eyes_ = nullptr;      // RoboEyes（含内部的 OledGfx 后端）
    delete gfx_;   gfx_  = nullptr;
    if (lock_) vSemaphoreDelete(lock_);
}

void OledDisplay::SetStatus(const char* status) {
    Lock(100);
    if (status) {
        strncpy(status_text_, status, sizeof(status_text_) - 1);
        status_text_[sizeof(status_text_) - 1] = 0;
    } else {
        status_text_[0] = 0;
    }
    dirty_ = true;
    Unlock();
}

void OledDisplay::ShowNotification(const char* notification, int duration_ms) {
    SetStatus(notification);
}

void OledDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    SetStatus(notification.c_str());
}

void OledDisplay::SetChatMessage(const char* role, const char* content) {
    if (!content || !*content) {
        SetStatus("");
        return;
    }
    // ⚠️ 原来这里直接把消息丢了（注释写着"C3 屏太小"），屏幕上永远看不到对话内容。
    //   现在"文字模式"要显示助手回复，所以存下来 —— 只存 assistant 的，
    //   用户自己说的话不需要在屏幕上回显。
    if (role && strcmp(role, "assistant") == 0) {
        Lock(100);
        snprintf(chat_text_, sizeof(chat_text_), "%s", content);
        Unlock();
        dirty_ = true;      // 立刻重画，不用等下一次 tick
    }
}

// 文字 / 表情切换（对话时屏幕上显示什么）
//   切换后立刻重画。持久化（NVS）由调用方（网页 / 按键）负责，这里只管显示。
void OledDisplay::SetChatDisplayMode(bool text_mode) {
    chat_text_mode_ = text_mode;
    dirty_ = true;
}

void OledDisplay::SetEmotion(const char* emotion) {
    Lock(100);
    int idx = 0;  // default = neutral
    if (emotion) {
        std::string e(emotion);
        // ⚠️ 原表只认 6 个英文词，服务端实际会下发 20+ 种情绪名和 emoji
        //    （laughing / winking / confused / 😆 / 🤔 ...），全部落到 neutral
        //    → 用户看到"只有眨眼、没有别的表情"。这里做全覆盖映射。
        if      (e == "neutral" || e == "😶" || e == "sleepy" || e == "😴") {
            idx = 0;   // 平静 / 困
        }
        else if (e == "happy"  || e == "🙂" || e == "😊" || e == "smile"   ||
                 e == "loving" || e == "😍" || e == "kissy"   || e == "😘"  ||
                 e == "cool"   || e == "😎" || e == "relaxed" || e == "😌"  ||
                 e == "confident" || e == "😏" || e == "delicious" || e == "🤤" ||
                 e == "wink"   || e == "winking" || e == "😉") {
            idx = 1;   // 开心 / 喜欢 / 得意
        }
        else if (e == "sad"    || e == "😔" || e == "crying" || e == "😭" ||
                 e == "cry"    || e == "embarrassed" || e == "😳") {
            idx = 2;   // 难过 / 哭 / 害羞
        }
        else if (e == "angry"  || e == "😠" || e == "mad" || e == "😡" ||
                 e == "annoyed" || e == "🙄") {
            idx = 7;   // 生气（横椭圆眼 + 视线下压 + 眨得勤）
        }
        else if (e == "thinking" || e == "🤔" || e == "confused" || e == "🙄" ||
                 e == "silly"    || e == "😜" || e == "thinking_face") {
            idx = 3;   // 思考 / 困惑（视线往右）
        }
        else if (e == "listening" || e == "curious") {
            idx = 4;   // 聆听（视线往左、睁大）
        }
        else if (e == "surprised" || e == "😲" || e == "shocked" || e == "😱") {
            idx = 6;   // 惊讶（眼睛放到最大最圆，且很少眨眼 = 盯着看）
        }
        else if (e == "speaking" || e == "talking" || e == "wake" ||
                 e == "laughing" || e == "😆" || e == "funny" || e == "😂" ||
                 e == "laugh") {
            idx = 5;   // 说话 / 大笑（嘴会动）
        }
        // 其余未知情绪 → 保持 0 neutral
    }
    emotion_idx_ = idx;
    ApplyRoboEmotion(idx);      // 让 RoboEyes 立刻切到这套 mood / 视线 / 动画
    dirty_ = true;
    Unlock();
}

void OledDisplay::SetAnimatedEmotionMode(bool enable) {
    animated_mode_ = enable;
    // 进入时钟模式时把 last_clock_sec_ 设为 -1，让下一帧立即渲染
    if (!enable) {
        last_clock_sec_ = -1;
    }
    dirty_ = true;
}

void OledDisplay::SetMusicMode(bool on) {
    if (music_mode_.load() == on) return;
    music_mode_ = on;
    dirty_ = true;   // 时钟模式也要立刻重画（去掉/加上音符）
}

// 🎵 音乐界面：整屏居中 2 个 32x32 大音符，替代表情/时钟显示
// ------------------ 整屏信息页：配网信息 / 激活验证码 ------------------
// 从 C3 小车那版移植过来（xiaozhi-car/main/display/oled_display.cc）。
// 目的：这台成品板的 AP 是开放网络，用户必须知道"连哪个热点 / 打开哪个地址"；
//   激活时必须看到那串数字才能去 APP 里输入。原来这两件事只在串口日志里打，
//   屏幕上什么都没有（Alert 写的 status_text_ 在表情/壁纸模式下不渲染）。

// 中文/ASCII 混合字符串的像素宽度（累加 14px 普惠字体的 adv_w）
//   只用于算居中位置。缺字按半角 7px 估，别把整行挤歪。
int OledDisplay::MeasureUtf8String(const char* s) {
    if (!s) return 0;
    const lv_font_t* f = &font_puhui_14_1;
    const lv_font_fmt_txt_dsc_t* fd = (const lv_font_fmt_txt_dsc_t*)f->dsc;
    if (fd->bpp != 1) return (int)strlen(s) * 7;

    int w = 0;
    const uint8_t* p = (const uint8_t*)s;
    while (*p) {
        uint32_t uni = 0;
        int len = 1;
        if      ((*p & 0x80) == 0)    { uni = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { uni = ((uint32_t)(p[0] & 0x1F) <<  6) | (p[1] & 0x3F); len = 2; }
        else if ((*p & 0xF0) == 0xE0) { uni = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
        else if ((*p & 0xF8) == 0xF0) { uni = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); len = 4; }
        else { p++; continue; }                     // 非法字节，跳过

        uint32_t gid = LookupGlyphId(fd, uni);
        if (gid) {
            const lv_font_fmt_txt_glyph_dsc_t* gd = &fd->glyph_dsc[gid];
            w += gd->adv_w >> 4;                    // adv_w 是 28.4 定点
        } else {
            w += 7;                                 // 缺字
        }
        p += len;
    }
    return w;
}

// 整屏配网信息（128x64 布局）：
//   y=2   请连接热点      （14px 中文，居中）
//   y=20  热点名          （6x8 ASCII，居中）
//   y=38  打开浏览器访问  （14px 中文，居中）
//   y=50  配网地址        （6x8 ASCII，居中）
void OledDisplay::RenderWifiConfigInfo() {
    const char* t1 = "请连接热点";
    int x = (kWidth - MeasureUtf8String(t1)) / 2;
    DrawUtf8String(x < 0 ? 0 : x, 2, t1);

    // ⚠️ 6x8 小字库里没有 '/'（会画成空格），所以调用方已经把 "http://" 前缀去掉了
    int w = (int)strlen(cfg_ssid_) * 6;
    x = (kWidth - w) / 2;
    DrawString6x8(x < 0 ? 0 : x, 20, cfg_ssid_);

    const char* t2 = "打开浏览器访问";
    x = (kWidth - MeasureUtf8String(t2)) / 2;
    DrawUtf8String(x < 0 ? 0 : x, 38, t2);

    w = (int)strlen(cfg_url_) * 6;
    x = (kWidth - w) / 2;
    DrawString6x8(x < 0 ? 0 : x, 50, cfg_url_);
}

// 整屏激活验证码：标题 + 大号数字（用户照着输入到 APP / 控制台）
void OledDisplay::RenderActivationCode() {
    const char* t = "激活验证码";
    int x = (kWidth - MeasureUtf8String(t)) / 2;
    DrawUtf8String(x < 0 ? 0 : x, 4, t);

    // scale=2 → 每字符占位 12px（5px 字放大 2 倍 + 间距），整体居中
    int total = (int)strlen(act_code_) * 12 - 2;   // 最后一个字符不算尾部间距
    x = (kWidth - total) / 2;
    DrawStringScaled(x < 0 ? 0 : x, 26, act_code_, 2);
}

void OledDisplay::SetWifiConfigInfo(const char* ssid, const char* url) {
    Lock(100);
    if (ssid && ssid[0]) {
        strncpy(cfg_ssid_, ssid, sizeof(cfg_ssid_) - 1);
        cfg_ssid_[sizeof(cfg_ssid_) - 1] = 0;
        strncpy(cfg_url_, url ? url : "", sizeof(cfg_url_) - 1);
        cfg_url_[sizeof(cfg_url_) - 1] = 0;
        cfg_info_visible_ = true;
    } else {
        cfg_info_visible_ = false;      // 传空串 = 关掉这一页
    }
    dirty_ = true;
    Unlock();
}

void OledDisplay::SetActivationCode(const char* code) {
    Lock(100);
    if (code && code[0]) {
        strncpy(act_code_, code, sizeof(act_code_) - 1);
        act_code_[sizeof(act_code_) - 1] = 0;
        act_code_visible_ = true;
    } else {
        act_code_visible_ = false;      // 传空串 = 关掉这一页（激活完成）
    }
    dirty_ = true;
    Unlock();
}

void OledDisplay::RenderMusic() {
    Clear();

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (boot_ms_ == 0) boot_ms_ = now_ms;
    uint32_t rel = now_ms - boot_ms_;

    // 歌名（顶部居中，14px 中文字库 —— font_puhui_14_1 含 12814 个汉字）
    //   宽度在 SetSongName 里已经量好了，这里直接用
    if (song_name_[0]) {
        int x = (kWidth - song_w_) / 2;
        if (x < 0) x = 0;                  // 歌名太长就左对齐，别两边都被裁掉
        DrawUtf8String(x, 0, song_name_);
    }

    // 单个大 ♫（48x40）：比并排两个小音符更大更清楚，
    //   也不会像单音符 ♪ 那样把符头椭圆看成「一张嘴」
    static constexpr int kNoteW = 48;
    static constexpr int kNoteH = 40;
    int x0     = (kWidth - kNoteW) / 2;
    int base_y = 18;                           // 顶部歌名约占 0..16，音符从 18 起

    int bob = (int)((rel / 300) % 3) - 1;      // -1 / 0 / 1，轻微跳动
    DrawMusicNote(x0, base_y + bob);
    // 音乐界面不画状态文字：竖向空间都给了歌名和音符

    dirty_ = true;   // 持续动画：下一帧继续推屏
}

void OledDisplay::UpdateAnimatedEmotion() {
    // 主循环每 tick 也会调这里 → 必须走 RenderTick()，不能自己判断
    // （否则会和 timer 路径逻辑不一致，见 RenderTick 的注释）
    if (Lock(0)) {
        RenderTick();
        Unlock();
    }
}

void OledDisplay::SetSpeaking(bool on) {
    if (speaking_.load() == on) return;
    speaking_ = on;
    dirty_ = true;
}

void OledDisplay::SetSongName(const char* name) {
    Lock(100);
    if (!name || !*name) {
        song_name_[0] = 0;
        song_w_ = 0;
    } else {
        snprintf(song_name_, sizeof(song_name_), "%s", name);   // char[32]：最多 10 个汉字，够屏宽
        // 宽度只在这里算一次（用离屏坐标测宽，越界像素会被 SetPixel 裁掉）。
        // 否则 RenderMusic 每帧都要重跑一遍 UTF-8 解码 + 字形查找，白烧 CPU
        song_w_ = DrawUtf8String(-200, 0, song_name_);
    }
    Unlock();
    dirty_ = true;
}
