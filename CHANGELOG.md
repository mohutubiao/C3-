# 更新日志

> 版本号说明：固件内 `PROJECT_VER` 仍沿用上游的 `2.1.0`（`CMakeLists.txt`），
> 本文件按**构建日期**归档，避免与上游版本号混淆。

---

## 2026-10-18

**发布包**：`firmware-merged-20261018.bin`
大小 3,019,816 B（2.88 MiB），MD5 `33438949C3157720443433A46DE0B5E6`
适用：`qebabe-xiaoche-c3`（ESP32-C3，4MB Flash），整包烧录到 `0x0`。
同日已在 COM19、COM16 两台实机烧录验证。

### 新增：动作序列系统（`self.motor.sequence`）

一次下发整段动作，固件内背靠背执行，段间零网络延迟。

- 格式 `"steer,throttle,ms;..."`，最多 64 段、总时长 ≤15 秒、单段 ≤5 秒
- 执行体是独立低优先级任务 `m2wd_seq`（栈 4096、优先级 5），惰性创建
- 序列期间登记忙窗口，不会被唤醒/说话/表情反馈抢占
- 外部新指令（`drive` / `stop` / 网页摇杆）会中止当前序列，语义为「最后一句说了算」

**为什么需要**：靠大模型连调多次 `self.motor.drive` 时，每次调用要走一轮 WS 往返（服务端推理 + 传输 + 两次事件循环），段与段之间明显停顿，车「走走停停」；且每段都重新走一次 ramp 软启动。序列把整段压成一次调用，时序精度由网络延迟级降到 10ms 级。

### 新增：预设动作支持自定义时长（`duration_ms`）

- `self.car.act` 新增可选参数 `duration_ms`（0 = 用动作自带默认值，上限 15000）
- `dance` 会**循环拼段**直到跳满指定时长（一轮 4 步 1.1 秒，5 秒 ≈ 18 段）
- `forward` / `backward` / `turn_*` / `quick_*` 同样可被覆盖

**修复的问题**：此前舞蹈时长硬编码 1.1 秒、前进 1.5 秒，用户说「跳 5 秒」时大模型没有任何参数可表达，实际只跳 1.1 秒。

### 修复：舞蹈 / 多步动作被装饰性动作掐断

`dance` 分支此前未登记忙窗口（`MarkMotorBusyForApp`），服务端随后的情绪或说话反馈会走
`QueueMotorAction → StopRealtimeMotorControl → motor->Stop()`，把动作当场腰斩。
现在 `dance` 与 `angry` 的多步序列都按实际时长登记忙窗口。

### 修复：连续下发的序列被丢弃

`PlaySequence` 原先在「上一段还在跑」时直接返回拒绝，导致「再跳久一点」这类连续调用全部失效。
改为**覆盖**语义：先中止旧序列再接管。

### 优化：唤醒响应速度（OLED 刷新 20Hz → 10Hz）

`RefreshTimerCb` 在 `esp_timer` 任务（优先级 22）里同步刷屏，每次 `PushFrame` 走 I2C 400kHz
写 1024 字节约 23ms，远高于 `audio_input` 任务（优先级 8）——每 50ms 就有 23ms 让唤醒词检测
完全停摆，I2C 占用率高达 46%。降到 10Hz 后占用率约 23%，唤醒最多快 23ms，表情仍有 10fps。

> 遗留：`esp_timer` 回调里做阻塞 I2C 传输本身不推荐，彻底解法是改成「置标志位 + 低优先级任务刷屏」。

### 新增：距离传感器在位探测（`self.sensor.get_distance`）

- 复用 OLED 的 `I2C_NUM_0`（C3 只有一条 I2C 控制器），地址 `0x29`
- 读 `MODEL_ID`（0xC0，期望 0xEE）判断 VL53L0X 是否在位，**尚不含连续测距**
- 避障 / 跌落检测待模块到货后实现（方案已定：装屏幕上方微微朝下，双阈值判断）

### 内部重构：`Stop()` 拆分为 `ApplyStop()` + `Stop()`

- `ApplyStop()`：纯断电，不碰序列状态，供自动停定时器到期调用
- `Stop()`：断电 + 中止序列，供外部急停调用

**为什么必须拆**：序列每步都会 `ArmAutoStop`，到期若走 `Stop()` 会被判成「外部急停」，整段序列只跑第一步。

### 文档

- 新增 `PROJECT.md`：硬件、引脚、功能、动作系统分层、编译烧录、资源占用、已知限制

---

## 2026-10-16（提交 `930cd2e`）

### 修复：语音 / MCP 动作被抢占

MCP 工具绕过动作队列直接驱电机，紧随其后的唤醒/说话/表情动作会从队列抢占并
`StopRealtimeMotorControl()` 把它掐断——实测「前进 4 秒」在 300ms 后被 200ms 表情 twitch
覆盖停止，表现为「只动一下」「转圈只转一点点」。MCP 执行时登记忙窗口（多给 150ms ramp 余量，
用有符号差值比较避免 tick 回绕），窗口内直接丢弃装饰性动作。

### 新增：全局速度上限

网页「🐢 速度」滑条写入 NVS、断电保持，AI 下发的 `speed_percent` / `throttle` / `steer`
先乘 `(max_speed_percent/100)` 再下发，使滑条对语音 / MCP 指令同样生效，而不只作用于摇杆。

### 修复：关机左轮自转

`MOTOR_LF_GPIO` 从 `GPIO0` 迁到 `GPIO2`。根因是 GPIO0 带板载上拉，断电后把 IN1 撑在高电平
（IN1=1 / IN2=0 → 左轮正转）。4 路电机 GPIO 同时启用内部下拉；断电后内部下拉失效，
根治仍需 GPIO2→GND 的 10k 外部电阻。

### 其他

- `spin_around` 复用 `MOTOR_TURN_LEFT`（双轮反向原地转），与 `turn_left` 仅时长不同
- 新增 C 接口包装，板级 MCP 工具 / `web_server` 无需 include `application.h`

---

## 2026-10-16（提交 `427daf0`）

### 修复：退出控制模式后语音唤醒失效

`DeviceStateMachine::TransitionTo()` 对「已是目标状态」直接返回、不触发状态变更回调，
导致 idle 分支里的 `EnableWakeWordDetection(true)` 从未执行。改为在
`OnControlModeChange(false)` 里显式恢复语音处理 + 唤醒词检测。

### 修复：定时器任务自杀导致 `session_mutex_` 永久死锁

`control_mode_timer_task` / `session_timer_task` 到点后自己调 `End*()`，而 `End*()` 内部
`Cancel*Timer()` 会 `vTaskDelete(自己)` → `xSemaphoreGive()` 永远执行不到 → 锁被永久带走 →
之后每次唤醒都以 `portMAX_DELAY` 阻塞、主事件循环挂死，只能断电重启。
改为两段式：持锁只改状态位，放锁后再回调 + 取消定时器。

（该提交另含安全加固与代码清理，详见 `git log 427daf0`。）

---

## 烧录说明

- **整包**：`write_flash 0x0 firmware-merged-YYYYMMDD.bin`
- **分包**：

  ```
  0x0      bootloader.bin
  0x8000   partition-table.bin
  0xd000   ota_data_initial.bin
  0x10000  xiaozhi.bin
  0x280000 generated_assets.bin
  ```

- 本项目分区表只有 `factory`，**无 OTA 分区**，服务端 OTA 会跳过（`Running from factory partition, skipping`），升级只能 USB 刷。
