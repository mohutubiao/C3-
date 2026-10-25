#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"
#include "emotion_eyes.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    // emotion-ball 风格动画表情
    EmotionEyes* emotion_eyes_ = nullptr;
    // 待机时钟
    lv_obj_t* clock_container_ = nullptr;
    lv_obj_t* clock_panel_ = nullptr;        // 半透明圆角面板（壁纸背景下保证可读性）
    lv_obj_t* clock_time_label_ = nullptr;
    lv_obj_t* clock_date_label_ = nullptr;
    bool clock_first_show_ = true;           // 首次显示时钟时播放过渡动画
    IdleScreenOptions idle_opts_;            // 待机界面元素配置（NVS 持久化）

    void InitializeLcdThemes();
    void SetupUI();
    void ApplyClockPanelPos();       // 按配置摆放时钟面板位置（调用方需持锁）
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
    
protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override; 
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // 动画表情 / 待机时钟
    virtual void SetAnimatedEmotionMode(bool enable) override;
    virtual void UpdateAnimatedEmotion() override;
    virtual void SetEmotionDirection(int direction) override;
    virtual void ShowClock(bool show) override;
    virtual void UpdateClockText() override;
    virtual void SetIdleScreenOptions(const IdleScreenOptions& opts) override;
    
    // Set whether to hide chat messages/subtitles
    virtual void SetHideSubtitle(bool hide);
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
