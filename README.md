# ESP32-S3 AI 语音机器人

<p align="center">
  <img src="https://img.shields.io/badge/ESP32--S3-N16R8-blue?logo=espressif&logoColor=white" alt="ESP32-S3">
  <img src="https://img.shields.io/badge/Framework-Arduino-00979D?logo=arduino&logoColor=white" alt="Arduino">
  <img src="https://img.shields.io/badge/Build-PlatformIO-orange?logo=platformio&logoColor=white" alt="PlatformIO">
  <img src="https://img.shields.io/badge/Python-3.10%2B-3776AB?logo=python&logoColor=white" alt="Python">
  <img src="https://img.shields.io/badge/MQTT-paho--2.x-660066?logo=mqtt&logoColor=white" alt="MQTT">
  <img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/ASR-Whisper-412991?logo=openai&logoColor=white" alt="Whisper">
  <img src="https://img.shields.io/badge/LLM-DeepSeek-4D6BFE" alt="DeepSeek">
  <img src="https://img.shields.io/badge/TTS-edge--tts-00A4EF?logo=microsoft&logoColor=white" alt="edge-tts">
  <img src="https://img.shields.io/badge/I2S-INMP441%20%2B%20MAX98357-green" alt="I2S">
  <img src="https://img.shields.io/badge/Motor-MX1508-red" alt="Motor">
  <img src="https://img.shields.io/badge/Display-SSD1306-black" alt="Display">
</p>

> 一台能看、能听、能说、能跑的小车。ESP32-S3 做本体，PC 做大脑，通过 WiFi + MQTT 通信。

```
┌─────────────────────────┐                    ┌──────────────────────┐
│   ESP32-S3 机器人        │      WiFi/MQTT     │   PC 大脑             │
│                         │                    │                      │
│  OLED 表情（7 种）       │ ◄──── 状态/音频 ────┤  Whisper ASR         │
│  超声波避障             │                    │       ↓              │
│  MX1508 差速转向        │                    │  DeepSeek LLM        │
│  INMP441 麦克风          │ ──── 录音 ────────►│       ↓              │
│  MAX98357 功放 + 喇叭   │                    │  edge-tts 语音合成    │
│                         │ ◄── 音频/指令 ─────┤                      │
└─────────────────────────┘                    └──────────────────────┘
                                                          │
                                                     手机网页遥控
```

## 目录

- [能力](#能力)
- [硬件](#硬件)
- [快速开始](#快速开始)
- [串口命令](#串口命令)
- [踩过的坑](#踩过的坑)
- [项目结构](#项目结构)
- [已知限制](#已知限制)

## 详细文档

| 文档 | 内容 |
|---|---|
| [docs/01-硬件与接线.md](docs/01-硬件与接线.md) | 缺件清单、接线表、7 步接线顺序、超声波分压、上电前检查 |
| [docs/02-电机调试.md](docs/02-电机调试.md) | MX1508 焊接（10 个焊点）、方向校准、动力调优、调试命令 |
| [docs/03-烧录与配置.md](docs/03-烧录与配置.md) | 烧录流程、Write timeout 处理、串口无输出、PSRAM 配置、正常启动日志 |
| [docs/04-语音链路.md](docs/04-语音链路.md) | 麦克风 I2S、音频传输、Whisper 识别、edge-tts 合成、VAD 自动对话 |
| [docs/05-调试实战.md](docs/05-调试实战.md) | 8 个真实 bug 的完整排查过程（JSON解析、双重编码、IPv6、削波…） |


## 能力

| 能力 | 实现 |
|---|---|
| **看** | OLED 显示 7 种情绪表情，随 AI 回复变化 |
| **听** | INMP441 I2S 麦克风 + Whisper 本地识别 |
| **说** | edge-tts 微软神经网络语音（接近真人）→ MAX98357 → 喇叭 |
| **动** | MX1508 双路电机驱动，差速转向，超声波自动避障 |
| **想** | DeepSeek / 通义 / 豆包 / 智谱 / 阶跃 云端大模型 |
| **控** | 手机浏览器网页遥控（方向键 + 对话 + 调试面板） |

AI 输出的不是文字，而是结构化指令：

```json
{
  "action": "move_forward",
  "speed": 80,
  "duration_ms": 2000,
  "expression": "happy",
  "speech": "好嘞，小蓝出发啦！"
}
```

所以它能**一边动、一边变表情、一边说话**。

## 硬件

### 必需

| 模块 | 型号 | 说明 |
|---|---|---|
| 主控 | ESP32-S3 N16R8 | 16MB Flash + 8MB PSRAM（必须 Octal PSRAM） |
| 表情屏 | SSD1306 0.96" | I2C 4 针 |
| 超声波 | HC-SR04 | 3.3V 供电可省掉分压电阻 |
| 电机驱动 | MX1508 | 双 H 桥，无 ENA，PWM 直接给 IN |
| 麦克风 | INMP441 | I2S 数字麦克风，6 针 |
| 功放 | MAX98357 | I2S 数字功放，7 针 |
| 喇叭 | 2535 4Ω 3W | 接功放绿色端子 |
| 底盘 | 2WD + 万向轮 | 含 TT 电机 ×2 |
| 电源 | 6 节 AA (9V) + HW-131 | HW-131 出 5V / 3.3V |

### 可选

| 模块 | 用途 |
|---|---|
| HC-SR501 | 人体红外 |
| HW-072 | 光敏 |
| ESP32-CAM | 视觉（固件暂未接入） |

### 接线

```
上 + 轨 = 5V    → ESP32 VIN、MX1508 VCC、HC-SR04 VCC、MAX98357 VIN
上 − 轨 = GND   → 所有 GND
下 + 轨 = 3.3V  → OLED VCC、INMP441 VDD
```

| 模块 | 引脚 | → | ESP32-S3 |
|---|---|---|---|
| OLED | SDA / SCL | → | GPIO8 / GPIO9 |
| HC-SR04 | Trig / Echo | → | GPIO4 / GPIO5 |
| MX1508 | INT1-4 | → | GPIO6 / 7 / 15 / 16 |
| INMP441 | SCK / WS / SD / L/R | → | GPIO17 / 18 / 21 / GND |
| MAX98357 | BCLK / LRC / DIN | → | GPIO41 / 42 / 40 |

> **INMP441 的 L/R 必须接 GND**，悬空会输出左右声道交错的乱码。
>
> **喇叭两根线都不能接地**，MAX98357 是 BTL 输出，接地会烧功放。

## 快速开始

### 1. 烧录固件

```bash
cd firmware
cp src/config.example.h src/config.h    # 改 WiFi / MQTT IP / 引脚
pio run -t upload --upload-port COM8
```

如果报 `Write timeout`：

```
1. 按住 BOOT
2. 点一下 RST
3. 松开 BOOT
4. powershell -ExecutionPolicy Bypass -File ..\flash.ps1
```

### 2. 配 WiFi

串口监视器（115200）输入：

```
WIFI 你的WiFi名 你的密码
```

配置会存进 NVS，下次自动连接。换网络就再输一次。

### 3. 启动 PC 端

```bash
cd pc
cp config.example.py config.py          # 填 API Key
cd ..
python start.py
```

会打印：

```
手机浏览器打开: http://192.168.1.100:8765
机器人固件里 MQTT_HOST 填: 192.168.1.100
```

### 4. 手机遥控

手机连同一个 WiFi，浏览器打开上面的地址。

## 串口命令

| 命令 | 作用 |
|---|---|
| `WIFI <ssid> <pass>` | 配 WiFi |
| `RESET_WIFI` | 清除 WiFi |
| `STATUS` | 打印传感器读数 |
| `REC` | 录音 4 秒并上传识别 |
| `CHAT` | 开关自动对话模式（VAD） |
| `AUTO` / `MANUAL` | 自动避障 / 手动 |
| `STOP` | 立即停车 |
| `FWD` / `BACK` / `LEFT` / `RIGHT` | 四方向校准（各 1-2 秒） |
| `L+80` / `L-80` / `R+80` / `R-80` | 单轮调试 |
| `RAW+` / `RAW-` | 原始 GPIO（绕过所有修正） |

## 踩过的坑

这些都是实际调试中浪费过时间的，记下来省得重复踩。

### 1. 串口看不到任何输出

这块 S3 有两个 USB 口，`Serial` 指向哪个取决于 `platformio.ini`：

| 插的口 | 出现的端口 | 配置 |
|---|---|---|
| UART 口（CH343） | COM8 | `ARDUINO_USB_CDC_ON_BOOT=0` |
| USB 口（原生 JTAG） | COM10 | `ARDUINO_USB_CDC_ON_BOOT=1` |

配错就是——端口在、能烧录、但串口一个字都没有。

### 2. PSRAM 用不了（只剩 320KB 堆）

```ini
board_build.arduino.memory_type = qio_opi
```

N16R8 的 8MB PSRAM 是 **Octal (OPI)**，不是 QSPI。不写这行会报：

```
E psram: PSRAM ID read error: 0x00ffffff, PSRAM chip not found
```

### 3. 烧录报 Write timeout

S3 的 USB-Serial/JTAG 口在板子反复重启时抓不到握手窗口。手动进下载模式 + `--before no_reset`。

### 4. 声音低沉发闷

```
TTS 合成 22050Hz，固件按 16000Hz 播 → 音调被拉低 27%
```

修法：TTS 强制输出 16kHz。

### 5. 声音机械死板

SAPI 的 Huihui 是 2010 年代的老语音。换 edge-tts（微软在线神经网络语音，8 个中文声音）。

### 6. 喇叭没声音（只收到 826 字节）

`PubSubClient` 接收 buffer 默认 2048 字节，30KB 的音频块被截断。改 `setBufferSize(32768)`。

### 7. 喇叭只播一小段就停

每块音频到达就播放，没有等收完。改成累积到缓冲、600ms 没新块才一次性播。

### 8. 录音后半段全是垃圾数据

按 1024 字节分块 + `delay(5)` 发 125 块，625ms 期间 I2S 还在采集，DMA 被覆盖。

修法：块大小改 30000 字节，`delay` 只给 1ms。

### 9. 麦克风满幅削波（峰值恒定 32768）

INMP441 是 24 位器件，按 16 位读会位对齐错误。

修法：读 32 位，右移 14 位。
- 移太少（11）→ 直流偏置占满量程，削波
- 移太多（18）→ 有效信号太小

### 10. 电机吱吱响但轮子不动

不是电机坏了，也不是焊接问题——**占空比太低，力矩不足**。

```
5V → MX1508 压降 0.5V → 电机 4.5V
4.5V × 40% = 等效 1.8V，带不动 1:48 减速箱
```

手拨一下能转 = 外力帮忙突破静摩擦，最容易误判成机械卡死。

修法：基础速度提到 75-80%，并加满力矩起步。

```cpp
void forwardKick(int speed, int kickMs = 250) {
  driveSide(0, 1, 100);   // 先 100% 冲一下突破静摩擦
  driveSide(1, 1, 100);
  delay(kickMs);
  forward(speed);         // 再降到目标速度
}
```

### 11. 上电就自己往前冲

默认模式是 `MODE_AUTO`，一通电自动避障就动了。改成默认 `MODE_MANUAL` 停车。

### 12. 后退变前进 / 原地打转

`backward()` 里如果统一取 `dir = +1` 来"翻转方向"，会绕过右轮的 `R_INVERT`，
变成一侧往前一侧往后 → 原地打转。

必须让每侧各自走 `driveSide()`，invert 只在该函数内部生效。

### 13. 网页端口打不开（WinError 10013）

8080 在 Windows 保留端口段。改 8765。

### 14. Python 3.13+ 的 SimpleHTTPRequestHandler

`super().__init__(*a, directory=...)` 已移除，会静默失败（端口在监听但不返回数据）。
自己实现静态文件分发。

### 15. whisper 识别为空

whisper 的 VAD 很保守，音量偏低时直接判「无语音」。

```python
model.transcribe(path, no_speech_threshold=1.0,
                 logprob_threshold=None, temperature=0.0)
```

### 16. %TEMP% 里的同名 .py 会遮蔽标准库

在 TEMP 下跑过 `import wave` 之类的测试脚本，同名文件会覆盖标准库，报出完全无关的错误。

### 17. 某个方向单独能跑，连着跑就失灵

最容易误判的一条。

```
单独测 FWD  → 正常
单独测 BACK → 正常
连续测 FWD → BACK → LEFT → RIGHT
  → 后退和右转失灵
```

**根因**：`stop()` 用 `digitalWrite(LOW)` 关引脚，但之前 `analogWrite`
设的 PWM 占空比寄存器还留着旧值。紧接着做反向动作时 MX1508 收到
冲突电平，表现为不动。

**修法**：`stop()` 里先用 `analogWrite(pin, 0)` 清占空比，再 `digitalWrite(LOW)`；
动作之间加 `delay(300)` 静默期。

详见 [docs/02-电机调试.md](docs/02-电机调试.md#六最隐蔽的坑动作之间会失效)。

### 18. JSON 解析漏了一个 `p++`，所有动作指令失效

```cpp
p = json.indexOf(':', p + k.length());   // p 指向冒号
while (json[p] == ' ' || json[p] == '\t') p++;   // ❌ 没跳过冒号
if (json[p] != '"') return false;                 // 永远返回 false
```

现象最迷惑：MQTT 消息收到了、JSON 格式对、对话能用，**只有遥控指令失效**。
因为对话走 `speech` → PC TTS → `robot/speak`，不经过这个解析。

### 19. HTTP 响应双重 JSON 编码，网页读不到状态

```python
json.dumps(已经是字符串的数据)   # ❌ 外面多一层引号
```

前端 `JSON.parse()` 拿到字符串而非对象，所有字段都是 `undefined`。
排查方法：用原始 socket 看 body 第一个字符是 `{` 还是 `"`。

### 20. SAPI 回退时没初始化 COM

```
[TTL] SAPI 不可用: (-2147221008, '尚未调用 CoInitialize。')
```

`reply()` 跑在子线程，Windows COM 要求**每线程单独初始化**。
`_synth_sapi()` 里要 `pythoncom.CoInitialize()`。

### 21. IPv6 路由不通，edge-tts 成功率只有 33%

**音色时好时坏**：短句是甜甜的女声，长句是机械的女声。
根因：DNS 返回 AAAA 记录，aiohttp 优先连 IPv6，但路由不通。

修法（进程级 patch，所有网络库受益）：

```python
_orig = socket.getaddrinfo
def _v4(host, port, family=0, type=0, proto=0, flags=0):
    r = _orig(host, port, family, type, proto, flags)
    v4 = [x for x in r if x[0] == socket.AF_INET]
    return v4 if v4 else r
socket.getaddrinfo = _v4
```

效果：成功率 33% → **100%**，耗时 1.9 秒 → **1.3 秒**。

### 22. 软件音量放大导致滋滋电流声

```cpp
int32_t v = src[i] * 150 / 100;
if (v > 32767) v = 32767;    // 大量采样被压平 → 方波 → 滋滋声
```

**不要用软件放大**。想更响改硬件：MAX98357 的 GAIN 接 GND（9dB → 15dB）。

### 23. 喇叭只播第一块 1024 字节

两个 bug 叠加：

1. `spkReset()` 没清 `g_spkLast` → 下一轮第一块到达时 `millis() - 0 > 900` 立刻成立
2. `publishPcm()` 上传后把 buffer 设回 **2048**，但 SPEAK 每块是 **8192** → 收不下

修法：用显式 `g_spkActive` 标记 + buffer 恢复成 32768。

### 24. 避障疯狂转圈

避障动作耗时 830ms，采样周期只有 300ms → 动作没做完又触发一次。

修法：`g_avoiding` 锁 + 避障期间跳过超声波采样。
效果：22 秒内从十几次触发降到 **2 次**。

### 25. VAD 阈值太严，说话检测不到

```
[对话] 检测到说话 (rms=16219, 阈值=15975)
[对话] 太短忽略 (说话 100ms)
```

两个问题：
- 阈值 = 底噪 × 3.0 太严，说话稍弱就卡边缘
- 「太短」直接丢弃，没有宽容期（人说话有词间停顿）

修法：倍数降到 1.6，加 1.5 秒宽容期。

## 项目结构

```
robot-car/
├── firmware/
│   ├── platformio.ini
│   └── src/
│       ├── main.cpp        主程序
│       ├── config.example.h 配置模板（复制成 config.h）
│       ├── motor.h         MX1508 驱动 + 方向修正
│       ├── face.h          OLED 表情
│       ├── ultrasonic.h    超声波
│       ├── audio_io.h      I2S 麦克风 + 功放
│       ├── chat_mode.h     VAD 自动对话
│       └── wifi_config.h   串口配网
├── pc/
│   ├── brain.py            AI + ASR + TTS + MQTT + 网页服务
│   └── config.example.py   配置模板（复制成 config.py）
├── web/index.html          手机遥控界面
├── flash.ps1               强制烧录脚本
└── start.py                一键启动
```

## 依赖

```
paho-mqtt>=2.0      # 2.x 才有 CallbackAPIVersion.VERSION2
requests
pywin32             # Windows TTS（SAPI 兜底）
openai-whisper      # 语音识别
edge-tts            # 语音合成
```

`edge-tts` 需要 **ffmpeg** 把 MP3 转成 PCM。

## 已知限制

- **对话延迟约 3-4 秒**（ASR 1.5s + AI 1.4s + TTS 1.3s）
- **edge-tts 需要联网**。已在启动时强制 IPv4，成功率从 33% 提到 100%
- **音量偏小**：MAX98357 的 GAIN 脚悬空是 9dB，接 GND 可到 15dB（约 2 倍）。
  不要用软件放大——会削波成电流声
- **无唤醒词**：需要串口发 `CHAT` 或网页点击开启对话模式
- **摄像头未接入**：ESP32-CAM 硬件已在，固件待开发
- **whisper small 模型**：准确率约 70%，`medium` 下载总失败（1.4GB）

## 实测性能

| 环节 | 耗时 | 说明 |
|---|---|---|
| VAD 录音 | 1-8 秒 | 取决于说多久 |
| ASR (whisper small) | 1.5 秒 | base 只要 0.5 秒但准确率 50% |
| AI (deepseek-chat) | 1.4 秒 | 台词限制 20 字内 |
| TTS (edge 晓晓) | 1.3 秒 | IPv4 修正后从 1.9 秒降到 1.3 秒 |
| MQTT + 播放 | 0.3 秒 | 8192 字节/块 |
| **端到端** | **约 4.5 秒** | 不含说话和播放时长 |
## License

MIT
