#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <esp_http_server.h>
#include <string>
#include <functional>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class WebServer {
public:
    WebServer();
    ~WebServer();

    // httpd 自身始终运行（这样页面随时可访问、显示时间和状态文字）
    bool Start(int port = 80);
    void Stop();

    // ───────────────────────────────────────────────────────────────
    // 5 分钟控制窗口
    //   - 由 Application::HandleStateChangedEvent 在 state→listening
    //     （即"唤醒词触发"）时调用 StartSession()，开 5 分钟窗口
    //   - 窗口内 WebSocket 推状态、/api/control /api/motor/action 可用
    //   - 窗口外这些 API 返回 503、页面 UI 灰显
    //   - 5 分钟到点后由 session_timer_task 自动 EndSession()
    //   - 重复 StartSession() 会续期到新的 5 分钟
    // ───────────────────────────────────────────────────────────────
    void StartSession();
    void EndSession();
    bool IsSessionActive() const;
    int64_t GetSessionEndMs() const;  // esp_timer 毫秒；0 表示未激活

    // ───────────────────────────────────────────────────────────────
    // 控制模式（按用户要求：进入控制页面后彻底禁用对话）
    //   - 绕过 IsSessionActive() 检查，让 act/joy/stop 消息全部直达电机
    //   - 5 分钟无活动自动退出（由 control_mode_timer_task 触发 EndControlMode）
    //   - EndSession() 也会顺手 EndControlMode
    //   - 通过 {t:"control_enter"} / {t:"control_exit"} WS 消息切换
    // ───────────────────────────────────────────────────────────────
    void StartControlMode();
    void EndControlMode();
    bool IsControlMode() const { return control_mode_; }
    int64_t GetControlModeEndMs() const { return control_mode_end_ms_; }
    // 控制模式期间每次收到有效消息调用 → 续期 idle 倒计时
    void TouchControlMode();

    // 遥控时自动表情：前进 excited / 后退 confused / 左转 loving / 右转 surprised
    //   状态没变不刷屏（防摇杆边界抖动），停车保持当前表情
    void AutoEmotionForDrive(int steer, int throttle);

    // 推送 state JSON 给所有 WebSocket 客户端
    //   json_str 必须以 '\0' 结尾；此函数内部异步发送、不阻塞调用者
    void BroadcastStateJson(const char* json_str);

    // 缓存最近一次 state JSON，用于页面首次加载时（避免白屏）
    const char* GetLastStateJson() const;

    // 注册电机控制回调函数 - 使用 std::function 支持 lambda
    void SetMotorControlCallback(std::function<void(int direction, int speed)> callback);
    // 由外部调用以触发电机控制（封装私有回调）
    void InvokeMotorControl(int direction, int speed);

    // 差速转向回调（C3 等双电机板专用；与 SetMotorControlCallback 共存）
    //   steer:    -100..+100（左/右；+100 = 原地右转，-100 = 原地左转）
    //   throttle: -100..+100（前/后；+100 = 前进满速，-100 = 后退满速）
    void SetSteerCallback(std::function<void(int steer, int throttle)> callback);
    // 由 web handler 调用以触发差速控制
    void InvokeSteer(int steer, int throttle);

    // 切换对话（与板载 BOOT 键同语义）：
    //   空闲/待机 → 唤醒进入聆听；说话中 → 打断（AbortSpeaking）；
    //   聆听中 → 关闭聆听回 idle。由 web handler 调用，封装弱符号。
    void InvokeToggleChat();

    // 注册表情设置回调函数
    void SetEmotionCallback(std::function<void(const char* emotion)> callback);
    // 设置表情（封装私有回调）
    void SetEmotion(const char* emotion);

    // 电机动作配置回调
    struct MotorActionConfig {
        int forward_duration_ms = 5000;
        int backward_duration_ms = 5000;
        int left_turn_duration_ms = 600;
        int right_turn_duration_ms = 600;
        int spin_duration_ms = 2500;
        int wiggle_duration_ms = 600;
        int dance_duration_ms = 1500;
        int quick_forward_duration_ms = 5000;
        int quick_backward_duration_ms = 5000;
        int default_speed_percent = 100;
        bool swap_lr = false;       // 左右电机对调
        bool invert_left = false;   // 左电机反向
        bool invert_right = false;  // 右电机反向
    };

    void SetMotorActionConfigCallback(std::function<MotorActionConfig()> get_callback,
                                     std::function<void(const MotorActionConfig&)> set_callback);

    // Debug handler 注册（/api/debug/motor_test）
    static esp_err_t debug_motor_test_handler(httpd_req_t *req);

    // 全局速度上限（/api/max_speed）：GET 读 / POST 写，写 NVS 断电保持
    static esp_err_t api_max_speed_handler(httpd_req_t *req);

    // ── 新控制页专用（这台设备只有屏幕 + 语音，没有电机）─────────────────
    //   /api/display_mode  屏幕「文字 / 表情」切换：GET 读当前、POST 写入
    //   /api/info          设备信息（IP、当前显示模式），页面加载时拉一次
    static esp_err_t api_display_mode_handler(httpd_req_t *req);
    static esp_err_t api_info_handler(httpd_req_t *req);
    //   /api/theme  待机主题：动态星球 / 静态时钟（GET 读、POST 写）
    static esp_err_t api_theme_handler(httpd_req_t *req);

    // 5 分钟会话时长（毫秒）—— 公开便于测试或菜单调整
    static constexpr uint32_t kSessionDurationMs = 5 * 60 * 1000;

private:
    httpd_handle_t server_handle_;
    std::function<void(int direction, int speed)> motor_control_callback_;
    std::function<void(int steer, int throttle)> steer_callback_;
    std::function<void(const char* emotion)> emotion_callback_;
    std::function<MotorActionConfig()> get_motor_config_callback_;
    std::function<void(const MotorActionConfig&)> set_motor_config_callback_;

    // HTTP请求处理函数
    static esp_err_t index_get_handler(httpd_req_t *req);
    static esp_err_t control_post_handler(httpd_req_t *req);
    static esp_err_t api_control_handler(httpd_req_t *req);
    static esp_err_t api_motor_action_handler(httpd_req_t *req);
    static esp_err_t api_motor_calib_get_handler(httpd_req_t *req);
    static esp_err_t config_get_handler(httpd_req_t *req);
    static esp_err_t config_post_handler(httpd_req_t *req);
    static esp_err_t api_config_get_handler(httpd_req_t *req);
    static esp_err_t api_config_post_handler(httpd_req_t *req);

    // WebSocket 处理器
    static esp_err_t ws_handler(httpd_req_t *req);

    // 获取HTML页面内容
    // 新控制页（这台设备没有电机）：状态 + 屏幕显示模式 + 设备信息
    static const char* get_control_html_page();

    // 解析控制命令
    //   注：原 parse_json_control_command() 无任何调用点（表单走 parse_config_form_data、
    //   控制指令走 parse_simple_control_command / WS 极简解析），已删除。
    bool parse_simple_control_command(const char* data, int& direction, int& speed,
                                     int* steer_out = nullptr, int* throttle_out = nullptr);

    // 配置相关方法
    void parse_config_form_data(const char* data, MotorActionConfig& config);
    static const char* get_config_html_page();

    // ─── 会话状态 ───
    bool session_active_ = false;
    int64_t session_end_ms_ = 0;  // esp_timer 毫秒；用于广播真实结束时间
    TaskHandle_t session_timer_handle_ = nullptr;
    SemaphoreHandle_t session_mutex_ = nullptr;  // 保护 session_active_ + 续期

    // ─── 控制模式状态（独立于 session，但 EndSession 会一并清掉）───
    bool control_mode_ = false;
    int64_t control_mode_end_ms_ = 0;       // esp_timer 毫秒；0 表示未激活
    int64_t control_mode_last_msg_ms_ = 0;  // 最近一次 WS 消息时间戳（每次收包续期）
    TaskHandle_t control_mode_timer_handle_ = nullptr;

    // ─── 遥控自动表情 ───
    char last_auto_emotion_[16] = {0};

    // ─── WebSocket 客户端列表 ───
    struct WsClient {
        int fd;
    };
    std::vector<WsClient> ws_clients_;
    SemaphoreHandle_t ws_clients_mutex_ = nullptr;

    // 最近一次状态 JSON 缓存（页面打开时用最新值渲染）
    mutable SemaphoreHandle_t state_cache_mutex_ = nullptr;
    char last_state_json_[384] = {0};

    // 内部：会话控制
    static void session_timer_task(void* arg);
    void CancelSessionTimer();
    void StartSessionTimerLocked();

    // 内部：控制模式
    static void control_mode_timer_task(void* arg);
    void CancelControlModeTimer();
    void StartControlModeTimerLocked();
    void SetControlModeEnabled(bool enabled);  // 状态变更 + 通知应用层（通过弱符号）

    // 内部：WebSocket 客户端
    void AddWsClient(int fd);
    void RemoveWsClient(int fd);
    void SendToAllWsClients(const char* json, size_t len);

    // 内部：辅助 handler（检查会话是否激活，不在就 503）
    static esp_err_t check_session_active(httpd_req_t *req, bool* is_active);
};

#endif // WEB_SERVER_H
