#include "emotion_eyes.h"
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_random.h>
#include <cstring>
#include <cmath>

#define TAG "EmotionEyes"

LV_FONT_DECLARE(font_awesome_30_4);

namespace {

constexpr uint32_t kBodyColor = 0x000000;   // 无实体球：纯黑背景
constexpr uint32_t kBodyBorder = 0x000000;
constexpr uint32_t kEyeColor = 0xFFFFFF;    // 白色眼睛
constexpr uint32_t kBlushColor = 0xFF9EB5;
constexpr uint32_t kTearColor = 0x6EC3FF;
constexpr uint32_t kHeartColor = 0xFFFFFF;  // 白色（黑底可見）

// 布局常量（相对小球中心）——整体放大 1.3 倍
constexpr float kFaceScale = 1.3f;
constexpr float kEyeDx = 34 * kFaceScale;    // 眼距中心
constexpr float kEyeDy = -12 * kFaceScale;   // 眼睛高度
constexpr float kMouthDy = 34 * kFaceScale;  // 嘴高度
constexpr float kBallSize = 172 * kFaceScale;

} // namespace

// 21 种表情参数表（emotion-ball 视觉语言：形状 + 旋转 + 嘴 + 配件）
static const EmotionEyes::FaceConfig kFaces[] = {
    // shape                     lw   lh  lrot   rw   rh  rrot   mouth                extras
    /* neutral     */ { EmotionEyes::SHAPE_BALL,     30, 50,   0,   30, 50,   0,  EmotionEyes::MOUTH_LINE,      EmotionEyes::EX_NONE },
    /* happy       */ { EmotionEyes::SHAPE_ARC_UP,    46, 26,   0,   46, 26,   0,  EmotionEyes::MOUTH_SMILE,    EmotionEyes::EX_NONE },
    /* laughing    */ { EmotionEyes::SHAPE_ARC_UP,    50, 30,   0,   50, 30,   0,  EmotionEyes::MOUTH_BIG_OPEN, EmotionEyes::EX_NONE },
    /* funny       */ { EmotionEyes::SHAPE_BALL,     24, 42,  18,   24, 42, -18,  EmotionEyes::MOUTH_SMILE,    EmotionEyes::EX_NONE },
    /* sad         */ { EmotionEyes::SHAPE_ARC_DOWN,  46, 26,   0,   46, 26,   0,  EmotionEyes::MOUTH_FROWN,    EmotionEyes::EX_NONE },
    /* angry       */ { EmotionEyes::SHAPE_BALL,     28, 40, -22,   28, 40,  22,  EmotionEyes::MOUTH_FROWN,    EmotionEyes::EX_NONE },
    /* crying      */ { EmotionEyes::SHAPE_ARC_DOWN,  46, 26,   0,   46, 26,   0,  EmotionEyes::MOUTH_FROWN,    EmotionEyes::EX_TEAR },
    /* loving      */ { EmotionEyes::SHAPE_HEART,      0,  0,   0,    0,  0,   0,  EmotionEyes::MOUTH_SMILE,    EmotionEyes::EX_BLUSH },
    /* embarrassed */ { EmotionEyes::SHAPE_BALL,     22, 34,   8,   22, 34,  -8,  EmotionEyes::MOUTH_LINE,     EmotionEyes::EX_BLUSH },
    /* surprised   */ { EmotionEyes::SHAPE_BALL,     42, 42,   0,   42, 42,   0,  EmotionEyes::MOUTH_OPEN,     EmotionEyes::EX_NONE },
    /* shocked     */ { EmotionEyes::SHAPE_BALL,     50, 50,   0,   50, 50,   0,  EmotionEyes::MOUTH_BIG_OPEN, EmotionEyes::EX_NONE },
    /* thinking    */ { EmotionEyes::SHAPE_BALL,     28, 46,   0,   28, 30,   0,  EmotionEyes::MOUTH_LINE,     EmotionEyes::EX_NONE },
    /* winking     */ { EmotionEyes::SHAPE_BALL,     30, 50,   0,   30, 50,   0,  EmotionEyes::MOUTH_SMILE,    EmotionEyes::EX_NONE },  // 右眼切换为 LINE
    /* cool        */ { EmotionEyes::SHAPE_GLASSES,    0,  0,   0,    0,  0,   0,  EmotionEyes::MOUTH_LINE,     EmotionEyes::EX_NONE },
    /* relaxed     */ { EmotionEyes::SHAPE_BALL,     32, 26,   0,   32, 26,   0,  EmotionEyes::MOUTH_LINE,     EmotionEyes::EX_NONE },
    /* delicious   */ { EmotionEyes::SHAPE_ARC_UP,    44, 24,   0,   44, 24,   0,  EmotionEyes::MOUTH_OPEN,     EmotionEyes::EX_BLUSH },
    /* kissy       */ { EmotionEyes::SHAPE_LINE,      0,  0,   0,    0,  0,   0,  EmotionEyes::MOUTH_PUCKER,   EmotionEyes::EX_NONE },
    /* confident   */ { EmotionEyes::SHAPE_BALL,     26, 40,  15,   26, 40, -15,  EmotionEyes::MOUTH_SMILE,    EmotionEyes::EX_NONE },
    /* sleepy      */ { EmotionEyes::SHAPE_LINE,      0,  0,   0,    0,  0,   0,  EmotionEyes::MOUTH_LINE,     EmotionEyes::EX_ZZZ },
    /* silly       */ { EmotionEyes::SHAPE_BALL,     34, 34,   0,   20, 52,   0,  EmotionEyes::MOUTH_SMILE,    EmotionEyes::EX_NONE },
    /* confused    */ { EmotionEyes::SHAPE_BALL,     40, 28,   0,   22, 48,   0,  EmotionEyes::MOUTH_LINE,     EmotionEyes::EX_NONE },
};

static const char* kEmotionNames[] = {
    "neutral", "happy", "laughing", "funny", "sad", "angry", "crying",
    "loving", "embarrassed", "surprised", "shocked", "thinking", "winking",
    "cool", "relaxed", "delicious", "kissy", "confident", "sleepy",
    "silly", "confused",
};

const EmotionEyes::FaceConfig* EmotionEyes::FindFace(const char* emotion) {
    if (emotion == nullptr) {
        return &kFaces[0];
    }
    const char* mapped = emotion;
    if (strcmp(emotion, "smile") == 0) mapped = "happy";
    else if (strcmp(emotion, "laugh") == 0) mapped = "laughing";
    else if (strcmp(emotion, "cry") == 0) mapped = "crying";
    else if (strcmp(emotion, "wink") == 0) mapped = "winking";
    for (size_t i = 0; i < sizeof(kEmotionNames) / sizeof(kEmotionNames[0]); i++) {
        if (strcmp(mapped, kEmotionNames[i]) == 0) {
            return &kFaces[i];
        }
    }
    return nullptr;  // 未知表情由调用方决定回退
}

// ------------------------------------------------------------------ 部件构建

static lv_obj_t* make_blob(lv_obj_t* parent, uint32_t color) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return obj;
}

static lv_obj_t* make_arc(lv_obj_t* parent, uint32_t color, int a0, int a1, int width) {
    lv_obj_t* obj = lv_arc_create(parent);
    lv_obj_remove_style(obj, NULL, (lv_style_selector_t)(LV_PART_KNOB | LV_STATE_ANY));
    lv_obj_remove_style(obj, NULL, (lv_style_selector_t)(LV_PART_INDICATOR | LV_STATE_ANY));
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_arc_width(obj, width, 0);
    lv_obj_set_style_arc_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_arc_rounded(obj, true, 0);
    lv_arc_set_rotation(obj, 0);
    lv_arc_set_bg_angles(obj, a0, a1);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return obj;
}

static lv_obj_t* make_label(lv_obj_t* parent, const char* text, uint32_t color, const lv_font_t* font) {
    lv_obj_t* obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    if (font != nullptr) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return obj;
}

void EmotionEyes::BuildWidgets(lv_obj_t* parent) {
    container_ = lv_obj_create(parent);
    lv_obj_remove_style_all(container_);
    lv_obj_set_size(container_, screen_w_, screen_h_);
    // 纯黑背景：唤醒后只有白色眼睛和嘴
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);

    // 球身体改为透明布局容器（眼睛/嘴仍是它的子对象，但不画身体）
    ball_ = make_blob(container_, kBodyColor);
    lv_obj_remove_flag(ball_, LV_OBJ_FLAG_HIDDEN);  // 必须可见（子对象才能渲染）
    lv_obj_set_style_bg_opa(ball_, LV_OPA_TRANSP, 0);  // 透明：无实体圆脸
    lv_obj_set_style_border_width(ball_, 0, 0);
    lv_obj_set_size(ball_, (int)kBallSize, (int)kBallSize);
    lv_obj_align(ball_, LV_ALIGN_CENTER, 0, 4);

    // 眼睛（每种形态一组部件，按表情切换可见性）
    l_ball_ = make_blob(ball_, kEyeColor);
    r_ball_ = make_blob(ball_, kEyeColor);
    l_arc_ = make_arc(ball_, kEyeColor, 180, 360, 11 * kFaceScale);   // ∩
    r_arc_ = make_arc(ball_, kEyeColor, 180, 360, 11 * kFaceScale);
    l_line_ = make_blob(ball_, kEyeColor);
    r_line_ = make_blob(ball_, kEyeColor);
    lv_obj_set_size(l_line_, 36 * kFaceScale, 6 * kFaceScale);
    lv_obj_set_size(r_line_, 36 * kFaceScale, 6 * kFaceScale);
    l_heart_ = make_label(ball_, FONT_AWESOME_HEART, kHeartColor, &font_awesome_30_4);
    r_heart_ = make_label(ball_, FONT_AWESOME_HEART, kHeartColor, &font_awesome_30_4);

    // 墨镜（cool）— 黑底用白色
    l_glass_ = make_blob(ball_, kEyeColor);
    r_glass_ = make_blob(ball_, kEyeColor);
    lv_obj_set_style_radius(l_glass_, 12 * kFaceScale, 0);
    lv_obj_set_style_radius(r_glass_, 12 * kFaceScale, 0);
    lv_obj_set_size(l_glass_, 54 * kFaceScale, 36 * kFaceScale);
    lv_obj_set_size(r_glass_, 54 * kFaceScale, 36 * kFaceScale);
    glass_bridge_ = make_blob(ball_, kEyeColor);
    lv_obj_set_style_radius(glass_bridge_, 3 * kFaceScale, 0);
    lv_obj_set_size(glass_bridge_, 22 * kFaceScale, 7 * kFaceScale);

    // 嘴
    mouth_arc_ = make_arc(ball_, kEyeColor, 0, 180, 9 * kFaceScale);  // ∪ 微笑
    mouth_open_ = make_blob(ball_, kEyeColor);
    mouth_line_ = make_blob(ball_, kEyeColor);
    lv_obj_set_size(mouth_line_, 30 * kFaceScale, 5 * kFaceScale);

    // 配件
    tear_ = make_blob(ball_, kTearColor);
    lv_obj_set_size(tear_, 8 * kFaceScale, 12 * kFaceScale);
    blush_l_ = make_blob(ball_, kBlushColor);
    blush_r_ = make_blob(ball_, kBlushColor);
    lv_obj_set_size(blush_l_, 22 * kFaceScale, 12 * kFaceScale);
    lv_obj_set_size(blush_r_, 22 * kFaceScale, 12 * kFaceScale);
    lv_obj_set_style_bg_opa(blush_l_, LV_OPA_60, 0);
    lv_obj_set_style_bg_opa(blush_r_, LV_OPA_60, 0);
    zzz_ = make_label(ball_, "z z", kEyeColor, nullptr);
}

EmotionEyes::EmotionEyes(lv_obj_t* parent, int screen_w, int screen_h)
    : screen_w_(screen_w), screen_h_(screen_h) {
    BuildWidgets(parent);
    current_ = &kFaces[0];
    target_ = &kFaces[0];
    cur_lw_ = target_->lw * kFaceScale;
    cur_lh_ = target_->lh * kFaceScale;
    cur_rw_ = target_->rw * kFaceScale;
    cur_rh_ = target_->rh * kFaceScale;
    ApplyFace(target_);
    timer_ = lv_timer_create(TimerCallback, 50, this);
    lv_timer_pause(timer_);  // 未激活时停帧
}

EmotionEyes::~EmotionEyes() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
    }
    if (container_ != nullptr) {
        lv_obj_delete(container_);
    }
}

// ------------------------------------------------------------------ 对外接口

void EmotionEyes::SetEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return;
    }
    strncpy(pending_emotion_, emotion, sizeof(pending_emotion_) - 1);
    pending_emotion_[sizeof(pending_emotion_) - 1] = '\0';
    pending_ = true;
    if (timer_ != nullptr && active_) {
        lv_timer_resume(timer_);
    }
}

void EmotionEyes::SetActive(bool active) {
    active_ = active;
    if (container_ == nullptr) {
        return;
    }
    if (active) {
        lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(container_);   // 确保不被后创建的对象遮挡
        lv_obj_invalidate(lv_screen_active()); // 强制全屏重绘
        if (timer_ != nullptr) {
            lv_timer_resume(timer_);
        }
        ESP_LOGI(TAG, "SetActive(true): container=%p hidden=%d ball_hidden=%d child_cnt=%lu",
                 container_, lv_obj_has_flag(container_, LV_OBJ_FLAG_HIDDEN),
                 ball_ ? lv_obj_has_flag(ball_, LV_OBJ_FLAG_HIDDEN) : -1,
                 lv_obj_get_child_cnt(container_));
    } else {
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
        if (timer_ != nullptr) {
            lv_timer_pause(timer_);
        }
        ESP_LOGI(TAG, "SetActive(false)");
    }
}

void EmotionEyes::SetDirection(int direction) {
    if (direction >= 0 && direction <= 8) {
        direction_ = direction;
    }
}

// ------------------------------------------------------------------ 动画核心

void EmotionEyes::TimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<EmotionEyes*>(lv_timer_get_user_data(timer));
    self->Tick();
}

static void dir_offset(int dir, float& dx, float& dy) {
    // 0=中 1=左上 2=上 3=右上 4=左 5=右 6=左下 7=下 8=右下
    static const float kTable[9][2] = {
        {0, 0}, {-6, -5}, {0, -7}, {6, -5}, {-7, 0}, {7, 0}, {-6, 5}, {0, 7}, {6, 5},
    };
    dx = kTable[dir][0];
    dy = kTable[dir][1];
}

void EmotionEyes::ApplyFace(const FaceConfig* face) {
    current_ = face;
    if (face == nullptr) {
        return;
    }

    // 眼睛形态切换
    UpdateEyeWidgets();
    UpdateMouth();

    // 只保留眼睛和嘴：配件（泪滴/腮红/Zzz）一律隐藏
    lv_obj_add_flag(tear_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(blush_l_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(blush_r_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(zzz_, LV_OBJ_FLAG_HIDDEN);

    // 弹跳反馈
    bounce_ms_ = 0;
}

void EmotionEyes::UpdateEyeWidgets() {
    const FaceConfig* f = current_;
    if (f == nullptr) {
        return;
    }

    auto show_ball = [&](lv_obj_t* ball, lv_obj_t* arc, lv_obj_t* line, lv_obj_t* heart,
                         lv_obj_t* glass, EyeShape shape) {
        bool use_ball = (shape == SHAPE_BALL);
        bool use_arc = (shape == SHAPE_ARC_UP || shape == SHAPE_ARC_DOWN);
        bool use_line = (shape == SHAPE_LINE);
        bool use_heart = (shape == SHAPE_HEART);
        bool use_glass = (shape == SHAPE_GLASSES);

        if (use_ball) lv_obj_remove_flag(ball, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ball, LV_OBJ_FLAG_HIDDEN);

        if (use_arc) {
            lv_arc_set_bg_angles(arc, shape == SHAPE_ARC_UP ? 180 : 0,
                                 shape == SHAPE_ARC_UP ? 360 : 180);
            lv_obj_remove_flag(arc, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
        }

        if (use_line) lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);

        if (use_heart) lv_obj_remove_flag(heart, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(heart, LV_OBJ_FLAG_HIDDEN);

        if (use_glass) lv_obj_remove_flag(glass, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(glass, LV_OBJ_FLAG_HIDDEN);
    };

    show_ball(l_ball_, l_arc_, l_line_, l_heart_, l_glass_, f->shape);
    show_ball(r_ball_, r_arc_, r_line_, r_heart_, r_glass_, f->shape);
    // winking：右眼用线
    if (strcmp(kEmotionNames[current_ - kFaces], "winking") == 0) {
        lv_obj_add_flag(r_ball_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r_line_, LV_OBJ_FLAG_HIDDEN);
    }
    // 墨镜梁与右片一起显示
    if (f->shape == SHAPE_GLASSES) {
        lv_obj_remove_flag(glass_bridge_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(glass_bridge_, LV_OBJ_FLAG_HIDDEN);
    }
}

void EmotionEyes::UpdateMouth() {
    const FaceConfig* f = current_;
    if (f == nullptr) {
        return;
    }
    auto show = [&](lv_obj_t* o, bool on) {
        if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    show(mouth_arc_, f->mouth == MOUTH_SMILE || f->mouth == MOUTH_FROWN);
    show(mouth_open_, f->mouth == MOUTH_OPEN || f->mouth == MOUTH_BIG_OPEN || f->mouth == MOUTH_PUCKER);
    show(mouth_line_, f->mouth == MOUTH_LINE);

    if (f->mouth == MOUTH_SMILE) {
        lv_arc_set_bg_angles(mouth_arc_, 0, 180);     // ∪
        lv_obj_set_size(mouth_arc_, 38 * kFaceScale, 22 * kFaceScale);
    } else if (f->mouth == MOUTH_FROWN) {
        lv_arc_set_bg_angles(mouth_arc_, 180, 360);   // ∩
        lv_obj_set_size(mouth_arc_, 38 * kFaceScale, 22 * kFaceScale);
    }
    if (f->mouth == MOUTH_OPEN) {
        lv_obj_set_size(mouth_open_, 14 * kFaceScale, 16 * kFaceScale);
    } else if (f->mouth == MOUTH_BIG_OPEN) {
        lv_obj_set_size(mouth_open_, 24 * kFaceScale, 30 * kFaceScale);
    } else if (f->mouth == MOUTH_PUCKER) {
        lv_obj_set_size(mouth_open_, 9 * kFaceScale, 9 * kFaceScale);
    }
    lv_obj_align_to(mouth_arc_, ball_, LV_ALIGN_CENTER, 0, (int)kMouthDy);
    lv_obj_align_to(mouth_open_, ball_, LV_ALIGN_CENTER, 0, (int)kMouthDy);
    lv_obj_align_to(mouth_line_, ball_, LV_ALIGN_CENTER, 0, (int)kMouthDy);
}

void EmotionEyes::Tick() {
    t_ms_ += 50;

    // 1) 应用新表情
    if (pending_) {
        pending_ = false;
        const FaceConfig* face = FindFace(pending_emotion_);
        if (face == nullptr) {
            face = &kFaces[0];
        }
        target_ = face;
        ApplyFace(face);
    }

    const FaceConfig* f = target_;
    if (f == nullptr) {
        return;
    }

    // 2) 瞬时切换形状后，几何量向目标插值（BALL 形态，目标值按 kFaceScale 放大）
    auto lerp = [](float cur, float tgt) { return cur + (tgt - cur) * 0.25f; };
    cur_lw_ = lerp(cur_lw_, f->lw * kFaceScale);
    cur_lh_ = lerp(cur_lh_, f->lh * kFaceScale);
    cur_lrot_ = lerp(cur_lrot_, f->lrot);
    cur_rw_ = lerp(cur_rw_, f->rw * kFaceScale);
    cur_rh_ = lerp(cur_rh_, f->rh * kFaceScale);
    cur_rrot_ = lerp(cur_rrot_, f->rrot);

    // 3) 眨眼状态机（仅 BALL 形态）
    float blink_l = 1.0f, blink_r = 1.0f;
    if (f->shape == SHAPE_BALL) {
        if (blink_phase_ms_ < 0 && (int32_t)t_ms_ >= next_blink_ms_) {
            blink_phase_ms_ = 0;
        }
        if (blink_phase_ms_ >= 0) {
            blink_phase_ms_ += 50;
            float p = blink_phase_ms_ / 240.0f;
            if (p >= 1.0f) {
                blink_phase_ms_ = -1;
                next_blink_ms_ = t_ms_ + 2600 + (esp_random() % 2800);
            } else {
                float s = sinf((float)M_PI * p);
                blink_l = blink_r = 1.0f - 0.94f * s;
            }
        }
    } else {
        blink_phase_ms_ = -1;
    }

    // 4) 瞟视：待机微漂移 + 方向偏移
    float idle_x = 2.6f * sinf(2.0f * (float)M_PI * t_ms_ / 4300.0f);
    float idle_y = 1.6f * sinf(2.0f * (float)M_PI * t_ms_ / 6100.0f + 1.2f);
    float dir_x, dir_y;
    dir_offset(direction_, dir_x, dir_y);
    // thinking：目光习惯性往左上瞟
    if (strcmp(kEmotionNames[f - kFaces], "thinking") == 0) {
        dir_x -= 5; dir_y -= 5;
    }
    glance_tx_ = idle_x + dir_x;
    glance_ty_ = idle_y + dir_y;
    glance_x_ = lerp(glance_x_, glance_tx_);
    glance_y_ = lerp(glance_y_, glance_ty_);

    // silly：两眼各自乱瞟
    float silly_lx = 0, silly_rx = 0;
    if (strcmp(kEmotionNames[f - kFaces], "silly") == 0) {
        silly_lx = 3.0f * sinf(t_ms_ / 310.0f);
        silly_rx = 3.0f * cosf(t_ms_ / 430.0f);
    }

    // 5) 应用左眼几何
    int lh = (int)(cur_lh_ * blink_l);
    if (lh < 4) lh = 4;
    lv_obj_set_size(l_ball_, (int)cur_lw_, lh);
    lv_obj_set_style_transform_rotation(l_ball_, (int32_t)(cur_lrot_ * 10), 0);
    lv_obj_align_to(l_ball_, ball_, LV_ALIGN_CENTER, (int)(-kEyeDx + glance_x_ + silly_lx),
                    (int)(kEyeDy + glance_y_));
    lv_obj_align_to(r_ball_, ball_, LV_ALIGN_CENTER, (int)(kEyeDx + glance_x_ + silly_rx),
                    (int)(kEyeDy + glance_y_));
    int rh = (int)(cur_rh_ * blink_r);
    if (rh < 4) rh = 4;
    lv_obj_set_size(r_ball_, (int)cur_rw_, rh);
    lv_obj_set_style_transform_rotation(r_ball_, (int32_t)(cur_rrot_ * 10), 0);

    // 6) 其他形态跟随瞟视
    lv_obj_align_to(l_arc_, ball_, LV_ALIGN_CENTER, (int)(-kEyeDx + glance_x_ + silly_lx), (int)(kEyeDy + glance_y_));
    lv_obj_align_to(r_arc_, ball_, LV_ALIGN_CENTER, (int)(kEyeDx + glance_x_ + silly_rx), (int)(kEyeDy + glance_y_));
    lv_obj_align_to(l_line_, ball_, LV_ALIGN_CENTER, (int)(-kEyeDx + glance_x_), (int)(kEyeDy + glance_y_));
    lv_obj_align_to(r_line_, ball_, LV_ALIGN_CENTER, (int)(kEyeDx + glance_x_), (int)(kEyeDy + glance_y_));
    lv_obj_align_to(l_heart_, ball_, LV_ALIGN_CENTER, (int)(-kEyeDx + glance_x_), (int)(kEyeDy + glance_y_));
    lv_obj_align_to(r_heart_, ball_, LV_ALIGN_CENTER, (int)(kEyeDx + glance_x_), (int)(kEyeDy + glance_y_));
    lv_obj_align_to(l_glass_, ball_, LV_ALIGN_CENTER, (int)(-kEyeDx + glance_x_ * 0.4f), (int)(kEyeDy + glance_y_ * 0.4f));
    lv_obj_align_to(r_glass_, ball_, LV_ALIGN_CENTER, (int)(kEyeDx + glance_x_ * 0.4f), (int)(kEyeDy + glance_y_ * 0.4f));
    lv_obj_align_to(glass_bridge_, ball_, LV_ALIGN_CENTER, (int)(glance_x_ * 0.4f), (int)(kEyeDy + glance_y_ * 0.4f));
    lv_obj_align_to(blush_l_, ball_, LV_ALIGN_CENTER, -48 * kFaceScale, 16 * kFaceScale);
    lv_obj_align_to(blush_r_, ball_, LV_ALIGN_CENTER, 48 * kFaceScale, 16 * kFaceScale);
    lv_obj_align_to(zzz_, ball_, LV_ALIGN_CENTER, 52 * kFaceScale, -58 * kFaceScale);

    // 7) 泪滴：下落循环
    if (current_ != nullptr && (current_->extras & EX_TEAR)) {
        tear_y_ += 1.4f * kFaceScale;
        if (tear_y_ > 30 * kFaceScale) {
            tear_y_ = 0;
        }
        lv_obj_set_style_bg_opa(tear_, (lv_opa_t)(255 - tear_y_ * 7 / kFaceScale), 0);
        lv_obj_align_to(tear_, ball_, LV_ALIGN_CENTER, (int)(-kEyeDx + 2), (int)(kEyeDy + 22 * kFaceScale + tear_y_));
    }

    // 8) Z z 呼吸
    if (current_ != nullptr && (current_->extras & EX_ZZZ)) {
        lv_opa_t opa = (lv_opa_t)(150 + 80 * sinf(t_ms_ / 600.0f));
        lv_obj_set_style_text_opa(zzz_, opa, 0);
    }

    // 9) 切换表情时的小弹跳
    if (bounce_ms_ >= 0) {
        bounce_ms_ += 50;
        float p = bounce_ms_ / 280.0f;
        if (p >= 1.0f) {
            bounce_ms_ = -1;
            ball_scale_ = 1.0f;
        } else {
            ball_scale_ = 1.0f + 0.06f * sinf((float)M_PI * p);
        }
        int size = (int)(kBallSize * ball_scale_);
        lv_obj_set_size(ball_, size, size);
        lv_obj_align(ball_, LV_ALIGN_CENTER, 0, 4);
    }
}
