#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }
private:
    std::string name_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);

    // 动画表情相关方法
    virtual void SetAnimatedEmotionMode(bool enable);
    virtual bool IsAnimatedEmotionMode() const { return animated_emotion_mode_; }

    // 对话中是否**想看文字**（而不是动画表情眼睛）。
    //   基类默认 false；OledDisplay 在用户把显示模式切成「文字」时返回 true。
    //   用途：application 原先只在「非动画表情模式」下才下发 assistant 消息，
    //        而文字模式恰好是动画模式下才显示 —— 导致它永远拿不到内容
    //        （屏幕上只显示三个点）。现在靠这个判断把内容放过去。
    virtual bool WantsChatText() const { return false; }
    virtual void SetEmotionDirection(int direction); // 0-8: center, up, down, left, right, diagonals
    virtual void UpdateAnimatedEmotion();

    // 待机时钟界面
    virtual void ShowClock(bool show) { (void)show; }
    virtual void UpdateClockText() {}

    // 网络 / 时间状态通知（OledDisplay 用来切换时钟内容；LCD/NoDisplay 默认忽略）
    virtual void OnWifiState(bool connected) { (void)connected; }
    virtual void OnClockSynced(bool synced) { (void)synced; }

    // 音乐播放指示：服务端下发 AI 歌曲时会在歌词前插一条 "% play_music..."
    // 的 sentence_start 标记，设备据此切换到音乐界面（默认忽略）。
    virtual void SetMusicMode(bool on) { (void)on; }
    // 歌名：play_music 标记之后的第一条 sentence_start 就是歌名（默认忽略）。
    virtual void SetSongName(const char* name) { (void)name; }
    // TTS 正在说话：进入/退出播报时通知，用来让嘴跟着动（默认忽略）。
    virtual void SetSpeaking(bool on) { (void)on; }

    // 待机界面元素配置（控制页可改，重启保留）
    struct IdleScreenOptions {
        bool show_time = true;
        bool show_date = true;
        bool show_anim = true;
        int position = 1;  // 0=偏上 1=居中 2=偏下
    };
    virtual void SetIdleScreenOptions(const IdleScreenOptions& opts) { (void)opts; }

    // 是否隐藏底部字幕（纯眼睛画面用）
    virtual void SetHideSubtitle(bool hide) { (void)hide; }

    inline int width() const { return width_; }
    inline int height() const { return height_; }

protected:
    bool animated_emotion_mode_ = false;
    int width_ = 0;
    int height_ = 0;

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};


class DisplayLockGuard {
public:
    DisplayLockGuard(Display *display) : display_(display) {
        if (!display_->Lock(30000)) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        display_->Unlock();
    }

private:
    Display *display_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif
