#include "web_server.h"
#include "board.h"
#include "display.h"
#include <esp_log.h>
#include <cstring>
#include <cJSON.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>

// Forward declarations for motor control functions
extern "C" void HandleMotorActionForApplication(int direction, int speed, int duration_ms, int priority);
extern "C" void HandleMotorActionForEmotion(const char* emotion);
extern "C" void HandleMotorActionForDance(uint8_t speed_percent);
// 全局速度上限（网页「🐢 速度」滑条）：NVS 持久化，断电保持；作用于语音 / MCP 动作
extern "C" void SetGlobalMaxSpeedPct(int pct);
extern "C" int  GetGlobalMaxSpeedPct();
// 屏幕「文字 / 表情」显示模式。
//   实现在板级（xmini_c3_board.cc）—— web_server 这里不直接依赖 oled_display.h，
//   沿用上面的弱符号风格：板子没接屏幕时这两个接口直接返回 501。
extern "C" void SetChatDisplayMode(bool text_mode) __attribute__((weak));
extern "C" bool IsChatTextMode() __attribute__((weak));
// 待机壁纸（0=静态时钟 1=动态星球 2=星空），同样是板级弱符号
extern "C" void SetWallpaper(int wp) __attribute__((weak));
extern "C" int  GetWallpaper() __attribute__((weak));

// ──────────────────────────────────────────────────────────────────────
//  /api/display_mode —— 屏幕「文字 / 表情」切换
//    GET  → {"text":false}
//    POST → body {"text":true}，切完返回新状态
//   实现是板级弱符号；板子没接屏幕时返回 501。
// ──────────────────────────────────────────────────────────────────────
esp_err_t WebServer::api_display_mode_handler(httpd_req_t *req) {
    if (!SetChatDisplayMode || !IsChatTextMode) {
        httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED, "no display");
        return ESP_OK;
    }
    bool text = IsChatTextMode();

    if (req->method == HTTP_POST) {
        char body[64] = {0};
        int ret = httpd_req_recv(req, body, sizeof(body) - 1);
        if (ret > 0) {
            body[ret] = '\0';
            // 极简解析：找 "text" 后面的 true / 1（body 就几个字节，不值得上 cJSON）
            const char* p = strstr(body, "text");
            if (p) {
                const char* q = strchr(p, ':');
                if (q) text = (strstr(q, "true") != nullptr) || (strstr(q, "1") != nullptr);
            }
        }
        SetChatDisplayMode(text);
        // ⚠️ 用字面量而不是 TAG：TAG 定义在本文件更后面（第 145 行），
        //    这两个 handler 在它之前，TAG 还没进作用域。
        ESP_LOGI("WebServer", "显示模式 → %s", text ? "文字" : "表情");
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "{\"text\":%s}", text ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ──────────────────────────────────────────────────────────────────────
//  /api/info —— 设备信息（IP + 当前显示模式）
//   新控制页加载时拉一次。不涉及任何电机。
// ──────────────────────────────────────────────────────────────────────
esp_err_t WebServer::api_info_handler(httpd_req_t *req) {
    char ip[16] = "---";
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ip\":\"%s\",\"text\":%s,\"wallpaper\":%d}",
             ip,
             (IsChatTextMode && IsChatTextMode()) ? "true" : "false",
             (GetWallpaper ? GetWallpaper() : 0));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ──────────────────────────────────────────────────────────────────────
//  /api/theme —— 待机主题：动态星球 / 静态时钟
//    GET  → {"planet":false}
//    POST → body {"planet":true}，切完返回新状态
//   动态星球每帧重绘（20fps）看着酷但费电；静态时钟 1 次/秒省电。
// ──────────────────────────────────────────────────────────────────────
esp_err_t WebServer::api_theme_handler(httpd_req_t *req) {
    if (!SetWallpaper || !GetWallpaper) {
        httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED, "no display");
        return ESP_OK;
    }
    int wp = GetWallpaper();

    if (req->method == HTTP_POST) {
        char body[64] = {0};
        int ret = httpd_req_recv(req, body, sizeof(body) - 1);
        if (ret > 0) {
            body[ret] = '\0';
            // 找 "wallpaper" 后面的数字（body 就几个字节，不值得上 cJSON）
            const char* p = strstr(body, "wallpaper");
            if (p) {
                const char* q = strchr(p, ':');
                if (q) {
                    while (*q && (*q < '0' || *q > '9')) q++;
                    if (*q) {
                        int v = atoi(q);
                        if (v >= 0 && v <= 2) wp = v;
                    }
                }
            }
        }
        SetWallpaper(wp);
        // ⚠️ 同样要列满 6 个，理由见 xmini_c3_board.cc 的注释（越界会崩）
        static const char* kName[] = {"时钟", "星球", "星空", "星际穿越", "指针时钟", "数字雨"};
        ESP_LOGI("WebServer", "待机壁纸 → %s", kName[wp]);
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "{\"wallpaper\":%d}", wp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ──────────────────────────────────────────────────────────────────────
//  /api/max_speed
//    GET  → 返回当前全局速度上限 {"pct":80}
//    POST → 设置（body: {"pct":80} 或纯数字），写入 NVS，断电保持
//   原来「🐢 速度」滑条的值只存在浏览器 localStorage（纯前端乘数），
//   既不下发给设备、也管不到语音动作；现在设备端也存一份，
//   MCP / 语音动作下发前会乘这个系数。
// ──────────────────────────────────────────────────────────────────────
esp_err_t WebServer::api_max_speed_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        char buf[48];
        snprintf(buf, sizeof(buf), "{\"pct\":%d}", GetGlobalMaxSpeedPct());
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char content[128];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    int pct = -1;
    cJSON* root = cJSON_Parse(content);
    if (root) {
        cJSON* item = cJSON_GetObjectItem(root, "pct");
        if (cJSON_IsNumber(item)) pct = item->valueint;
        cJSON_Delete(root);
    }
    if (pct < 0) pct = atoi(content);      // 兼容纯数字 body
    if (pct < 30)  pct = 30;
    if (pct > 100) pct = 100;

    SetGlobalMaxSpeedPct(pct);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// 弱符号（板子实现，web 层调用）—— 必须 extern "C" 且放在命名空间作用域：
//   board (compact_wifi_board.cc) 里的定义都是 extern "C"，
//   函数体内写 extern "C" 不符合 C++ 标准，且不带 extern "C" 的声明
//   会因 C++ name mangling 对不上链接符号 → 弱符号永远解析为 null → 功能失效
extern "C" {
void HandleBoardSteerControl(int steer, int throttle) __attribute__((weak));
bool GetMotorSwapLr() __attribute__((weak));
bool GetMotorInvertLeft() __attribute__((weak));
bool GetMotorInvertRight() __attribute__((weak));
void OnControlModeChange(bool enable) __attribute__((weak));
void SetMotorInvertLeft(bool v) __attribute__((weak));
void SetMotorInvertRight(bool v) __attribute__((weak));
void SetMotorSwapLr(bool v) __attribute__((weak));
void OnMotorCalibChanged(bool swap_lr, bool inv_l, bool inv_r) __attribute__((weak));
void HandleMotorExecuteByEnum(int motor_action, int speed, int duration_ms) __attribute__((weak));
// 切换对话（与板载 BOOT 键同语义：唤醒/打断/退出聆听）
//   由 web 端 {t:"interrupt"} 消息触发，封装到 Application::ToggleChatState()
//   走 MAIN_EVENT_TOGGLE_CHAT 事件组异步处理，不阻塞 httpd 任务
void HandleBoardToggleChat() __attribute__((weak));
}

static const char *TAG = "WebServer";

WebServer::WebServer()
    : server_handle_(nullptr),
      session_mutex_(xSemaphoreCreateMutex()),
      ws_clients_mutex_(xSemaphoreCreateMutex()),
      state_cache_mutex_(xSemaphoreCreateMutex()) {
}

WebServer::~WebServer() {
    Stop();
    if (session_mutex_)     vSemaphoreDelete(session_mutex_);
    if (ws_clients_mutex_)  vSemaphoreDelete(ws_clients_mutex_);
    if (state_cache_mutex_) vSemaphoreDelete(state_cache_mutex_);
}

bool WebServer::Start(int port) {
    ESP_LOGI(TAG, "Starting web server on port %d", port);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 16;  // 已注册 13 个处理器，余量留给扩展
    // 增加超时设置以更好地处理频繁请求
    config.recv_wait_timeout = 5;  // 接收超时5秒
    config.send_wait_timeout = 5;  // 发送超时5秒
    config.max_resp_headers = 8;   // 减少响应头数量

    if (httpd_start(&server_handle_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return false;
    }

    // 抑制 httpd 的"客户端断连"警告日志（防止刷屏饿死唤醒任务）
    //   ─── 为什么要这么做 ──────────────────────────────────────
    //   ESP-IDF 内部 httpd_txrx 在客户端断开时会打 W 级日志：
    //     httpd_sock_err: error in recv : 104   (ECONNRESET)
    //     httpd_sock_err: error in recv : 128   (ENOTCONN)
    //   客户端关标签页、手机锁屏、WiFi 抖动都会触发，实测能刷到每秒几十条。
    //   这些断连本身无害（httpd 会自行清理 fd），但每条都要格式化字符串
    //   + 走 115200 波特率串口（很慢）→ 单核 C3 上 httpd 任务会占满 CPU，
    //   唤醒词检测任务抢不到时间片、音频采样丢帧 →
    //   最终表现就是"待机一会后喊小智没反应"这种偶发失效。
    //   只压到 E 级：真实的 httpd 故障（如 httpd_start 失败）仍会打出来。
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    // 注册URI处理器
    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_get_handler,
        .user_ctx  = this
    };
    httpd_register_uri_handler(server_handle_, &index_uri);

    // ⚠️ 车（电机）相关的路由**全部不再注册**：
    //   /control /api/control /api/motor/action /api/motor_calib
    //   /api/debug/motor_test /api/max_speed /config /api/config
    //   这台设备是 xmini-c3，硬件上只有屏幕 + 语音、没有电机，这些都调不动。
    //   handler 函数还留在文件里（没注册就没人调得到），不占运行资源。

    // ── 屏幕「文字 / 表情」切换：GET 读当前值 / POST 写入 ──
    httpd_uri_t api_display_mode_get_uri = {
        .uri       = "/api/display_mode",
        .method    = HTTP_GET,
        .handler   = api_display_mode_handler,
        .user_ctx  = this
    };
    httpd_register_uri_handler(server_handle_, &api_display_mode_get_uri);

    httpd_uri_t api_display_mode_post_uri = {
        .uri       = "/api/display_mode",
        .method    = HTTP_POST,
        .handler   = api_display_mode_handler,
        .user_ctx  = this
    };
    httpd_register_uri_handler(server_handle_, &api_display_mode_post_uri);

    // ── 设备信息（IP + 当前显示模式），控制页加载时拉一次 ──
    httpd_uri_t api_info_uri = {
        .uri       = "/api/info",
        .method    = HTTP_GET,
        .handler   = api_info_handler,
        .user_ctx  = this
    };
    httpd_register_uri_handler(server_handle_, &api_info_uri);

    // ── 待机主题（动态星球 / 静态时钟）──
    httpd_uri_t api_theme_get_uri = {
        .uri       = "/api/theme",
        .method    = HTTP_GET,
        .handler   = api_theme_handler,
        .user_ctx  = this
    };
    httpd_register_uri_handler(server_handle_, &api_theme_get_uri);

    httpd_uri_t api_theme_post_uri = {
        .uri       = "/api/theme",
        .method    = HTTP_POST,
        .handler   = api_theme_handler,
        .user_ctx  = this
    };
    httpd_register_uri_handler(server_handle_, &api_theme_post_uri);

    // 壁纸/待机界面 API 已移除（控制页 UI 删了，对应 API 没人调了）

    // ── WebSocket 端点 ──
    //   浏览器长连接这里，服务器主动推 state。
    //   替代旧的 100ms 轮询 /api/status，把 httpd 任务负载从 10 req/s 降到 0（无变化时不收发任何东西）
    httpd_uri_t ws_uri = {
        .uri       = "/ws",
        .method    = HTTP_GET,
        .handler   = ws_handler,
        .user_ctx  = this,
        .is_websocket = true,  // 关键：开启 httpd 内置 WebSocket 握手和帧解析
    };
    httpd_register_uri_handler(server_handle_, &ws_uri);

    ESP_LOGI(TAG, "Web server started (http + ws)");
    return true;
}

void WebServer::Stop() {
    // 关闭 session 计时器 + 清空 WS 客户端列表
    CancelSessionTimer();
    if (ws_clients_mutex_) {
        xSemaphoreTake(ws_clients_mutex_, portMAX_DELAY);
        ws_clients_.clear();
        xSemaphoreGive(ws_clients_mutex_);
    }
    session_active_ = false;

    if (server_handle_) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        ESP_LOGI(TAG, "Web server stopped");
    }
}

void WebServer::SetMotorControlCallback(std::function<void(int direction, int speed)> callback) {
    motor_control_callback_ = callback;
}

void WebServer::InvokeMotorControl(int direction, int speed) {
    if (motor_control_callback_) {
        motor_control_callback_(direction, speed);
    }
}

void WebServer::SetSteerCallback(std::function<void(int steer, int throttle)> callback) {
    steer_callback_ = callback;
}

void WebServer::InvokeSteer(int steer, int throttle) {
    if (steer_callback_) {
        steer_callback_(steer, throttle);
    } else {
        // Fallback: 调 weak 符号，让板子自己实现
        // 弱符号：板子实现这两个参数（steer, throttle）的差速逻辑
        // 不强制 board 实现，由 IDF 默认 weak 0 容忍
        if (HandleBoardSteerControl) {
            HandleBoardSteerControl(steer, throttle);
        }
    }
}

void WebServer::InvokeToggleChat() {
    // 与板载 BOOT 键同路径。弱符号在 compact_wifi_board.cc 实现，
    // 调 Application::GetInstance().ToggleChatState() → MAIN_EVENT_TOGGLE_CHAT
    // （事件组异步派发，不阻塞 httpd 任务，唤醒/打断零延迟感知）
    if (HandleBoardToggleChat) {
        HandleBoardToggleChat();
    } else {
        ESP_LOGW(TAG, "HandleBoardToggleChat weak symbol not implemented on this board");
    }
}

void WebServer::SetMotorActionConfigCallback(std::function<MotorActionConfig()> get_callback,
                                           std::function<void(const MotorActionConfig&)> set_callback) {
    get_motor_config_callback_ = get_callback;
    set_motor_config_callback_ = set_callback;
}

void WebServer::SetEmotionCallback(std::function<void(const char* emotion)> callback) {
    emotion_callback_ = callback;
}

void WebServer::SetEmotion(const char* emotion) {
    if (emotion_callback_) {
        emotion_callback_(emotion);
    }
}

esp_err_t WebServer::index_get_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    // 固件升级后浏览器必须拿新页面（否则旧 JS 缓存导致"控制不了"）
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");

    httpd_resp_set_type(req, "text/html");
    // ⚠️ 换成新控制页（没有电机那套东西），旧的小车遥控页不再对外暴露
    httpd_resp_send(req, server->get_control_html_page(), HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

esp_err_t WebServer::control_post_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // 会话检查：5 分钟窗口外禁用控制
    bool active = false;
    if (check_session_active(req, &active) != ESP_OK) return ESP_FAIL;
    if (!active) {
        // 已经在 check_session_active 里发过 503
        return ESP_FAIL;
    }

    char content[100];
    // ⚠️ 留 1 字节给终止符（-1），收完手动补 '\0'：
    //    httpd_req_recv 不保证以 '\0' 结尾，而下游 parse_simple_control_command()
    //    会对数据做 strdup()；未终止的缓冲区会让 strdup 一路向后读，直到偶然
    //    碰到 0 字节 —— 越界读到栈上其他内容，解析出垃圾字段值。
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    int direction, speed, steer = 0, throttle = 0;
    bool has_steer = server->parse_simple_control_command(content, direction, speed, &steer, &throttle);

    // 如果有 steer/throttle 字段，优先走差速回调（双电机 C3 板用）
    if (has_steer) {
        server->InvokeSteer(steer, throttle);
    } else {
        // 否则走旧 0~4 离散方向回调（4 电机/3 电机/单电机板用）
        server->InvokeMotorControl(direction, speed);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

esp_err_t WebServer::api_control_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // 会话检查
    bool active = false;
    if (check_session_active(req, &active) != ESP_OK) return ESP_FAIL;
    if (!active) return ESP_FAIL;

    char content[200];
    // 同上：留 1 字节 + 手动补终止符，下游 cJSON_Parse 依赖 C 字符串
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // 解析JSON数据
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* jdir = cJSON_GetObjectItem(root, "direction");
    cJSON* jspd = cJSON_GetObjectItem(root, "speed");
    int direction = cJSON_IsNumber(jdir) ? jdir->valueint : 0;
    int speed = cJSON_IsNumber(jspd) ? jspd->valueint : 0;

    cJSON_Delete(root);

    // 调用电机控制回调（通过公共封装）
    server->InvokeMotorControl(direction, speed);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

// Simple struct passed to the stop task
struct debug_stop_param_t {
    WebServer* server;
    int duration_ms;
};

static void debug_stop_task(void* arg) {
    debug_stop_param_t* p = static_cast<debug_stop_param_t*>(arg);
    if (!p) vTaskDelete(NULL);
    vTaskDelay(pdMS_TO_TICKS(p->duration_ms));
    if (p->server) {
        p->server->InvokeMotorControl(0, 0); // stop via public wrapper
    }
    free(p);
    vTaskDelete(NULL);
}

esp_err_t WebServer::debug_motor_test_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // debug 接口忽略 session（开发用）
    char content[256];
    // 同上：留 1 字节 + 手动补终止符，下游 cJSON_Parse 依赖 C 字符串
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    int direction = 4;
    int speed = 80;
    int duration = 1000;
    if (root) {
        cJSON* jdir = cJSON_GetObjectItem(root, "direction");
        cJSON* jspd = cJSON_GetObjectItem(root, "speed");
        cJSON* jdur = cJSON_GetObjectItem(root, "duration");
        if (cJSON_IsNumber(jdir)) direction = jdir->valueint;
        if (cJSON_IsNumber(jspd)) speed = jspd->valueint;
        if (cJSON_IsNumber(jdur)) duration = jdur->valueint;
        cJSON_Delete(root);
    }

    if (server) {
        server->InvokeMotorControl(direction, speed);
        // spawn a short-lived task to stop after duration ms
        debug_stop_param_t* p = (debug_stop_param_t*)malloc(sizeof(debug_stop_param_t));
        if (p) {
            p->server = server;
            p->duration_ms = duration;
            BaseType_t rc = xTaskCreate(debug_stop_task, "dbg_stop", 2048, p, tskIDLE_PRIORITY + 1, NULL);
            if (rc != pdPASS) {
                free(p);
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t WebServer::api_motor_action_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // 会话检查
    bool active = false;
    if (check_session_active(req, &active) != ESP_OK) return ESP_FAIL;
    if (!active) return ESP_FAIL;

    char content[256];
    // 同上：留 1 字节 + 手动补终止符，下游 cJSON_Parse 依赖 C 字符串
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* action_json = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid action parameter");
        return ESP_FAIL;
    }

    std::string action = action_json->valuestring;

    // Get motor configuration for durations
    MotorActionConfig config;
    if (server->get_motor_config_callback_) {
        config = server->get_motor_config_callback_();
    }

    // Call motor action functions directly (simulating MCP tool calls) with configured durations
    try {
        ESP_LOGI(TAG, "网页动作请求: %s", action.c_str());

        if (action == "move_forward") {
            ESP_LOGI(TAG, "执行前进动作 - 速度:%d%%, 持续时间:%d ms", config.default_speed_percent, config.forward_duration_ms);
            HandleMotorActionForApplication(4, config.default_speed_percent, config.forward_duration_ms, 1);
        } else if (action == "move_backward") {
            ESP_LOGI(TAG, "执行后退动作 - 速度:%d%%, 持续时间:%d ms", config.default_speed_percent, config.backward_duration_ms);
            HandleMotorActionForApplication(2, config.default_speed_percent, config.backward_duration_ms, 1);
        } else if (action == "spin_around") {
            ESP_LOGI(TAG, "执行转圈动作 - 速度:%d%%, 持续时间:%d ms", config.default_speed_percent, config.spin_duration_ms);
            HandleMotorActionForApplication(3, config.default_speed_percent, config.spin_duration_ms, 1);
        } else if (action == "turn_left") {
            ESP_LOGI(TAG, "执行左转动作 - 速度:%d%%, 持续时间:%d ms", config.default_speed_percent, config.left_turn_duration_ms);
            HandleMotorActionForApplication(3, config.default_speed_percent, config.left_turn_duration_ms, 1);
        } else if (action == "turn_right") {
            ESP_LOGI(TAG, "执行右转动作 - 速度:%d%%, 持续时间:%d ms", config.default_speed_percent, config.right_turn_duration_ms);
            HandleMotorActionForApplication(1, config.default_speed_percent, config.right_turn_duration_ms, 1);
        } else if (action == "quick_forward") {
            ESP_LOGI(TAG, "执行快速前进动作 - 速度:%d%%, 持续时间:%d ms", config.default_speed_percent, config.quick_forward_duration_ms);
            HandleMotorActionForApplication(4, config.default_speed_percent, config.quick_forward_duration_ms, 1);
        } else if (action == "quick_backward") {
            ESP_LOGI(TAG, "执行快速后退动作 - 速度:%d%%, 持续时间:%d ms", config.default_speed_percent, config.quick_backward_duration_ms);
            HandleMotorActionForApplication(2, config.default_speed_percent, config.quick_backward_duration_ms, 1);
        } else if (action == "wiggle") {
            ESP_LOGI(TAG, "执行摆动动作 (情感:困惑)");
            server->SetEmotion("confused");
            HandleMotorActionForEmotion("confused");
        } else if (action == "dance") {
            ESP_LOGI(TAG, "执行跳舞动作 - 速度:%d%%", config.default_speed_percent);
            server->SetEmotion("excited");
            HandleMotorActionForDance(config.default_speed_percent);
        } else if (action == "stop") {
            ESP_LOGI(TAG, "执行停止动作");
            HandleMotorActionForApplication(0, 0, 0, 2); // stop, high priority
        } else if (action == "wake_up") {
            ESP_LOGI(TAG, "执行唤醒情感动作");
            server->SetEmotion("sleepy");  // 原版特殊动画：困倦效果
            HandleMotorActionForEmotion("wake");
        } else if (action == "happy") {
            ESP_LOGI(TAG, "执行开心情感动作");
            server->SetEmotion("laughing");  // 原版特殊动画：大笑动画
            HandleMotorActionForEmotion("happy");
        } else if (action == "sad") {
            ESP_LOGI(TAG, "执行悲伤情感动作");
            server->SetEmotion("crying");  // 原版特殊动画：哭泣动画
            HandleMotorActionForEmotion("sad");
        } else if (action == "thinking") {
            ESP_LOGI(TAG, "执行思考情感动作");
            server->SetEmotion("thinking");  // 原版特殊动画：思考动画
            HandleMotorActionForEmotion("thinking");
        } else if (action == "listening") {
            ESP_LOGI(TAG, "执行倾听情感动作");
            server->SetEmotion("wink");  // 原版特殊动画：眨眼动画
            HandleMotorActionForEmotion("listening");
        } else if (action == "speaking") {
            ESP_LOGI(TAG, "执行说话情感动作");
            server->SetEmotion("funny");  // 原版特殊动画：搞笑动画
            HandleMotorActionForEmotion("speaking");
        } else if (action == "excited") {
            ESP_LOGI(TAG, "执行兴奋情感动作");
            server->SetEmotion("shocked");  // 原版特殊动画：震惊动画
            HandleMotorActionForEmotion("excited");
        } else if (action == "loving") {
            ESP_LOGI(TAG, "执行爱慕情感动作");
            server->SetEmotion("kissy");  // 原版特殊动画：亲吻动画
            HandleMotorActionForEmotion("loving");
        } else if (action == "angry") {
            ESP_LOGI(TAG, "执行生气情感动作");
            server->SetEmotion("angry");  // 基础表情（生气）
            HandleMotorActionForEmotion("angry");
        } else if (action == "surprised") {
            ESP_LOGI(TAG, "执行惊讶情感动作");
            server->SetEmotion("surprised");  // 基础表情（惊讶）
            HandleMotorActionForEmotion("surprised");
        } else if (action == "confused") {
            ESP_LOGI(TAG, "执行困惑情感动作");
            server->SetEmotion("confused");  // 原版特殊动画：困惑动画
            HandleMotorActionForEmotion("confused");
        } else {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
            return ESP_FAIL;
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Failed to execute motor action %s: %s", action.c_str(), e.what());
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to execute action");
        return ESP_FAIL;
    }

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// GET /api/motor_calib → 返回当前 motor 自身的 swap_lr/inv_l/inv_r
//   不需要解锁（任何时候都能读，包括用于排查接线问题）
esp_err_t WebServer::api_motor_calib_get_handler(httpd_req_t *req) {
    bool swap = GetMotorSwapLr     ? GetMotorSwapLr()     : false;
    bool invL = GetMotorInvertLeft ? GetMotorInvertLeft() : false;
    bool invR = GetMotorInvertRight? GetMotorInvertRight(): false;
    char json[128];
    snprintf(json, sizeof(json),
        "{\"swap_lr\":%s,\"inv_l\":%s,\"inv_r\":%s}",
        swap ? "true" : "false", invL ? "true" : "false", invR ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------- 控制命令解析

bool WebServer::parse_simple_control_command(const char* data, int& direction, int& speed, int* steer_out, int* throttle_out) {
    // 简单解析格式：direction=X,speed=Y[,steer=A,throttle=B]
    // 后两个参数可选：差速转向模式（用于双电机 C3 板）
    char* str = strdup(data);
    char* token = strtok(str, ",");

    direction = 0;
    speed = 0;
    if (steer_out)    *steer_out    = 0;
    if (throttle_out) *throttle_out = 0;
    bool has_steer_mode = false;

    while (token != NULL) {
        if (strstr(token, "direction=")) {
            direction = atoi(token + 10);
        } else if (strstr(token, "speed=")) {
            speed = atoi(token + 6);
        } else if (strstr(token, "steer=") && steer_out) {
            *steer_out = atoi(token + 6);
            has_steer_mode = true;
        } else if (strstr(token, "throttle=") && throttle_out) {
            *throttle_out = atoi(token + 9);
        }
        token = strtok(NULL, ",");
    }

    free(str);

    // 如果发了 steer/throttle 字段，方向自动由符号决定（前进/后退不再由 direction 表示）
    if (has_steer_mode && throttle_out) {
        // 旧 direction 字段被忽略
        direction = 0;
    }
    return has_steer_mode;
}

// ──────────────────────────────────────────────────────────────────────
//  新控制页（/ ）
//    这台设备（xmini-c3）只有屏幕 + 语音，硬件上没有电机，原来那套摇杆遥控 /
//    电机配置页全部撤掉，改成三块：状态、屏幕显示模式、设备信息。
//    状态走 WebSocket 推送（/ws），不再用 100ms 轮询。
// ──────────────────────────────────────────────────────────────────────
const char* WebServer::get_control_html_page() {
    static const char html_page[] = R"html(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>小智 · 控制</title>
<style>
  :root{
    --bg:#0f1115; --card:#171a21; --line:#252a33;
    --txt:#e8eaed; --dim:#8b93a1; --ok:#3ddc84; --warn:#ffb020; --acc:#4a9eff;
  }
  *{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
  body{
    background:var(--bg);color:var(--txt);
    font-family:-apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif;
    padding:18px;max-width:560px;margin:0 auto;line-height:1.5;
  }
  h1{font-size:20px;font-weight:600;margin-bottom:3px}
  .sub{color:var(--dim);font-size:13px;margin-bottom:18px}
  .card{
    background:var(--card);border:1px solid var(--line);border-radius:14px;
    padding:16px;margin-bottom:14px;
  }
  .card h2{
    font-size:12px;font-weight:600;color:var(--dim);
    letter-spacing:.08em;margin-bottom:12px;
  }
  .row{display:flex;justify-content:space-between;align-items:center;padding:7px 0}
  .row + .row{border-top:1px solid var(--line)}
  .row .k{color:var(--dim);font-size:14px}
  .row .v{font-size:14px;font-variant-numeric:tabular-nums}
  .dot{
    display:inline-block;width:8px;height:8px;border-radius:50%;
    margin-right:7px;vertical-align:1px;background:#5a6270;
  }
  .dot.on{background:var(--ok)}
  .dot.warn{background:var(--warn)}
  .seg{display:flex;gap:8px;margin-top:2px}
  .seg button{
    flex:1;padding:13px 0;font-size:15px;font-weight:500;cursor:pointer;
    background:#20242d;color:var(--dim);border:1px solid var(--line);
    border-radius:10px;transition:.15s;
  }
  .seg button.active{background:var(--acc);color:#fff;border-color:var(--acc)}
  .seg button:active{transform:scale(.98)}
  .hint{color:var(--dim);font-size:12px;margin-top:11px}
</style>
</head>
<body>

<h1>小智 · 控制</h1>
<div class="sub" id="sub">连接中…</div>

<div class="card">
  <h2>状态</h2>
  <div class="row"><span class="k">设备</span><span class="v" id="state">—</span></div>
  <div class="row"><span class="k">IP 地址</span><span class="v" id="ip">—</span></div>
  <div class="row"><span class="k">会话</span><span class="v" id="ses">—</span></div>
</div>

<div class="card">
  <h2>屏幕显示</h2>
  <div class="seg">
    <button id="btn-face" onclick="setMode(false)">表情</button>
    <button id="btn-text" onclick="setMode(true)">文字</button>
  </div>
  <div class="hint">
    对话时屏幕上显示什么。<br>
    也可以直接 <b>双击 BOOT 键</b> 切换。
  </div>
</div>

<div class="card">
  <h2>待机壁纸</h2>
  <div class="seg">
    <button id="btn-wp0" onclick="setWallpaper(0)">时钟</button>
    <button id="btn-wp1" onclick="setWallpaper(1)">星球</button>
    <button id="btn-wp2" onclick="setWallpaper(2)">星空</button>
  </div>
  <div class="seg">
    <button id="btn-wp3" onclick="setWallpaper(3)">星际穿越</button>
    <button id="btn-wp4" onclick="setWallpaper(4)">指针时钟</button>
    <button id="btn-wp5" onclick="setWallpaper(5)">数字雨</button>
  </div>
  <div class="hint">
    <b>时钟</b>：大号数字时间 + IP，每秒刷新，最省电。<br>
    <b>星球</b>：光照球体 + 土星环 + 卫星 + 流星（12fps）。<br>
    <b>星空</b>：闪烁星点 + 偶尔流星，最安静（12fps）。<br>
    <b>星际穿越</b>：星点从中心辐射飞出，纵深感最强（12fps）。<br>
    <b>指针时钟</b>：模拟表盘 + 时分秒针，每秒跳一格。<br>
    <b>数字雨</b>：黑客帝国字符流（10fps）。<br>
    <br>
    也可以直接 <b>三击 BOOT 键</b> 循环切换。
  </div>
</div>

<div class="card">
  <h2>设备</h2>
  <div class="row"><span class="k">板型</span><span class="v">xmini-c3 · 16MB</span></div>
  <div class="row"><span class="k">屏幕</span><span class="v">SSD1306 128×64</span></div>
  <div class="row"><span class="k">功能</span><span class="v">屏幕 + 语音</span></div>
</div>

<script>
var STATE_TXT = {
  idle:'待机', listening:'聆听中', speaking:'说话中',
  connecting:'连接中', starting:'启动中', activating:'激活中',
  wifi:'配网中', upgrading:'升级中', fatal_error:'错误'
};

function setMode(text){
  fetch('/api/display_mode',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({text:text})
  }).then(function(r){ return r.json(); })
    .then(function(d){ paintMode(d.text); })
    .catch(function(e){ console.log(e); });
}

function paintMode(text){
  document.getElementById('btn-face').classList.toggle('active', !text);
  document.getElementById('btn-text').classList.toggle('active',  !!text);
}

function setWallpaper(v){
  // ⚠️ 先立刻高亮再发请求：单核 C3 上动态壁纸会拖慢 HTTP 响应（几百 ms 起），
  //   等服务端回包再更新 UI 的话，用户会以为"点了没反应"。
  //   先用本地状态给个即时反馈，等设备回包再用真实值校正一次。
  paintWallpaper(v);
  fetch('/api/theme',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({wallpaper:v})
  }).then(function(r){ return r.json(); })
    .then(function(d){ paintWallpaper(d.wallpaper); })
    .catch(function(e){ console.log(e); });
}

function paintWallpaper(v){
  // ⚠️ 上限必须是 6（壁纸总数）。之前写成 3，结果后 3 个按钮的 active
  //   永远不会被更新 —— 切到星际穿越/指针时钟/数字雨时按钮不亮，像"没反应"。
  for (var i = 0; i < 6; i++) {
    document.getElementById('btn-wp' + i).classList.toggle('active', i === v);
  }
}

function paintState(s, ses, now, end){
  var dot = (s !== 'idle') ? 'dot on' : 'dot';
  document.getElementById('state').innerHTML =
    '<span class="' + dot + '"></span>' + (STATE_TXT[s] || s);

  var el = document.getElementById('ses');
  if (ses && end > now){
    var left = end - now;
    el.textContent = Math.floor(left/60) + '分' + (left%60) + '秒后结束';
  } else {
    el.textContent = '无活跃会话';
  }
}

fetch('/api/info').then(function(r){ return r.json(); }).then(function(d){
  document.getElementById('ip').textContent = d.ip || '—';
  paintMode(!!d.text);
  paintWallpaper(d.wallpaper | 0);
}).catch(function(){});

var ws, retry = 0;
function connect(){
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onopen = function(){
    retry = 0;
    document.getElementById('sub').textContent = '已连接';
  };
  ws.onmessage = function(ev){
    try{
      var d = JSON.parse(ev.data);
      if (d.t === 'state') paintState(d.s, d.ses, d.now, d.end);
    }catch(e){}
  };
  ws.onclose = function(){
    document.getElementById('sub').textContent = '已断开，重连中…';
    retry++;
    setTimeout(connect, Math.min(retry,5) * 1000);
  };
}
connect();
</script>
</body>
</html>
)html";
    return html_page;
}

// 配置页面处理器
esp_err_t WebServer::config_get_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, server->get_config_html_page(), HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

// 配置页面POST处理器
esp_err_t WebServer::config_post_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';  // httpd_req_recv 不保证终止，strtok 依赖 C 字符串

    // 解析表单数据（以当前配置为底，表单缺字段时保留现值）
    MotorActionConfig config;
    if (server->get_motor_config_callback_) {
        config = server->get_motor_config_callback_();
    }
    server->parse_config_form_data(content, config);

    // 保存配置
    if (server->set_motor_config_callback_) {
        server->set_motor_config_callback_(config);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, R"html(<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>配置已保存</title>
    <style>
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            margin: 0;
            padding: 20px;
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
        }

        .container {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 20px;
            padding: 40px;
            box-shadow: 0 20px 40px rgba(0,0,0,0.1);
            text-align: center;
            max-width: 500px;
            width: 100%;
        }

        .success-icon {
            font-size: 4em;
            margin-bottom: 20px;
        }

        h1 {
            color: #333;
            margin-bottom: 10px;
            font-size: 2.2em;
        }

        .message {
            color: #666;
            margin-bottom: 30px;
            font-size: 1.1em;
        }

        .buttons {
            display: flex;
            gap: 15px;
            justify-content: center;
            flex-wrap: wrap;
        }

        .btn {
            background: linear-gradient(135deg, #4CAF50, #45a049);
            color: white;
            border: none;
            padding: 12px 24px;
            border-radius: 25px;
            font-size: 16px;
            cursor: pointer;
            text-decoration: none;
            display: inline-block;
            transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(76, 175, 80, 0.3);
        }

        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(76, 175, 80, 0.4);
        }

        .btn.secondary {
            background: linear-gradient(135deg, #2196F3, #1976D2);
        }

        .btn.secondary:hover {
            box-shadow: 0 6px 20px rgba(33, 150, 243, 0.4);
        }

        @media (max-width: 480px) {
            .container {
                padding: 30px 20px;
            }

            .buttons {
                flex-direction: column;
                align-items: center;
            }

            .btn {
                width: 200px;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="success-icon">✅</div>
        <h1>配置已保存！</h1>
        <div class="message">您的电机动作配置已成功保存到设备中。</div>

        <div class="buttons">
            <a href="/config" class="btn secondary">⚙️ 返回配置页面</a>
            <a href="/" class="btn">🏠 返回遥控器</a>
        </div>
    </div>
</body>
</html>)html", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Configuration callback not set");
    }

    return ESP_OK;
}

// API配置GET处理器
esp_err_t WebServer::api_config_get_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    if (server->get_motor_config_callback_) {
        MotorActionConfig config = server->get_motor_config_callback_();

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "forward_ms", config.forward_duration_ms);
        cJSON_AddNumberToObject(root, "backward_ms", config.backward_duration_ms);
        cJSON_AddNumberToObject(root, "left_turn_ms", config.left_turn_duration_ms);
        cJSON_AddNumberToObject(root, "right_turn_ms", config.right_turn_duration_ms);
        cJSON_AddNumberToObject(root, "spin_ms", config.spin_duration_ms);
        cJSON_AddNumberToObject(root, "quick_fwd_ms", config.quick_forward_duration_ms);
        cJSON_AddNumberToObject(root, "quick_bwd_ms", config.quick_backward_duration_ms);
        cJSON_AddNumberToObject(root, "def_speed_pct", config.default_speed_percent);
        cJSON_AddBoolToObject(root, "swap_lr", config.swap_lr);
        cJSON_AddBoolToObject(root, "invert_left", config.invert_left);
        cJSON_AddBoolToObject(root, "invert_right", config.invert_right);

        char *json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        if (json_str) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
            cJSON_free(json_str);
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate JSON");
        }
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Configuration callback not set");
    }

    return ESP_OK;
}

// API配置POST处理器
esp_err_t WebServer::api_config_post_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }
    content[ret] = '\0';  // httpd_req_recv 不保证终止，cJSON_Parse 依赖 C 字符串

    // 解析JSON数据
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // 以当前配置为底：POST 缺哪个字段就保留哪个字段的现值（部分提交安全）
    MotorActionConfig config;
    if (server->get_motor_config_callback_) {
        config = server->get_motor_config_callback_();
    }
    auto get_int = [&](const char* key, int& field) {
        cJSON* item = cJSON_GetObjectItem(root, key);
        if (cJSON_IsNumber(item)) field = item->valueint;
    };
    get_int("forward_ms", config.forward_duration_ms);
    get_int("backward_ms", config.backward_duration_ms);
    get_int("left_turn_ms", config.left_turn_duration_ms);
    get_int("right_turn_ms", config.right_turn_duration_ms);
    get_int("spin_ms", config.spin_duration_ms);
    get_int("wiggle_ms", config.wiggle_duration_ms);
    get_int("dance_ms", config.dance_duration_ms);
    get_int("quick_fwd_ms", config.quick_forward_duration_ms);
    get_int("quick_bwd_ms", config.quick_backward_duration_ms);
    get_int("def_speed_pct", config.default_speed_percent);
    // 方向修正开关（缺省保留当前值）
    cJSON* jdir = cJSON_GetObjectItem(root, "swap_lr");
    if (cJSON_IsBool(jdir)) config.swap_lr = cJSON_IsTrue(jdir);
    jdir = cJSON_GetObjectItem(root, "invert_left");
    if (cJSON_IsBool(jdir)) config.invert_left = cJSON_IsTrue(jdir);
    jdir = cJSON_GetObjectItem(root, "invert_right");
    if (cJSON_IsBool(jdir)) config.invert_right = cJSON_IsTrue(jdir);

    cJSON_Delete(root);

    // 保存配置
    if (server->set_motor_config_callback_) {
        server->set_motor_config_callback_(config);

        // 在串口中输出保存的配置信息
        ESP_LOGI(TAG, "网页配置已保存:");
        ESP_LOGI(TAG, "  前进时间: %d ms", config.forward_duration_ms);
        ESP_LOGI(TAG, "  后退时间: %d ms", config.backward_duration_ms);
        ESP_LOGI(TAG, "  左转时间: %d ms", config.left_turn_duration_ms);
        ESP_LOGI(TAG, "  右转时间: %d ms", config.right_turn_duration_ms);
        ESP_LOGI(TAG, "  转圈时间: %d ms", config.spin_duration_ms);
        ESP_LOGI(TAG, "  快速前进时间: %d ms", config.quick_forward_duration_ms);
        ESP_LOGI(TAG, "  快速后退时间: %d ms", config.quick_backward_duration_ms);
        ESP_LOGI(TAG, "  默认速度: %d%%", config.default_speed_percent);
        ESP_LOGI(TAG, "  接线修正: swap_lr=%d invert_left=%d invert_right=%d",
                 config.swap_lr, config.invert_left, config.invert_right);

        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Configuration callback not set");
    }

    return ESP_OK;
}

// 解析配置表单数据
void WebServer::parse_config_form_data(const char* data, MotorActionConfig& config) {
    // 完整表单语义：checkbox 未勾选时不会出现在提交数据里
    config.swap_lr = config.invert_left = config.invert_right = false;

    char* str = strdup(data);
    char* token = strtok(str, "&");

    while (token != NULL) {
        char* eq_pos = strchr(token, '=');
        if (eq_pos) {
            *eq_pos = '\0';
            char* key = token;
            char* value = eq_pos + 1;

            if (strcmp(key, "forward_ms") == 0) {
                config.forward_duration_ms = atoi(value);
            } else if (strcmp(key, "backward_ms") == 0) {
                config.backward_duration_ms = atoi(value);
            } else if (strcmp(key, "left_turn_ms") == 0) {
                config.left_turn_duration_ms = atoi(value);
            } else if (strcmp(key, "right_turn_ms") == 0) {
                config.right_turn_duration_ms = atoi(value);
            } else if (strcmp(key, "spin_ms") == 0) {
                config.spin_duration_ms = atoi(value);
            } else if (strcmp(key, "wiggle_ms") == 0) {
                config.wiggle_duration_ms = atoi(value);
            } else if (strcmp(key, "dance_ms") == 0) {
                config.dance_duration_ms = atoi(value);
            } else if (strcmp(key, "quick_fwd_ms") == 0) {
                config.quick_forward_duration_ms = atoi(value);
            } else if (strcmp(key, "quick_bwd_ms") == 0) {
                config.quick_backward_duration_ms = atoi(value);
            } else if (strcmp(key, "def_speed_pct") == 0) {
                config.default_speed_percent = atoi(value);
            } else if (strcmp(key, "swap_lr") == 0) {
                config.swap_lr = (strcmp(value, "on") == 0 || strcmp(value, "1") == 0 ||
                                  strcmp(value, "true") == 0);
            } else if (strcmp(key, "invert_left") == 0) {
                config.invert_left = (strcmp(value, "on") == 0 || strcmp(value, "1") == 0 ||
                                      strcmp(value, "true") == 0);
            } else if (strcmp(key, "invert_right") == 0) {
                config.invert_right = (strcmp(value, "on") == 0 || strcmp(value, "1") == 0 ||
                                       strcmp(value, "true") == 0);
            }
        }
        token = strtok(NULL, "&");
    }

    free(str);
}

// 获取配置页面HTML
const char* WebServer::get_config_html_page() {
    static const char config_html_page[] = R"html(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>电机动作配置</title>
    <style>
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            margin: 0;
            padding: 20px;
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
        }

        .container {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 20px;
            padding: 30px;
            box-shadow: 0 20px 40px rgba(0,0,0,0.1);
            max-width: 600px;
            width: 100%;
        }

        h1 {
            color: #333;
            text-align: center;
            margin-bottom: 30px;
        }

        .form-group {
            margin-bottom: 20px;
        }

        label {
            display: block;
            margin-bottom: 5px;
            font-weight: bold;
            color: #555;
        }

        input[type="number"] {
            width: 100%;
            padding: 10px;
            border: 2px solid #ddd;
            border-radius: 8px;
            font-size: 16px;
            transition: border-color 0.3s ease;
        }

        input[type="number"]:focus {
            outline: none;
            border-color: #4CAF50;
        }

        .unit {
            color: #666;
            font-size: 14px;
            margin-left: 5px;
        }

        .description {
            color: #777;
            font-size: 14px;
            margin-top: 3px;
            font-weight: normal;
        }

        .buttons {
            text-align: center;
            margin-top: 30px;
        }

        .btn {
            background: linear-gradient(135deg, #4CAF50, #45a049);
            color: white;
            border: none;
            padding: 12px 30px;
            border-radius: 25px;
            font-size: 16px;
            cursor: pointer;
            margin: 0 10px;
            text-decoration: none;
            display: inline-block;
            transition: all 0.3s ease;
        }

        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(76, 175, 80, 0.4);
        }

        .btn.secondary {
            background: linear-gradient(135deg, #2196F3, #1976D2);
        }

        .btn.secondary:hover {
            box-shadow: 0 6px 20px rgba(33, 150, 243, 0.4);
        }

        .grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
        }

        @media (max-width: 480px) {
            .grid {
                grid-template-columns: 1fr;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>⚙️ 电机动作配置</h1>
        <form method="POST" action="/config">
            <div class="grid">
                <div class="form-group">
                    <label for="forward_ms">前进时间</label>
                    <input type="number" id="forward_ms" name="forward_ms" min="100" max="30000" step="100" required>
                    <span class="unit">毫秒</span>
                    <div class="description">默认前进动作的持续时间</div>
                </div>

                <div class="form-group">
                    <label for="backward_ms">后退时间</label>
                    <input type="number" id="backward_ms" name="backward_ms" min="100" max="30000" step="100" required>
                    <span class="unit">毫秒</span>
                    <div class="description">默认后退动作的持续时间</div>
                </div>

                <div class="form-group">
                    <label for="left_turn_ms">左转时间</label>
                    <input type="number" id="left_turn_ms" name="left_turn_ms" min="100" max="10000" step="50" required>
                    <span class="unit">毫秒</span>
                    <div class="description">左转动作的持续时间</div>
                </div>

                <div class="form-group">
                    <label for="right_turn_ms">右转时间</label>
                    <input type="number" id="right_turn_ms" name="right_turn_ms" min="100" max="10000" step="50" required>
                    <span class="unit">毫秒</span>
                    <div class="description">右转动作的持续时间</div>
                </div>

                <div class="form-group">
                    <label for="spin_ms">转圈时间</label>
                    <input type="number" id="spin_ms" name="spin_ms" min="500" max="10000" step="100" required>
                    <span class="unit">毫秒</span>
                    <div class="description">转圈动作的持续时间</div>
                </div>


                <div class="form-group">
                    <label for="def_speed_pct">默认速度</label>
                    <input type="number" id="def_speed_pct" name="def_speed_pct" min="10" max="100" step="5" required>
                    <span class="unit">%</span>
                    <div class="description">电机动作的默认速度百分比</div>
                </div>

                <h3 style="grid-column: 1 / -1; margin: 10px 0 0 0; font-size: 16px; color: #333;">🔄 电机方向修正（接线不对时勾选，无需改线）</h3>
                <div class="form-group">
                    <label for="swap_lr" style="display:flex;align-items:center;gap:8px;cursor:pointer;">
                        <input type="checkbox" id="swap_lr" name="swap_lr" style="width:auto;">
                        左右电机对调
                    </label>
                    <div class="description">小车前进变成了原地转圈、或左右转反了时勾选</div>
                </div>
                <div class="form-group">
                    <label for="invert_left" style="display:flex;align-items:center;gap:8px;cursor:pointer;">
                        <input type="checkbox" id="invert_left" name="invert_left" style="width:auto;">
                        左电机反向
                    </label>
                    <div class="description">前进时向左跑偏（左边轮子装反）时勾选</div>
                </div>
                <div class="form-group">
                    <label for="invert_right" style="display:flex;align-items:center;gap:8px;cursor:pointer;">
                        <input type="checkbox" id="invert_right" name="invert_right" style="width:auto;">
                        右电机反向
                    </label>
                    <div class="description">前进时向右跑偏（右边轮子装反）时勾选</div>
                </div>
            </div>

            <div class="buttons">
                <button type="submit" class="btn">💾 保存配置</button>
                <a href="/" class="btn secondary">🏠 返回遥控器</a>
            </div>
        </form>
    </div>

    <script>
        // 页面加载时获取当前配置
        window.onload = function() {
            fetch('/api/config')
                .then(response => response.json())
                .then(config => {
                    document.getElementById('forward_ms').value = config.forward_ms;
                    document.getElementById('backward_ms').value = config.backward_ms;
                    document.getElementById('left_turn_ms').value = config.left_turn_ms;
                    document.getElementById('right_turn_ms').value = config.right_turn_ms;
                    document.getElementById('spin_ms').value = config.spin_ms;
                    document.getElementById('def_speed_pct').value = config.def_speed_pct;
                    document.getElementById('swap_lr').checked = !!config.swap_lr;
                    document.getElementById('invert_left').checked = !!config.invert_left;
                    document.getElementById('invert_right').checked = !!config.invert_right;
                })
                .catch(error => console.error('Failed to load config:', error));
        };

        // 处理表单提交
        document.getElementById('config-form').addEventListener('submit', function(e) {
            e.preventDefault(); // 阻止默认表单提交

            // 显式组装 JSON（数值 + 布尔）；未提交的字段由服务端保留当前值
            const data = {
                forward_ms: parseInt(document.getElementById('forward_ms').value) || 5000,
                backward_ms: parseInt(document.getElementById('backward_ms').value) || 5000,
                left_turn_ms: parseInt(document.getElementById('left_turn_ms').value) || 600,
                right_turn_ms: parseInt(document.getElementById('right_turn_ms').value) || 600,
                spin_ms: parseInt(document.getElementById('spin_ms').value) || 2500,
                def_speed_pct: parseInt(document.getElementById('def_speed_pct').value) || 100,
                swap_lr: document.getElementById('swap_lr').checked,
                invert_left: document.getElementById('invert_left').checked,
                invert_right: document.getElementById('invert_right').checked
            };

            fetch('/api/config', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify(data)
            })
            .then(response => response.json())
            .then(result => {
                if (result.status === 'success') {
                    // 显示成功消息
                    alert('配置保存成功！');
                    // 自动跳转回遥控器界面
                    window.location.href = '/';
                } else {
                    alert('配置保存失败，请重试');
                }
            })
            .catch(error => {
                console.error('Failed to save config:', error);
                alert('配置保存失败，请检查网络连接');
            });
        });
    </script>
</body>
</html>
)html";

    return config_html_page;
}

// ════════════════════════════════════════════════════════════════════════════
//  会话控制（5 分钟窗口）
// ════════════════════════════════════════════════════════════════════════════

// 5 分钟定时器任务：到点自动 EndSession
void WebServer::session_timer_task(void* arg) {
    WebServer* srv = static_cast<WebServer*>(arg);
    const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(kSessionDurationMs);
    while (xTaskGetTickCount() < end) {
        // 续期检查：EndSession 会在 srv->session_active_ = false 时删掉本 task
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!srv->session_active_) {
            break;  // 已被外部 EndSession
        }
    }
    if (srv->session_active_) {
        ESP_LOGI(TAG, "Web session 5 min expired, ending");
        srv->EndSession();   // 内部已推送锁定消息，此处不重复推
    }
    vTaskDelete(nullptr);
}

void WebServer::CancelSessionTimer() {
    // ⚠️ 必须先清句柄、再删任务，且绝不删"当前任务自身"：
    //   ─── 踩过的坑 ────────────────────────────────────────────
    //   本函数有两条调用路径：
    //     A) 外部任务（httpd 收到消息）→ 删掉旧的定时器任务 → 安全
    //     B) 定时器任务自己到点 → EndSession() → 调本函数 → 自杀！
    //   路径 B 下 vTaskDelete(自己的句柄) 会让 FreeRTOS 立即切走，
    //   "session_timer_handle_ = nullptr" 这行**永远不会执行** → 句柄悬空；
    //   下一次 StartSessionTimerLocked() 拿这个悬空句柄再调 vTaskDelete()
    //   → 破坏 FreeRTOS 内部链表 / 堆 → 随机 crash。
    //   更糟的是 EndSession() 持着 session_mutex_ 自杀 → 锁永不释放
    //   → 之后每次唤醒都要拿这把锁（portMAX_DELAY）→ 主事件循环挂死。
    TaskHandle_t h = session_timer_handle_;
    if (h) {
        session_timer_handle_ = nullptr;               // 1) 先清句柄，杜绝二次删除
        if (h != xTaskGetCurrentTaskHandle()) {        // 2) 只删别人，不删自己
            vTaskDelete(h);
        }
        // h == 自己：什么都不做，由任务末尾的 vTaskDelete(nullptr) 正常收尾
    }
}

void WebServer::StartSessionTimerLocked() {
    CancelSessionTimer();
    if (!server_handle_) return;  // httpd 没起来就别开
    BaseType_t rc = xTaskCreate(session_timer_task, "ws_session",
                                2048, this, 1, &session_timer_handle_);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create session timer task");
        session_timer_handle_ = nullptr;
    }
}

void WebServer::StartSession() {
    if (!session_mutex_) { ESP_LOGW(TAG, "StartSession: no mutex"); return; }
    xSemaphoreTake(session_mutex_, portMAX_DELAY);
    bool was_active = session_active_;
    session_active_ = true;
    session_end_ms_ = (esp_timer_get_time() / 1000) + (int64_t)kSessionDurationMs;
    if (!was_active) {
        ESP_LOGI(TAG, "Web session STARTED (5 min) - WS clients=%u", (unsigned)ws_clients_.size());
    } else {
        ESP_LOGI(TAG, "Web session RENEWED (5 min)");
    }
    StartSessionTimerLocked();
    xSemaphoreGive(session_mutex_);
}

void WebServer::EndSession() {
    if (!session_mutex_) return;
    bool was_active = false;
    bool had_control = false;

    // ① 持锁期间**只改状态位**（纯内存操作）。
    //    绝不在这里做：调回调、阻塞式 I/O、vTaskDelete（尤其不能自杀——
    //    自杀会让下面的 xSemaphoreGive 永远执行不到，锁被永久带走）。
    xSemaphoreTake(session_mutex_, portMAX_DELAY);
    was_active   = session_active_;
    had_control  = control_mode_;
    session_active_      = false;
    session_end_ms_      = 0;
    control_mode_        = false;
    control_mode_end_ms_ = 0;
    xSemaphoreGive(session_mutex_);     // ← 先放锁

    // ② 放锁后再取消定时器。若本函数是由定时器任务自己调进来的，
    //    CancelSessionTimer() 内部会跳过自杀，于是代码能继续往下走到 ③。
    CancelSessionTimer();
    CancelControlModeTimer();

    // ③ ⚠️ 安全兜底：会话结束 = 客户端失联（锁屏 / 关标签页 / WiFi 断）。
    //    joy 路径走的是 2 参 Drive()，本身不会自动停，若不显式停车，
    //    车会保持最后一条非零指令一直全速跑下去。
    InvokeSteer(0, 0);
    InvokeMotorControl(0, 0);

    // ④ 放锁后再回调 application 恢复 wake word（回调内含阻塞式 WS 发送）
    if (had_control) {
        ESP_LOGI(TAG, "Auto-exit control mode (session ended)");
        SetControlModeEnabled(false);
    }
    if (was_active) {
        ESP_LOGI(TAG, "Web session ENDED");
    }

    // ⑤ 推一条锁定消息让前端立即显示锁屏
    //   原来只有 session_timer_task 到期路径才推，其他调用 EndSession() 的路径
    //   前端不会立刻锁屏；统一放到这里，与 EndControlMode() 行为一致。
    const char* expired_msg =
        "{\"t\":\"state\",\"s\":\"idle\",\"ses\":false,\"now\":0,\"end\":0,\"ctrl\":false,\"ctrl_end\":0}";
    SendToAllWsClients(expired_msg, strlen(expired_msg));
}

bool WebServer::IsSessionActive() const {
    return session_active_;
}

int64_t WebServer::GetSessionEndMs() const {
    return session_end_ms_;
}

// ════════════════════════════════════════════════════════════════════════════
//  控制模式（按用户要求：进入控制页面后彻底禁用对话）
//   - 绕过 ws_handler 的 IsSessionActive() 检查
//   - 5 分钟无活动自动 EndControlMode
//   - 同时通过弱符号 OnControlModeChange() 通知 application
//     禁用/启用 wake word detection
// ════════════════════════════════════════════════════════════════════════════

void WebServer::control_mode_timer_task(void* arg) {
    WebServer* srv = static_cast<WebServer*>(arg);
    // 每次有新消息续期 control_mode_last_msg_ms_；本任务 1s 检查一次
    while (srv->control_mode_) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - srv->control_mode_last_msg_ms_ >= (int64_t)kSessionDurationMs) {
            ESP_LOGI(TAG, "Control mode 5 min idle, auto exit");
            // EndControlMode() 结尾已推过一条 ctrl=false 的锁定消息，此处不重复推
            srv->EndControlMode();
            break;
        }
    }
    vTaskDelete(nullptr);
}

void WebServer::CancelControlModeTimer() {
    // 同 CancelSessionTimer()：先清句柄、只删别人不删自己。
    //   控制模式 5 分钟无操作到期 → control_mode_timer_task 自己走 EndControlMode()
    //   → 调本函数 → 若在这里自杀，session_mutex_ 就永远放不掉。
    TaskHandle_t h = control_mode_timer_handle_;
    if (h) {
        control_mode_timer_handle_ = nullptr;
        if (h != xTaskGetCurrentTaskHandle()) {
            vTaskDelete(h);
        }
    }
}

void WebServer::StartControlModeTimerLocked() {
    CancelControlModeTimer();
    if (!server_handle_) return;
    BaseType_t rc = xTaskCreate(control_mode_timer_task, "ws_ctrlmode",
                                2048, this, 1, &control_mode_timer_handle_);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create control mode timer task");
        control_mode_timer_handle_ = nullptr;
    }
}

void WebServer::SetControlModeEnabled(bool enabled) {
    // 状态变更 → 通知 application（禁用/恢复 wake word + 停 listening）
    if (OnControlModeChange) OnControlModeChange(enabled);
}

void WebServer::StartControlMode() {
    if (!session_mutex_) { ESP_LOGW(TAG, "StartControlMode: no mutex"); return; }
    xSemaphoreTake(session_mutex_, portMAX_DELAY);
    int64_t now_ms = esp_timer_get_time() / 1000;
    control_mode_last_msg_ms_ = now_ms;
    control_mode_end_ms_      = now_ms + (int64_t)kSessionDurationMs;
    bool was_active = control_mode_;
    control_mode_ = true;
    if (!was_active) {
        ESP_LOGI(TAG, "Control mode STARTED (5 min) - chat disabled");
        SetControlModeEnabled(true);
    } else {
        ESP_LOGI(TAG, "Control mode RENEWED (5 min)");
    }
    StartControlModeTimerLocked();
    xSemaphoreGive(session_mutex_);

    // 主动推一次 state 让前端立即解锁 + 拿到倒计时
    // 复用 application 的 JSON 格式（s/idle, ses=原状态, ctrl=1, ctrl_end=倒计时）
    // 用 uint32 秒（newlib-nano 对 %lld 不友好）
    uint32_t now_sec = (uint32_t)(now_ms / 1000);
    uint32_t ctrl_end_sec = (uint32_t)(control_mode_end_ms_ / 1000);
    uint32_t ses_end_sec = (uint32_t)(session_end_ms_ / 1000);
    char msg[224];
    snprintf(msg, sizeof(msg),
        "{\"t\":\"state\",\"s\":\"idle\",\"ses\":%s,\"now\":%u,\"end\":%u,\"ctrl\":true,\"ctrl_end\":%u}",
        session_active_ ? "true" : "false",
        (unsigned)now_sec, (unsigned)ses_end_sec, (unsigned)ctrl_end_sec);
    SendToAllWsClients(msg, strlen(msg));
}

void WebServer::EndControlMode() {
    if (!session_mutex_) return;
    bool was_active = false;

    // ① 持锁期间**只改状态位**（同 EndSession 的说明）：
    //    绝不在持锁时回调 application 或 vTaskDelete(自己)。
    //    控制模式 5 分钟无操作到期这条路径，正是定时器任务自己走进来的，
    //    一旦自杀 → xSemaphoreGive 执行不到 → session_mutex_ 永久丢失。
    xSemaphoreTake(session_mutex_, portMAX_DELAY);
    was_active = control_mode_;
    control_mode_ = false;
    control_mode_end_ms_ = 0;
    xSemaphoreGive(session_mutex_);       // ← 先放锁

    // ② 放锁后取消定时器。若是自己 → CancelControlModeTimer() 跳过自杀，
    //    代码得以继续往下走（旧代码在这里就"消失"了）。
    CancelControlModeTimer();

    // ③ ⚠️ 安全兜底：退出控制模式立刻停车。
    //    joy 走的是 2 参 Drive()，无自动停止；不显式停的话车会照最后一条
    //    非零指令一直跑（手机锁屏 / 切后台 / 掉线都会触发本函数）。
    InvokeSteer(0, 0);
    InvokeMotorControl(0, 0);

    // ④ 放锁后再回调 application（内部含阻塞式 WS 发送，不可持锁调用）
    if (was_active) {
        ESP_LOGI(TAG, "Control mode ENDED - chat re-enabled");
        SetControlModeEnabled(false);
    }

    // ⑤ 推一条锁定消息让前端立即显示锁屏
    const char* expired = "{\"t\":\"state\",\"s\":\"idle\",\"ses\":false,\"now\":0,\"end\":0,\"ctrl\":false,\"ctrl_end\":0}";
    SendToAllWsClients(expired, strlen(expired));
}

void WebServer::TouchControlMode() {
    if (!session_mutex_) return;
    xSemaphoreTake(session_mutex_, portMAX_DELAY);
    if (control_mode_) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        control_mode_last_msg_ms_ = now_ms;
        control_mode_end_ms_      = now_ms + (int64_t)kSessionDurationMs;
    }
    xSemaphoreGive(session_mutex_);
}

// 遥控自动表情：动作变化才刷（抖动防抖），停车保持
// 映射到 OledDisplay::SetEmotion 实际支持的 5 种：
//   前进 happy(笑眼) / 后退 sad(哭眼) / 左转 listening(视线向左) / 右转 thinking(视线向右)
//   注意：excited/confused/surprised 等 OLED 不认，会落到 neutral → 表情不动
void WebServer::AutoEmotionForDrive(int steer, int throttle) {
    const char* e = nullptr;
    if      (throttle >  15) e = "happy";      // 前进：笑眼
    else if (throttle < -15) e = "sad";        // 后退：哭眼
    else if (steer < -15)    e = "listening";  // 左转：视线向左
    else if (steer >  15)    e = "thinking";   // 右转：视线向右
    else return;                               // 停车：保持当前表情
    if (strcmp(e, last_auto_emotion_) == 0) return;
    strncpy(last_auto_emotion_, e, sizeof(last_auto_emotion_) - 1);
    SetEmotion(e);
}

// HTTP handler 共用：检查 session，false 就 503
//   - 浏览器看到 503 不会重试 30s（不浪费 CPU）
esp_err_t WebServer::check_session_active(httpd_req_t *req, bool* is_active) {
    WebServer* server = (WebServer*)req->user_ctx;
    if (!server) return ESP_FAIL;
    *is_active = server->IsSessionActive();
    if (*is_active) return ESP_OK;
    const char* body = "{\"error\":\"session_locked\",\"hint\":\"say \\\"你好小智\\\" to unlock 5 min\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

// ════════════════════════════════════════════════════════════════════════════
//  WebSocket 客户端管理
// ════════════════════════════════════════════════════════════════════════════

void WebServer::AddWsClient(int fd) {
    if (!ws_clients_mutex_) return;
    if (xSemaphoreTake(ws_clients_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        // 避免重复
        for (auto& c : ws_clients_) {
            if (c.fd == fd) { xSemaphoreGive(ws_clients_mutex_); return; }
        }
        ws_clients_.push_back({fd});
        xSemaphoreGive(ws_clients_mutex_);
        ESP_LOGI(TAG, "WS client %d added (total=%u)", fd, (unsigned)ws_clients_.size());
    }
}

void WebServer::RemoveWsClient(int fd) {
    if (!ws_clients_mutex_) return;
    bool last_one_gone = false;
    if (xSemaphoreTake(ws_clients_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = ws_clients_.begin();
        while (it != ws_clients_.end()) {
            if (it->fd == fd) { it = ws_clients_.erase(it); break; }
            else ++it;
        }
        // 在锁内判断"是否最后一个"，避免并发下误判
        last_one_gone = ws_clients_.empty();
        xSemaphoreGive(ws_clients_mutex_);
        ESP_LOGI(TAG, "WS client %d removed (total=%u)", fd, (unsigned)ws_clients_.size());
    }

    // ⚠️ 安全兜底（锁外执行）：最后一个网页客户端掉线 → 立即停车。
    //    ─── 为什么必须有 ────────────────────────────────────────
    //    手机锁屏 / 关标签页 / 切后台 / WiFi 断开时，前端来不及发出
    //    "松手停车"({t:'joy',s:0,th:0})；而 joy 走的是 2 参 Drive()，
    //    **本身不带自动停止**（只有 /api/control 路径有 watchdog）→
    //    车会照着最后一条非零指令一直全速跑下去，直到撞墙或电池耗尽。
    if (last_one_gone) {
        InvokeSteer(0, 0);
        InvokeMotorControl(0, 0);
    }
}

void WebServer::SendToAllWsClients(const char* data, size_t len) {
    if (!ws_clients_mutex_ || !server_handle_) return;
    if (xSemaphoreTake(ws_clients_mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return;

    // 复制 fd 列表到局部，避免发送时持锁太久
    std::vector<int> fds;
    fds.reserve(ws_clients_.size());
    for (auto& c : ws_clients_) fds.push_back(c.fd);
    xSemaphoreGive(ws_clients_mutex_);

    // 异步发送（不阻塞调用者，且发送失败会回调 __notified 路径）
    for (int fd : fds) {
        httpd_ws_frame_t frame = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t*)data,
            .len = len,
        };
        esp_err_t err = httpd_ws_send_frame_async(server_handle_, fd, &frame);
        if (err != ESP_OK) {
            // 失败多半是对端断了，懒清理
            ESP_LOGD(TAG, "WS send to fd=%d failed (%d), will remove", fd, err);
            RemoveWsClient(fd);
        }
    }
}

void WebServer::BroadcastStateJson(const char* json_str) {
    if (!json_str) return;
    size_t len = strlen(json_str);
    if (len == 0 || len >= sizeof(last_state_json_)) return;

    // 1) 缓存最新 state，页面打开时通过 / 的 HTML 嵌入给前端，避免白屏
    if (state_cache_mutex_ && xSemaphoreTake(state_cache_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(last_state_json_, json_str, len + 1);
        xSemaphoreGive(state_cache_mutex_);
    }

    // 2) 异步推给所有已连接 WS 客户端
    int n_clients = 0;
    if (ws_clients_mutex_ && xSemaphoreTake(ws_clients_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        n_clients = (int)ws_clients_.size();
        xSemaphoreGive(ws_clients_mutex_);
    }
    SendToAllWsClients(json_str, len);
    ESP_LOGI(TAG, "Broadcast [%s] to %d client(s): %.80s", session_active_ ? "ACTIVE" : "locked", n_clients, json_str);
}

const char* WebServer::GetLastStateJson() const {
    if (state_cache_mutex_ && xSemaphoreTake(state_cache_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        const char* p = last_state_json_;
        xSemaphoreGive(state_cache_mutex_);
        return p;
    }
    return "";
}

// WebSocket handler
//   - httpd 自动处理握手（因为 .is_websocket = true）
//   - 这里只需要管：收消息（来自浏览器的控制）+ 连接关闭
esp_err_t WebServer::ws_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;
    if (!server) return ESP_FAIL;
    ESP_LOGI(TAG, "ws_handler called, method=%d uri=%s", req->method, req->uri);

    // 区分握手阶段 vs 帧阶段
    //   握手时 httpd 已经处理了 Upgrade，这里只处理已升级后的帧
    if (req->method == HTTP_GET) {
        // is_websocket=true 后，httpd 已发 101 Switching Protocols 并切换到 WS 协议。
        // 这里处理新客户端接入 + 立即推一份最新 state。
        // 之前用 httpd_ws_get_fd_info 检查发现会返回 INVALID → 让客户端重连 → 死循环，
        // 所以这里直接信任 is_websocket 标志，不做额外检查。
        int fd = httpd_req_to_sockfd(req);
        server->AddWsClient(fd);

        // 立即推一份最新 state（让新连接同步当前状态，不显示 5s "连接中"）
        const char* cached = server->GetLastStateJson();
        if (cached && cached[0]) {
            httpd_ws_frame_t frame = {
                .final = true, .fragmented = false,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t*)cached,
                .len = strlen(cached),
            };
            esp_err_t err = httpd_ws_send_frame(req, &frame);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Initial state push to fd=%d failed: %d", fd, err);
            }
        }
        return ESP_OK;
    }

    // 收帧
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = HTTPD_WS_TYPE_TEXT;
    // 先 peek 长度
    esp_err_t err = httpd_ws_recv_frame(req, &pkt, 0);
    if (err != ESP_OK) {
        int fd = httpd_req_to_sockfd(req);
        server->RemoveWsClient(fd);
        return ESP_OK;  // 不返回 ESP_FAIL，否则 httpd 可能关连接
    }
    if (pkt.len == 0) return ESP_OK;  // ping-only 帧

    // ⚠️ 长度上限校验（防畸形帧头导致溢出 + 越界写）
    //   pkt.len 直接取自 WS 帧头，理论上限 2^64-1。没有这道闸会怎样：
    //     ① malloc(pkt.len + 1)：pkt.len+1 **整数溢出为 0** → malloc(0)
    //        newlib 下返回非 NULL（不是 NULL！），于是 !buf 的判空形同虚设；
    //     ② buf[pkt.len] = '\0'：向 2^64-1 的偏移写 0 → 立即 LoadStore 异常
    //        → 设备 panic 重启。一条畸形帧就能把车打重启。
    //   实测正常控制报文（joy / act / emotion / interrupt）都远小于 512 字节，
    //   超限一律视为异常客户端，断开（断开会触发停车，安全）。
    if (pkt.len > 512) {
        ESP_LOGW(TAG, "WS frame too large (%u bytes), dropping client", (unsigned)pkt.len);
        int fd = httpd_req_to_sockfd(req);
        server->RemoveWsClient(fd);
        return ESP_OK;
    }

    // 分配 payload 缓冲（此时 pkt.len <= 512，+1 不可能溢出）
    uint8_t* buf = (uint8_t*)malloc(pkt.len + 1);
    if (!buf) return ESP_OK;
    pkt.payload = buf;
    err = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (err != ESP_OK) {
        free(buf);
        int fd = httpd_req_to_sockfd(req);
        server->RemoveWsClient(fd);
        return ESP_OK;
    }
    buf[pkt.len] = '\0';

    // 解析消息类型（极简 JSON 解析，避免 cJSON 体积）
    //   {t:"joy", s:steer, th:throttle}  摇杆
    //   {t:"act", a:"move_forward"}      动作按钮
    //   {t:"stop"}                      停止
    //   {t:"ping"}                      心跳
    char* p = (char*)buf;

    // session 没开就只允许 ping / control_enter / control_exit / set_motor_calib
    //   - control_enter 必须允许通过（否则永远进不去控制模式）
    //   - set_motor_calib 改的是无危害的配置（接线修正），任何时候都允许
    //   - control_mode 开启后绕过此检查（控制页面用户不需要喊"你好小智"）
    bool is_ping = strstr(p, "\"t\":\"ping\"") != nullptr;
    bool is_ctrl_enter = strstr(p, "\"t\":\"control_enter\"") != nullptr;
    bool is_ctrl_exit  = strstr(p, "\"t\":\"control_exit\"")  != nullptr;
    bool is_calib      = strstr(p, "\"t\":\"set_motor_calib\"") != nullptr;
    bool is_emotion    = strstr(p, "\"t\":\"emotion\"")       != nullptr;  // 表情按钮锁定时也放行（不碰电机）
    bool is_interrupt  = strstr(p, "\"t\":\"interrupt\"")    != nullptr;  // 切换对话按钮：锁定时也放行（语义同 BOOT 键）
    if (!server->IsSessionActive() && !server->IsControlMode()
        && !is_ping && !is_ctrl_enter && !is_ctrl_exit && !is_calib && !is_emotion && !is_interrupt) {
        free(buf);
        return ESP_OK;
    }

    if (is_ping) {
        // 不回 ping（节省带宽），客户端 JS 不依赖 pong
    } else if (is_ctrl_enter) {
        server->StartControlMode();
    } else if (is_ctrl_exit) {
        server->EndControlMode();
    } else if (strstr(p, "\"t\":\"set_motor_calib\"")) {
        // 接线修正：{t:"set_motor_calib", swap_lr:0/1, inv_l:0/1, inv_r:0/1}
        //   改 motor 自身 invert/swap flag（立即生效）+ 持久化到 NVS（由 application 实现）
        bool swap_lr = false, inv_l = false, inv_r = false;
        char* jp = strstr(p, "\"swap_lr\":");  if (jp) swap_lr = (atoi(jp + 10) != 0) || (strstr(jp, "true") != nullptr);
        jp = strstr(p, "\"inv_l\":");          if (jp) inv_l   = (atoi(jp + 8)  != 0) || (strstr(jp, "true") != nullptr);
        jp = strstr(p, "\"inv_r\":");          if (jp) inv_r   = (atoi(jp + 8)  != 0) || (strstr(jp, "true") != nullptr);
        if (SetMotorSwapLr)     SetMotorSwapLr(swap_lr);
        if (SetMotorInvertLeft) SetMotorInvertLeft(inv_l);
        if (SetMotorInvertRight)SetMotorInvertRight(inv_r);
        if (OnMotorCalibChanged) OnMotorCalibChanged(swap_lr, inv_l, inv_r);
        ESP_LOGI(TAG, "Motor calib updated: swap_lr=%d inv_l=%d inv_r=%d", swap_lr, inv_l, inv_r);
    } else if (strstr(p, "\"t\":\"stop\"")) {
        // 控制模式期间收到任何非 ping 消息都续期 idle 超时
        if (server->IsControlMode()) server->TouchControlMode();
        server->InvokeSteer(0, 0);
        server->InvokeMotorControl(0, 0);
    } else if (strstr(p, "\"t\":\"act\"")) {
        if (server->IsControlMode()) server->TouchControlMode();
        // 提取 a 字段值（简单找 ":\"...\""）
        char* ap = strstr(p, "\"a\":\"");
        if (ap) {
            ap += 5;
            char* end = strchr(ap, '\"');
            if (end) {
                *end = '\0';
                // 直接调底层 handler 路径：构造伪请求走 motor_action
                // 这里不复用 InvokeSteer/InvokeMotorControl 也不走 Application 的 4 象限路径
                // （那条路径会应用 swap_lr/invert 配置，方向会和摇杆/语音命令不一致）。
                // 直接走 motor_controller 的 Execute（用 MOTOR 枚举）——和 MCP 工具同一个路径，
                // 只受 motor 自己的 invert flag 影响，方向永远符合代码里的字面意义。
                ESP_LOGI(TAG, "WS act: %s", ap);
                if      (strcmp(ap, "move_forward")  == 0 && HandleMotorExecuteByEnum) { HandleMotorExecuteByEnum(1, 100, 800); server->AutoEmotionForDrive(0, 100); }
                else if (strcmp(ap, "move_backward") == 0 && HandleMotorExecuteByEnum) { HandleMotorExecuteByEnum(2, 100, 800); server->AutoEmotionForDrive(0, -100); }
                else if (strcmp(ap, "turn_left")     == 0 && HandleMotorExecuteByEnum) { HandleMotorExecuteByEnum(3, 100, 600); server->AutoEmotionForDrive(-100, 0); }
                else if (strcmp(ap, "turn_right")    == 0 && HandleMotorExecuteByEnum) { HandleMotorExecuteByEnum(4, 100, 600); server->AutoEmotionForDrive(100, 0); }
                *end = '\"';
            }
        }
    } else if (strstr(p, "\"t\":\"joy\"")) {
        // 提取 s / th
        int steer = 0, throttle = 0;
        char* sp = strstr(p, "\"s\":");
        if (sp) steer = atoi(sp + 4);
        char* tp = strstr(p, "\"th\":");
        if (tp) throttle = atoi(tp + 5);
        if (server->IsControlMode()) server->TouchControlMode();
        // 不打日志：25Hz 逐条 log 的串口开销会引入抖动延迟
        server->InvokeSteer(steer, throttle);
        // 遥控自动表情（节流 1.5s，别刷爆 display）：
        //   前进 excited / 后退 confused / 转向 loving / 停车 sad
        server->AutoEmotionForDrive(steer, throttle);
    } else if (strstr(p, "\"t\":\"emotion\"")) {
        // {t:"emotion", e:"happy"} → 设备屏幕表情（网页表情按钮）
        char* ep = strstr(p, "\"e\":\"");
        if (ep) {
            ep += 5;
            char* end = strchr(ep, '\"');
            if (end) {
                *end = '\0';
                // 白名单（display 支持的表情）
                static const char* kEmotions[] = {
                    "happy","sad","angry","surprised","confused",
                    "excited","loving","thinking","wake","sad2","neutral"
                };
                for (auto e : kEmotions) {
                    if (strcmp(ep, e) == 0) { server->SetEmotion(ep); break; }
                }
                *end = '\"';
            }
        }
    } else if (is_interrupt) {
        // {t:"interrupt"} → 切换对话（与板载 BOOT 键同语义）
        //   空闲 → 唤醒聆听；说话中 → AbortSpeaking；聆听中 → 关闭回 idle
        //   弱符号 HandleBoardToggleChat 在 compact_wifi_board.cc 实现
        //   → Application::GetInstance().ToggleChatState() → MAIN_EVENT_TOGGLE_CHAT
        //   事件组异步派发（main thread 处理），不阻塞 httpd 任务。
        ESP_LOGI(TAG, "WS interrupt: toggle chat");
        server->InvokeToggleChat();
    }

    free(buf);
    return ESP_OK;
}
