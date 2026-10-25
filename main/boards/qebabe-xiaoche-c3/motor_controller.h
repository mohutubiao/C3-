#ifndef _MOTOR_CONTROLLER_H_
#define _MOTOR_CONTROLLER_H_

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <cstdio>
#include <cstring>

#define MOTOR_LOG_TAG "Motor2WD"

// 2 电机版（L298N 接法：4 个 GPIO 控制 2 个电机）
//   左电机 A 路: IN1=LF, IN2=LB
//   右电机 B 路: IN3=RF, IN4=RB
// 速度控制: 0~100 % 占空比（10-bit PWM，5kHz）

enum MotorDir {
    MOTOR_STOP       = 0,
    MOTOR_FORWARD    = 1,
    MOTOR_BACKWARD   = 2,
    MOTOR_TURN_LEFT  = 3,
    MOTOR_TURN_RIGHT = 4,
};

class Motor2WD {
private:
    gpio_num_t lf_pin_;
    gpio_num_t lb_pin_;
    gpio_num_t rf_pin_;
    gpio_num_t rb_pin_;
    bool pwm_initialized_ = false;
    esp_timer_handle_t auto_stop_timer_ = nullptr;  // 异步自动停（替代 vTaskDelay）

    // ─── 异步动作序列（一次下发多段，固件内连续执行）─────────────────
    //   为什么必须异步：MCP 工具跑在 Application 主循环里（McpServer::
    //   DoToolCall → app.Schedule → main task 依次 task()），在工具回调里
    //   vTaskDelay 会把音频采集/唤醒词检测/屏幕/按键全冻住。所以工具只
    //   负责"塞进来就返回"，分步执行交给下面这个低优先级任务。
    //   段数上限 64：舞蹈模板 4 段 = 1.1 秒，跳 15 秒要 54 段；
    //   原来 12 段最多只能表达 3.3 秒的舞（"说 5 秒实际 3 秒"的根因之一）
    static constexpr int kMaxSeqSteps   = 64;      // 段数上限（LLM 侧建议 ≤24）
    static constexpr int kMaxSeqTotalMs = 15000;   // 整段总时长上限
    static constexpr int kMaxSeqStepMs  = 5000;    // 单段时长上限

    struct SeqStep { int steer; int throttle; int ms; };

    SeqStep seq_[kMaxSeqSteps] = {};
    volatile int  seq_count_   = 0;
    volatile bool seq_running_ = false;
    volatile bool seq_abort_   = false;
    TaskHandle_t  seq_task_handle_ = nullptr;

    // ─── 软启动 / 错峰 / 占空比上限（解决电机启动拉低电池导致 ESP32 复位）───
    //   300mAh 小电池 + 双 DC 电机：两个电机同时全占空比启动时堵转电流
    //   峰值能 1A+，电池压降 0.5V+，LDO 3.3V 输出跌到 2.8V → 触发 BOD 复位。
    //   三招：
    //   1. 软启动：每个通道 actual 按 d/3 趋近 target，约 175ms 0→80%
    //      （启动阶段低占空比 → 低转矩 → 电机缓加速 → 堵转电流小）
    //   2. 错峰：冷启动时右电机延迟 50ms 启动，避免两路同时拉电流
    //   3. 占空比上限：实际输出不超过 85%（留 15% 头给瞬态）
    //   副作用：响应略慢（~200ms 满速起步），但消掉 95% 复位
    // ───────────────────────────────────────────────────────────
    static constexpr int kMaxDutyPct     = 85;    // 限幅上限（SetDuty 内部裁剪）
    // tick 从 10ms 降到 5ms：冷启动 div=3 到位时间 ~70ms → ~35ms。
    //   前端节流 25ms（40Hz）配合 → 端到端首摇响应 80~100ms 内到位。
    //   5ms 仍保留每步电流爬升梯度（≈20 步），比 div=2 安全得多。
    static constexpr int kRampTickMs     = 5;
    static constexpr int kRightStaggerMs = 8;     // 冷启动时右电机延迟（从 25ms 压到 8ms）
    int lf_target_ = 0, lb_target_ = 0, rf_target_ = 0, rb_target_ = 0;
    int lf_actual_ = 0, lb_actual_ = 0, rf_actual_ = 0, rb_actual_ = 0;
    int64_t right_ramp_start_ms_ = 0;            // 0=无错峰；>0=右 ramp 在此时刻后才能动
    int ramp_div_ = 3;                           // 当前 ramp 步进除数（1=立即 2=中速 3=慢）
    // Stop() 后置位：ESP_TIMER_TASK 里可能已 dispatch 一个还没执行的 tick，
    // esp_timer_stop 取消不掉它 → 那个 tick 会把电机"复活"。
    // 有此标志后 RampTick 直接返回，杜绝松手后车还在走。
    volatile bool stopped_ = false;
    esp_timer_handle_t ramp_timer_ = nullptr;

    static void RampCb(void* arg) { static_cast<Motor2WD*>(arg)->RampTick(); }

    // 占空比裁剪到 0..kMaxDutyPct（target/actual 都走这里，保证两者口径一致）
    static int CapDuty(int v) {
        if (v < 0) v = 0;
        if (v > kMaxDutyPct) v = kMaxDutyPct;
        return v;
    }

    // 原始设置（应用 kMaxDutyPct 限幅，不走 ramp）—— ramp 内部和 Stop 用
    void SetDutyRaw(ledc_channel_t ch, int pct) {
        if (!pwm_initialized_) return;
        pct = CapDuty(pct);
        uint32_t duty = (uint32_t)pct * 1023 / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
    }

    // 把 4 路 actual 写进 PWM（错峰期间右路强制 0）
    void WriteAll(bool right_active = true) {
        SetDutyRaw(kChLF, lf_actual_);
        SetDutyRaw(kChLB, lb_actual_);
        SetDutyRaw(kChRF, right_active ? rf_actual_ : 0);
        SetDutyRaw(kChRB, right_active ? rb_actual_ : 0);
    }

    void RampTick() {
        // Stop() 已执行：哪怕这个 tick 是 stop 之前就 dispatch 出来的，也不许再写 PWM
        if (stopped_) return;
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool right_active = (right_ramp_start_ms_ == 0) || (now_ms >= right_ramp_start_ms_);

        // 一阶低通：d/ramp_div_ 步进（含 ±(div-1) 修正防整数死循环）
        //   div=1 → 一步到位；div=2 → 每帧走一半；div=3 → 每帧走 1/3
        const int div = (ramp_div_ < 1) ? 1 : ramp_div_;
        auto step = [div](int& a, int t) {
            int d = t - a;
            if (d > 0)      a += (d + div - 1) / div;
            else if (d < 0) a += (d - div + 1) / div;
            if (abs(d) <= div) a = t;
        };
        step(lf_actual_, lf_target_);
        step(lb_actual_, lb_target_);
        if (right_active) {
            step(rf_actual_, rf_target_);
            step(rb_actual_, rb_target_);
        }
        WriteAll(right_active);

        // 全部到 target → 停 timer
        if (right_active && lf_actual_ == lf_target_ && lb_actual_ == lb_target_
            && rf_actual_ == rf_target_ && rb_actual_ == rb_target_) {
            right_ramp_start_ms_ = 0;
            esp_timer_stop(ramp_timer_);
        }
    }

    void StartRampTimer() {
        if (!ramp_timer_) {
            const esp_timer_create_args_t args = {
                .callback = &Motor2WD::RampCb,
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "m2wd_ramp",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&args, &ramp_timer_) != ESP_OK) {
                ESP_LOGE(MOTOR_LOG_TAG, "ramp timer create failed");
                return;
            }
        }
        esp_timer_stop(ramp_timer_);
        esp_timer_start_periodic(ramp_timer_, kRampTickMs * 1000ULL);
    }

    // 设置 4 通道 target + 选择 ramp 速度（分级，兼顾防复位与响应）
    //   ─── 为什么分级 ──────────────────────────────────────────
    //   统一用慢 ramp 会让摇杆手感"黏"（每 25ms 才走 1/3，175ms 才到位 →
    //   用户感觉遥控延迟）。但电流尖峰只发生在"电机从静止起步"（堵转电流
    //   可达运行电流 5 倍）。所以按需选速度：
    //     · 冷启动（静止→转）  div=3 慢 ramp + 错峰  ← 唯一会复位的场景
    //     · 换向（正转↔反转）  div=2 中速             ← 反接制动电流次之
    //     · 同向调速/转向      div=1 立即到位          ← 零延迟，日常手感
    // ───────────────────────────────────────────────────────────
    void ApplyTarget(int lf, int lb, int rf, int rb) {
        lf = CapDuty(lf); lb = CapDuty(lb); rf = CapDuty(rf); rb = CapDuty(rb);

        // 方向：+1=正转通道有输出，-1=反转通道有输出，0=该侧静止
        auto dir_of = [](int fwd, int rev) { return (fwd > 0) ? 1 : ((rev > 0) ? -1 : 0); };
        bool all_stopped = (lf_actual_ == 0 && lb_actual_ == 0
                         && rf_actual_ == 0 && rb_actual_ == 0);
        bool any_move = (lf != 0 || lb != 0 || rf != 0 || rb != 0);
        bool cold = all_stopped && any_move;
        if (any_move) stopped_ = false;   // 有新动力请求 → 解除 Stop 封锁

        int dl_old = dir_of(lf_actual_, lb_actual_), dl_new = dir_of(lf, lb);
        int dr_old = dir_of(rf_actual_, rb_actual_), dr_new = dir_of(rf, rb);
        bool reversing = (dl_old != 0 && dl_new != 0 && dl_old != dl_new)
                      || (dr_old != 0 && dr_new != 0 && dr_old != dr_new);

        // 冷启动再按"目标幅度"分一档：小油门起步的堵转电流本来就小，
        //   不需要 div=3 的慢 ramp（原来无论多小的油门都要 ~70ms 才到位，
        //   微调方向时手感明显发黏）。大油门仍走慢 ramp 保护 300mAh 电池。
        int div;
        if (cold) {
            int maxd = lf;
            if (lb > maxd) maxd = lb;
            if (rf > maxd) maxd = rf;
            if (rb > maxd) maxd = rb;
            div = (maxd <= 40) ? 2 : 3;
        } else {
            div = reversing ? 2 : 1;
        }

        lf_target_ = lf; lb_target_ = lb; rf_target_ = rf; rb_target_ = rb;

        if (div == 1) {
            // 立即到位：热状态调速/转向 → 零延迟
            //   （电机已在转，改变占空比不产生堵转尖峰，安全）
            right_ramp_start_ms_ = 0;
            if (ramp_timer_) esp_timer_stop(ramp_timer_);
            lf_actual_ = lf; lb_actual_ = lb; rf_actual_ = rf; rb_actual_ = rb;
            WriteAll(true);
            return;
        }

        ramp_div_ = div;
        right_ramp_start_ms_ = cold
            ? (int64_t)(esp_timer_get_time() / 1000) + kRightStaggerMs
            : 0;   // 换向：两个电机都在转，无需错峰
        StartRampTimer();
    }

    static constexpr ledc_channel_t kChLF = LEDC_CHANNEL_0;
    static constexpr ledc_channel_t kChLB = LEDC_CHANNEL_1;
    static constexpr ledc_channel_t kChRF = LEDC_CHANNEL_2;
    static constexpr ledc_channel_t kChRB = LEDC_CHANNEL_3;

public:
    Motor2WD(gpio_num_t lf, gpio_num_t lb, gpio_num_t rf, gpio_num_t rb)
        : lf_pin_(lf), lb_pin_(lb), rf_pin_(rf), rb_pin_(rb) {
        // 先用 GPIO 拉低，确保上电时电机不窜
        // 启用内部下拉（原为 DISABLE）
        //   ─── 为什么 ──────────────────────────────────────────
        //   IN1(GPIO2) 偶尔被残压撑在高电平 → 左轮自转，实测用镊子把 GPIO2
        //   短接到 GND 就立刻停 → 说明这条线缺一个确定的低电平基准。
        //   内部下拉约 45kΩ，能让 MCU 运行期间、以及 PWM 接管前的那段窗口
        //   把脚稳稳拽在低电平，减少"抖一下/一直转"的概率。
        //   ⚠️ 根治仍要靠 GPIO2→GND 的 10k 外部下拉电阻：断电后 MCU 没电，
        //      内部下拉会失效，只有物理电阻才顶得住。
        //   ⚠️ 安全性：gpio_config() 在 MCU 启动完成后才执行，此时 strapping
        //      早已采样完毕，启用下拉不会影响 GPIO2 的启动模式判定。
        gpio_config_t io = {};
        io.mode = GPIO_MODE_OUTPUT;
        io.pin_bit_mask = (1ULL << lf) | (1ULL << lb) | (1ULL << rf) | (1ULL << rb);
        io.pull_down_en = GPIO_PULLDOWN_ENABLE;
        io.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io);
        gpio_set_level(lf, 0);
        gpio_set_level(lb, 0);
        gpio_set_level(rf, 0);
        gpio_set_level(rb, 0);
    }

    // PWM 初始化（由 Application 启动后调一次）
    bool InitPWM() {
        if (pwm_initialized_) return true;

        ledc_timer_config_t tcfg = {};
        tcfg.speed_mode = LEDC_LOW_SPEED_MODE;
        tcfg.duty_resolution = LEDC_TIMER_10_BIT;
        tcfg.timer_num = LEDC_TIMER_1;
        tcfg.freq_hz = 5000;
        tcfg.clk_cfg = LEDC_AUTO_CLK;
        if (ledc_timer_config(&tcfg) != ESP_OK) {
            ESP_LOGE(MOTOR_LOG_TAG, "PWM timer init failed");
            return false;
        }

        ledc_channel_config_t ccfg[4] = {};
        gpio_num_t pins[4] = { lf_pin_, lb_pin_, rf_pin_, rb_pin_ };
        ledc_channel_t chs[4] = { kChLF, kChLB, kChRF, kChRB };
        for (int i = 0; i < 4; i++) {
            ccfg[i].gpio_num = pins[i];
            ccfg[i].speed_mode = LEDC_LOW_SPEED_MODE;
            ccfg[i].channel = chs[i];
            ccfg[i].timer_sel = LEDC_TIMER_1;
            ccfg[i].duty = 0;
            ccfg[i].hpoint = 0;
            if (ledc_channel_config(&ccfg[i]) != ESP_OK) {
                ESP_LOGE(MOTOR_LOG_TAG, "PWM ch%d init failed", i);
                return false;
            }
        }
        pwm_initialized_ = true;
        ESP_LOGI(MOTOR_LOG_TAG, "PWM ready (5kHz, 10-bit)");
        return true;
    }

    // 纯断电，不碰序列状态 —— 只给「自动停 timer 到期」和序列内部收尾用
    //   ⚠️ 序列每步都会 ArmAutoStop，到期若走 Stop() 会被判成「外部急停」
    //      而把整段序列掐死（表现：序列只跑第一步）。所以必须有这一层。
    void ApplyStop() {
        if (!pwm_initialized_) InitPWM();
        CancelAutoStop();  // 手动停也取消 pending 自动停
        // 立即断电（不走 ramp）——松手/急停/复位场景要"瞬间"停，
        // ramp 软停在急停时反而拖泥带水（电机堵转电流小但车会滑行）
        lf_target_ = lb_target_ = rf_target_ = rb_target_ = 0;
        lf_actual_ = lb_actual_ = rf_actual_ = rb_actual_ = 0;
        right_ramp_start_ms_ = 0;
        stopped_ = true;                              // 挡住已 dispatch 的迟到 tick
        if (ramp_timer_) esp_timer_stop(ramp_timer_);
        SetDutyRaw(kChLF, 0);
        SetDutyRaw(kChLB, 0);
        SetDutyRaw(kChRF, 0);
        SetDutyRaw(kChRB, 0);
    }

    // 对外急停：断电 + 中止正在跑的序列（网页停车 / MCP stop / BOOT 打断都走这里）
    void Stop() {
        ApplyStop();
        if (seq_running_ && !InSeqTask()) {
            seq_abort_ = true;
            if (seq_task_handle_) xTaskNotifyGive(seq_task_handle_);
        }
    }

    // 差速转向（核心新接口）—— 摇杆 steer/throttle -100..+100
    //   steer > 0 = 右转（左侧电机降速，弧线；steer=±100 原地转）
    //   steer < 0 = 左转（右侧电机降速）
    //   throttle > 0 = 前进，< 0 = 后退
    //   死区 5%（灵敏但滤抖）
    //   差速曲线：steer=±100 减到 0（原地转）；steer=±50 减到 50%
    //   接线修正（运行时可调，网页上勾选即可生效）
    //   invert_left_/invert_right_：该侧电机正反转反了
    //   swap_lr_：左右电机对调（物理位置装反或接线交叉）
    bool invert_left_  = false;   // 左电机方向反了 → 设为 true
    bool invert_right_ = false;   // 右电机方向反了 → 设为 true
    bool swap_lr_      = false;   // 左右对调 → 设为 true

    void SetInvertLeft(bool v)  { invert_left_  = v; }
    void SetInvertRight(bool v) { invert_right_ = v; }
    void SetSwapLr(bool v)      { swap_lr_      = v; }
    bool GetInvertLeft()  const { return invert_left_;  }
    bool GetInvertRight() const { return invert_right_; }
    bool GetSwapLr()      const { return swap_lr_;      }

    // ─── 异步自动停（替代旧 vTaskDelay 方案）──────────────────────────
    //   旧方案 Drive(s,t,duration) 里 vTaskDelay(duration) 直接阻塞调用任务，
    //   WS act 按钮（800ms）一阻塞 httpd 任务 → joy/stop 全部排队 → 控制延迟。
    //   新方案：esp_timer one-shot，到期在 timer task 里调 Stop()，零阻塞。
    //   任何新的 Drive()/Stop() 都会取消 pending 的自动停。
    //   （ledc_set_duty 线程安全；多任务并发 Drive 竞态可接受——最坏情况
    //    后一条命令覆盖前一条，本来就是"最后一条说了算"的语义）
    // ─────────────────────────────────────────────────────────────────
    // ⚠️ 只调 ApplyStop：见 ApplyStop 上方注释（调 Stop 会掐死序列）
    static void AutoStopCb(void* arg) { static_cast<Motor2WD*>(arg)->ApplyStop(); }

    void CancelAutoStop() {
        if (auto_stop_timer_) esp_timer_stop(auto_stop_timer_);
    }

    void ArmAutoStop(uint32_t duration_ms) {
        if (!auto_stop_timer_) {
            const esp_timer_create_args_t args = {
                .callback = &Motor2WD::AutoStopCb,
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "m2wd_stop",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&args, &auto_stop_timer_) != ESP_OK) {
                ESP_LOGE(MOTOR_LOG_TAG, "auto-stop timer create failed");
                return;
            }
        }
        esp_timer_stop(auto_stop_timer_);                       // 重臂（忽略未激活错误）
        esp_timer_start_once(auto_stop_timer_, duration_ms * 1000ULL);
    }

    // expo 曲线：小舵柔、大舵猛（40% expo，遥控标准手感）
    //   x=50 → 34（柔），x=80 → 69，x=100 → 100（满）
    static int Expo(int v) {
        int a = (v < 0) ? -v : v;
        if (a > 100) a = 100;
        int a3 = a * a / 100 * a / 100;              // a³/10000（0..100）
        int e = (a3 * 40 + a * 60) / 100;            // 40% 三次曲线 + 60% 线性
        return (v < 0) ? -e : e;
    }

    void Drive(int steer, int throttle) {
        if (!pwm_initialized_) InitPWM();

        // 外部新指令 → 中断正在跑的序列（序列任务自己调时不中断，否则自我打断）
        if (!InSeqTask()) AbortSequence();

        // 新的直接命令 → 取消上一条 pending 的自动停
        CancelAutoStop();

        // 限幅到 -100..+100
        if (steer    >  100) steer    =  100;
        if (steer    < -100) steer    = -100;
        if (throttle >  100) throttle =  100;
        if (throttle < -100) throttle = -100;

        // 死区
        if (throttle > -3 && throttle < 3) throttle = 0;
        if (steer    > -5 && steer   <  5) steer    = 0;
        if (throttle == 0 && steer == 0) { Stop(); return; }

        // ─── 差速 v2（expo + 等比缩放）───────────────────────────────
        //   1. steer 过 expo 曲线（小舵柔、大舵猛）
        //   2. v_l = t + s, v_r = t - s
        //   3. 若超 ±100 → 双轮等比缩放（保差速比，不硬砍单边）
        //      旧版硬 clamp 会把内侧轮压成 0/外侧压满 → 斜推时转弯
        //      差异被吃掉，"差速效果很差"；等比缩放后转弯始终成比例
        //   - 斜推前左 (t=70,s=-50) → v_l=26, v_r=103→等比 25/100
        //     （外侧满速内侧 1/4 → 明显弧线，而不是旧版 20/100 之类被打平）
        //   - 满舵 (t=0,s=±100) → 双轮 ±100 反向 → 原地转
        int s = Expo(steer);
        int v_l = throttle + s;
        int v_r = throttle - s;
        int m = (v_l < 0 ? -v_l : v_l);
        int mr = (v_r < 0 ? -v_r : v_r);
        if (mr > m) m = mr;
        if (m > 100) {
            v_l = v_l * 100 / m;
            v_r = v_r * 100 / m;
        }

        // swap_lr：交换左右 → v 互换 + invert 互换（函数末尾恢复）
        if (swap_lr_) {
            std::swap(v_l, v_r);
            std::swap(invert_left_, invert_right_);
        }

        // 展开到四路开关（LF/LB/RF/RB）：正值走前进通道，负值走后退通道
        int lf_duty = (v_l > 0) ?  v_l : 0;
        int lb_duty = (v_l < 0) ? -v_l : 0;
        int rf_duty = (v_r > 0) ?  v_r : 0;
        int rb_duty = (v_r < 0) ? -v_r : 0;
        if (invert_left_)  { std::swap(lf_duty, lb_duty); }
        if (invert_right_) { std::swap(rf_duty, rb_duty); }

        // ⚠️ 这里不能再 swap 一次！swap_lr_ 已在上面第 347 行通过
        //    std::swap(v_l, v_r) 完成了左右互换（连同 invert 一起交换）。
        //    若在此处再 std::swap(lf_duty, rf_duty) 会二次抵消 →
        //    纯前进算出「左轮前进 + 右轮后退」→ 车原地扭（表现为前后哆嗦）。
        //    左右互换只需做一次：要么换 v_l/v_r，要么换 lf/rf，二者等价。
        // 走 ramp 软启动（限幅+错峰+分级 div 在 ApplyTarget 内部）
        ApplyTarget(lf_duty, lb_duty, rf_duty, rb_duty);
        if (swap_lr_) {
            std::swap(invert_left_, invert_right_);  // 恢复 invert 状态
        }
    }

    // 直接给 4 路 raw 值（用 ramp 软启动 + 上限 + 错峰）
    //   主要给 Forward/Backward/TurnLeft/TurnRight 等单动作接口用
    void DriveRamped(int lf, int lb, int rf, int rb) {
        if (swap_lr_) {
            std::swap(lf, rf);
            std::swap(lb, rb);
            std::swap(invert_left_, invert_right_);
        }
        if (invert_left_)  std::swap(lf, lb);
        if (invert_right_) std::swap(rf, rb);
        ApplyTarget(lf, lb, rf, rb);
        if (swap_lr_) {
            std::swap(invert_left_, invert_right_);
        }
    }

    void Forward(uint8_t pct = 100) {
        if (!pwm_initialized_) InitPWM();
        DriveRamped(pct, 0, pct, 0);
    }

    void Backward(uint8_t pct = 100) {
        if (!pwm_initialized_) InitPWM();
        DriveRamped(0, pct, 0, pct);
    }

    void TurnLeft(uint8_t pct = 100) {
        if (!pwm_initialized_) InitPWM();
        // 左轮后退、右轮前进 = 左转
        DriveRamped(0, pct, pct, 0);
    }

    void TurnRight(uint8_t pct = 100) {
        if (!pwm_initialized_) InitPWM();
        DriveRamped(pct, 0, 0, pct);
    }

    // 差速转向（带 duration）—— 摇杆/旧 API 共用
    //   duration_ms > 0：执行该时长后自动停（esp_timer 异步，不阻塞调用任务！）
    //   duration_ms = 0：持续执行（由外部 Stop / 下一条 Drive 取代）
    void Drive(int steer, int throttle, uint32_t duration_ms) {
        Drive(steer, throttle);  // 内部已 CancelAutoStop
        if (duration_ms > 0) ArmAutoStop(duration_ms);
    }

    // 简单动作：执行一段时间后自动停（用 mixer Drive 实现）
    //   左右转用 throttle=0 + 满舵 → 双轮反向原地转（YYXC 风格），
    //   不再是"内侧减到 0"的单轮转
    void Execute(MotorDir dir, uint8_t pct, uint32_t duration_ms) {
        int steer = 0, throttle = (int)pct;
        switch (dir) {
            case MOTOR_FORWARD:    steer = 0;    throttle = pct;  break;
            case MOTOR_BACKWARD:   steer = 0;    throttle = -pct; break;
            case MOTOR_TURN_LEFT:  steer = -100; throttle = 0;    break;  // 双轮反向原地左转
            case MOTOR_TURN_RIGHT: steer =  100; throttle = 0;    break;  // 双轮反向原地右转
            default:               Stop(); return;
        }
        Drive(steer, throttle, duration_ms);
    }

    // ═══ 异步动作序列 ═══════════════════════════════════════════════════
    //   一次解析 "steer,throttle,ms;..." 后交给独立低优先级任务连续执行，
    //   调用方（MCP 工具回调，跑在 Application 主循环里）立即返回。
    //
    //   Why 不能同步：MCP 工具经 app.Schedule() 在 main task 的事件循环里
    //   顺序执行，回调里 vTaskDelay 会冻住音频采集/唤醒词/屏幕/按键；
    //   而靠 LLM 连调多次 drive 又会被 WS 往返延迟打断成一走一停。
    //   序列把多段动作压到一次调用、固件内定时，段间 0 网络延迟。
    //
    //   steer/throttle 走的是和摇杆同一个 Drive() 混控，所以弧线、
    //   原地转、边走边转都能表达，不局限于 21 个预设动作名。
    // ─────────────────────────────────────────────────────────────────

    // 当前是否跑在序列任务里（用于区分"序列自己的指令"和"外部新指令"）
    bool InSeqTask() const {
        return seq_task_handle_ != nullptr &&
               xTaskGetCurrentTaskHandle() == seq_task_handle_;
    }

    uint32_t SequenceTotalMs() const {
        uint32_t total = 0;
        for (int i = 0; i < seq_count_; ++i) total += (uint32_t)seq_[i].ms;
        return total;
    }

    bool SequenceRunning() const { return seq_running_; }

    static void SequenceTaskEntry(void* arg) {
        auto self = static_cast<Motor2WD*>(arg);
        for (;;) {
            // 等启动信号（portMAX_DELAY 不耗 CPU）
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (self->seq_count_ <= 0) continue;

            ESP_LOGI(MOTOR_LOG_TAG, "seq start: %d steps, %u ms",
                     (int)self->seq_count_, (unsigned)self->SequenceTotalMs());
            for (int i = 0; i < self->seq_count_; ++i) {
                if (self->seq_abort_) break;
                const SeqStep& st = self->seq_[i];
                self->Drive(st.steer, st.throttle, (uint32_t)st.ms);
                // 带超时的等待：正常等本段跑完；Abort 时被 notify 立刻唤醒
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(st.ms + 10));
            }
            bool aborted = self->seq_abort_;
            self->seq_running_ = false;
            self->seq_abort_ = false;
            self->seq_count_ = 0;
            self->ApplyStop();   // ⚠️ 不能调 Stop()，否则又走一遍中止逻辑
            ESP_LOGI(MOTOR_LOG_TAG, "seq %s", aborted ? "aborted" : "done");
        }
    }

    // 解析并启动序列。返回整段总时长 ms；0 = 失败（格式错/超限/正忙）
    //   limit_pct：全局速度上限（网页「🐢 速度」滑条），等比缩放 steer/throttle
    uint32_t PlaySequence(const char* steps, int limit_pct = 100) {
        if (!steps || !*steps) return 0;
        // 上一段还在跑 → 覆盖它（"最后一句说了算"）。
        //   ⚠️ 原来这里是直接 return 0（拒绝）：LLM 想把 1.1 秒的 dance 连调
        //   几次凑够 5 秒时，第二次起全被丢弃，用户看到的永远是 1.1 秒。
        if (seq_running_) {
            ESP_LOGI(MOTOR_LOG_TAG, "seq running, overriding");
            AbortSequence();
            // 等旧任务真正退出（被 notify 后几 ms 就跳出循环并清 seq_running_）。
            // 上限 100ms：主循环不能被这里困住太久。
            for (int i = 0; i < 50 && seq_running_; ++i) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            if (seq_running_) {
                ESP_LOGW(MOTOR_LOG_TAG, "seq abort timeout, ignored");
                return 0;
            }
        }
        if (!pwm_initialized_) InitPWM();

        if (limit_pct < 0)   limit_pct = 0;
        if (limit_pct > 100) limit_pct = 100;

        int count = 0;
        uint32_t total = 0;
        const char* p = steps;
        while (*p && count < kMaxSeqSteps) {
            int s = 0, t = 0, ms = 0;
            if (sscanf(p, "%d,%d,%d", &s, &t, &ms) != 3) {
                ESP_LOGE(MOTOR_LOG_TAG, "seq parse error near: %.24s", p);
                return 0;
            }
            if (ms < 100) ms = 100;
            if (ms > kMaxSeqStepMs) ms = kMaxSeqStepMs;
            if (s < -100) s = -100;
            if (s >  100) s =  100;
            if (t < -100) t = -100;
            if (t >  100) t =  100;
            // 全局限速：steer/throttle 同比例缩 → 差速比不变，
            // throttle=0 的原地转仍是双轮反向（只是变慢），不会被缩成单轮转
            if (limit_pct < 100) {
                s = s * limit_pct / 100;
                t = t * limit_pct / 100;
            }

            total += (uint32_t)ms;
            if (total > (uint32_t)kMaxSeqTotalMs) {
                ESP_LOGW(MOTOR_LOG_TAG, "seq too long (%u ms), rejected", (unsigned)total);
                return 0;
            }
            seq_[count].steer = s;
            seq_[count].throttle = t;
            seq_[count].ms = ms;
            count++;

            const char* sep = strchr(p, ';');
            if (!sep) break;
            p = sep + 1;
        }
        if (count == 0) return 0;

        if (seq_task_handle_ == nullptr) {
            if (xTaskCreate(SequenceTaskEntry, "m2wd_seq", 4096, this, 5,
                            &seq_task_handle_) != pdPASS) {
                ESP_LOGE(MOTOR_LOG_TAG, "seq task create failed");
                seq_task_handle_ = nullptr;
                return 0;
            }
        }

        seq_count_   = count;     // 先写数据，再置标志，最后唤醒
        seq_abort_   = false;
        seq_running_ = true;
        xTaskNotifyGive(seq_task_handle_);
        ESP_LOGI(MOTOR_LOG_TAG, "seq queued: %d steps, %u ms",
                 count, (unsigned)total);
        return total;
    }

    // 中止序列：立即断电并唤醒任务退出（不阻塞调用方）
    void AbortSequence() {
        if (!seq_running_) return;
        seq_abort_ = true;
        CancelAutoStop();
        ApplyStop();
        if (seq_task_handle_) xTaskNotifyGive(seq_task_handle_);
    }
};

#endif // _MOTOR_CONTROLLER_H_
