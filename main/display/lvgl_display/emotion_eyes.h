#ifndef EMOTION_EYES_H
#define EMOTION_EYES_H

#include <lvgl.h>
#include <cstdint>

// emotion-ball 风格的动画表情：一颗奶油色小球 + 一对会眨/会瞟的眼睛。
// 21 种表情由纯参数表驱动，几何量用弹簧插值平滑过渡。
// 所有 LVGL 操作都发生在内部 lv_timer（LVGL 任务上下文）中，
// 对外接口只做线程安全的参数投递。
class EmotionEyes {
public:
    enum EyeShape : uint8_t {
        SHAPE_BALL,       // 椭圆（可旋转、可眨眼、可缩放）
        SHAPE_ARC_UP,     // 上弯月牙 ∩（开心）
        SHAPE_ARC_DOWN,   // 下弯月牙 ∪（难过）
        SHAPE_LINE,       // 细横线（闭眼）
        SHAPE_HEART,      // 爱心图标
        SHAPE_GLASSES,    // 墨镜
    };
    enum MouthShape : uint8_t {
        MOUTH_NONE, MOUTH_LINE, MOUTH_SMILE, MOUTH_FROWN, MOUTH_OPEN, MOUTH_BIG_OPEN, MOUTH_PUCKER
    };
    enum Extra : uint8_t { EX_NONE = 0, EX_TEAR = 1, EX_BLUSH = 2, EX_ZZZ = 4 };

    struct FaceConfig {
        EyeShape shape;
        float lw, lh, lrot;   // 左眼 宽/高/旋转角
        float rw, rh, rrot;   // 右眼
        MouthShape mouth;
        uint8_t extras;
    };

    EmotionEyes(lv_obj_t* parent, int screen_w, int screen_h);
    ~EmotionEyes();

    // 线程安全：投递目标表情（LLM 的 21 个 emotion 字符串）
    void SetEmotion(const char* emotion);
    // 显示/隐藏整个表情（隐藏时停帧省电）
    void SetActive(bool active);
    // 0=中 1..8=八方向瞟视
    void SetDirection(int direction);

    // 未知表情的回退配置
    static const FaceConfig* FindFace(const char* emotion);

private:
    static void TimerCallback(lv_timer_t* timer);
    void Tick();
    void ApplyFace(const FaceConfig* face);
    void BuildWidgets(lv_obj_t* parent);
    void UpdateEyeWidgets();
    void UpdateMouth();

    int screen_w_, screen_h_;

    lv_obj_t* container_ = nullptr;
    lv_obj_t* ball_ = nullptr;          // 身体
    lv_obj_t* l_ball_ = nullptr;        // 左眼（椭圆）
    lv_obj_t* r_ball_ = nullptr;
    lv_obj_t* l_arc_ = nullptr;         // 月牙眼（角度随表情切换）
    lv_obj_t* r_arc_ = nullptr;
    lv_obj_t* l_line_ = nullptr;        // 线状眼
    lv_obj_t* r_line_ = nullptr;
    lv_obj_t* l_heart_ = nullptr;       // 爱心眼
    lv_obj_t* r_heart_ = nullptr;
    lv_obj_t* l_glass_ = nullptr;       // 墨镜片
    lv_obj_t* r_glass_ = nullptr;
    lv_obj_t* glass_bridge_ = nullptr;  // 墨镜梁
    lv_obj_t* mouth_arc_ = nullptr;
    lv_obj_t* mouth_open_ = nullptr;
    lv_obj_t* mouth_line_ = nullptr;
    lv_obj_t* tear_ = nullptr;
    lv_obj_t* blush_l_ = nullptr;
    lv_obj_t* blush_r_ = nullptr;
    lv_obj_t* zzz_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    const FaceConfig* current_ = nullptr;
    const FaceConfig* target_ = nullptr;
    float cur_lw_ = 30, cur_lh_ = 50, cur_lrot_ = 0;
    float cur_rw_ = 30, cur_rh_ = 50, cur_rrot_ = 0;
    uint32_t t_ms_ = 0;
    int32_t next_blink_ms_ = 3000;
    int32_t blink_phase_ms_ = -1;       // <0 = 未在眨眼
    float glance_tx_ = 0, glance_ty_ = 0;
    float glance_x_ = 0, glance_y_ = 0;
    int direction_ = 0;
    float ball_scale_ = 1.0f;
    int32_t bounce_ms_ = -1;
    float tear_y_ = 0;

    // 跨线程投递
    volatile bool pending_ = false;
    char pending_emotion_[32] = {0};
    bool active_ = false;
};

#endif // EMOTION_EYES_H
