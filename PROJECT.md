# 小智语音小车（C3 版）

基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 定制的两轮差速语音小车。板型 `qebabe-xiaoche-c3`，ESP32-C3 + OLED + 麦克风/功放 + MX1508 双 H 桥驱动。

和原版的区别：**在原版「语音对话」的基础上加了一整套动作系统**——既能执行 21 个预设动作，也能让大模型自己编排任意动作序列。

---

## 1. 硬件

| 项目 | 型号 / 引脚 |
|---|---|
| 主控 | ESP32-C3-MINI1（4MB Flash，单核 160MHz） |
| 麦克风 | MicroDeer I2S 模块（SCK=4, WS=5, SD=6） |
| 功放 | I2S 输出（BCLK=10, LRCK=20, DIN=21） |
| 显示 | 0.96" SSD1306 OLED 128×64（I2C：SCL=8, SDA=9，地址 0x3C） |
| 电机驱动 | HW-358 / MX1508 双 H 桥（无 ENA/ENB，4 线直控） |
| 电机 | 左右各一个 DC 减速电机，差速转向 |
| 按键 | 板载 BOOT 键（GPIO0，active_low） |
| 供电 | 单节锂电池 + LDO，电机与主控共电源 |

### 引脚分配（12 个可用 IO 全部分配完）

```
电机  LF=2  LB=1  RF=3  RB=7      (MX1508 IN1~IN4)
麦克风 SCK=4  WS=5  SD=6
功放   BCLK=10  LRCK=20  DIN=21
OLED   SCL=8  SDA=9
按键   BOOT=0
```

⚠️ GPIO18/19 是 USB D-/D+，绝对不能用；GPIO20/21 原为 UART console，已让给 I2S（日志走 USB CDC）。

---

## 2. 功能

- **语音对话**：本地唤醒词「你好小智」（WakeNet9），经 WiFi 连 MQTT/WebSocket 到服务端，服务端大模型识别意图并通过 MCP 工具回调控制小车。
- **动作系统**：预设动作 + 差速摇杆 + 多步序列（详见第 4 节）。
- **网页控制**：板子起 80 端口 Web 服务，手机/电脑浏览器可摇杆实时驾驶、调速、配置电机接线修正、看状态。
- **单键三用**：BOOT 键 = 唤醒 / 打断播报 / 退出聆听回时钟。
- **OLED 表情**：RoboEyes 风格的眼睛（眨眼、追焦、情绪动画）+ 说话时开合的嘴 + 待机时钟。
- **拟人反馈**：唤醒、开始/结束说话、各情绪都有小幅电机动作配合。

---

## 3. 控制方式

| 方式 | 入口 | 说明 |
|---|---|---|
| 语音 | 唤醒后说自然语言 | 大模型自己决定调哪个工具、什么参数 |
| 网页 | `http://<设备IP>/` | 摇杆实时差速、速度上限滑条、接线修正勾选 |
| 按键 | BOOT 单击 | 唤醒 / 打断 / 回时钟 |

---

## 4. 动作系统（核心）

分三层，从上到下：

### 4.1 底层：5 个电机原语

`Motor2WD` 只认 5 个方向：停、前、后、左转、右转。左右转是**双轮反向的原地转**（不是单轮转）。

所有输出都经过 `DriveRamped()`：软启动 ramp、85% 占空比上限、右轮错峰启动——目的是压住双电机同时启动时的电流尖峰，避免 300mAh 小电池电压跌落导致主控复位。

### 4.2 中层：参数化差速

`Drive(steer, throttle, duration_ms)`：
- `steer` -100..100，正=右、负=左、±100 = 原地转
- `throttle` -100..100，正=前、负=后
- 两者混控成左右轮速度，超幅时**等比缩放**（保差速比，不硬砍单边）
- 速度曲线 40% 三次方 + 60% 线性，小舵柔、大舵猛

### 4.3 上层：预设动作 + 序列

**预设动作（`self.car.act`，21 个）**：forward / backward / spin_around / turn_left / turn_right / quick_forward / quick_backward / wiggle / dance / stop / wake_up / happy / sad / thinking / listening / speaking / excited / loving / angry / surprised / confused。

支持可选 `duration_ms` 覆盖默认时长，例如「跳 5 秒」= `action=dance, duration_ms=5000`（舞蹈会循环跳满 5 秒）。

**多步序列（`self.motor.sequence`）**：一次下发整段动作，固件内背靠背执行，段间零网络延迟。

```
steps = "steer,throttle,ms;steer,throttle,ms;..."
例：画圆  "0,80,800;70,80,900;0,80,800;-70,80,900"
```

最多 24 段、总时长 ≤15 秒。执行体在独立的低优先级任务 `m2wd_seq` 里，**不占用主循环**（MCP 工具回调跑在 Application 主线程，不能阻塞）。

### 4.4 MCP 工具清单

| 工具 | 用途 |
|---|---|
| `self.motor.sequence` | 多步连续动作（首选） |
| `self.motor.drive` | 单步差速移动 |
| `self.motor.move_forward` / `move_backward` / `turn_left` / `turn_right` / `stop` | 单动作 + 速度/时长 |
| `self.car.act` | 21 个预设动作 + 表情联动 |
| `self.sensor.get_distance` | 距离传感器探测（VL53L0X，仅检测在位，避障逻辑待开发） |
| `self.network.get_ip` | 查询设备 IP |

---

## 5. 显示

一颗 SSD1306 只有一条渲染入口 `OledDisplay::RenderTick()`，按优先级：音乐 → 表情 → 时钟，末尾统一推屏。

- **待机**：时钟 + 日期；未连 WiFi 显示 `WiFi...`，未对时显示 `NTP...`
- **表情**：复用 FluxGarage RoboEyes 的眼睛逻辑（眨眼、追焦），8 种情绪
- **音乐**：检测到服务端在放歌时整屏显示双音符 ♫ + 歌名
- 刷新 10Hz，刷屏走 I2C 阻塞传输，放在 `esp_timer` 回调里——频率过高会挤占音频任务、拖慢唤醒响应

---

## 6. 编译与烧录

环境：ESP-IDF v5.5.4，目标 `esp32c3`。

```powershell
# 编译：项目根目录直接跑 _rebuild.ps1（已封装环境变量），或手动执行：
$env:IDF_PATH="C:/Espressif/frameworks/esp-idf-v5.5.4"
$env:IDF_TOOLS_PATH="C:/Espressif"
$env:IDF_PYTHON_ENV_PATH="C:/Espressif/python_env/idf5.5_py3.11_env"
$env:PATH="C:/Espressif/python_env/idf5.5_py3.11_env/Scripts;C:/Espressif/tools/cmake/3.30.2/bin;C:/Espressif/tools/ninja/1.12.1;"+$env:PATH
python "C:/Espressif/frameworks/esp-idf-v5.5.4/tools/idf.py" -B build_c3 build

# 烧录（COM 口按实际改）
python -m esptool -p COM19 --chip esp32c3 -b 460800 --after hard_reset write_flash `
  0x0      build_c3/bootloader/bootloader.bin `
  0x8000   build_c3/partition_table/partition-table.bin `
  0xd000   build_c3/ota_data_initial.bin `
  0x10000  build_c3/xiaozhi.bin `
  0x280000 build_c3/generated_assets.bin
```

---

## 7. 资源占用

| 项 | 大小 | 占用率 |
|---|---|---|
| 固件 `xiaozhi.bin` | 2.17 MiB | **app 分区 89%**（余 274 KiB） |
| 资源 `generated_assets.bin` | 389 KiB | assets 分区 25% |
| 完整烧录包 | 2.58 MiB | — |
| 运行空闲 SRAM | ~103 KB | — |

---

## 8. 已知限制

- **只有 4MB Flash 且无 OTA 分区**：分区表只有 `factory`，服务端 OTA 会跳过（日志 `Running from factory partition, skipping`），升级只能 USB 刷。
- **只有一条 I2C 总线**：OLED 已占用 `I2C_NUM_0`，任何新 I2C 设备必须挂在同一条线上（地址不能冲突）。
- **设备端不能主动说话**：上行协议只有 `abort` / `listen` / `mcp` 三种消息，没有「让服务端 TTS 说一句」的接口。本地提示只能用预置 OGG 音效。
- **近距盲区**：VL53L0X 在 3~5cm 内会返回超量程 8191，做避障时需单独处理，否则会把贴脸障碍误判成跌落。
- **app 分区余量不足 300 KiB**，继续加功能前需评估空间与 SRAM。
