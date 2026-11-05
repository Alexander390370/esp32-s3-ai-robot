// ===========================================================================
// 机器人配置 - 编译前按需修改
// ===========================================================================
// WiFi 不写死：首次开机用串口配网（见 wifi_config.h），
// 配好后存在 NVS，换网络可以在串口重新配，不用重新编译。
// ===========================================================================
#pragma once

// ---- 引脚定义 ----

// MX1508 电机驱动（无 ENA，PWM 直接给 IN 引脚）
//   IN1/IN2 控制左轮，IN3/IN4 控制右轮
//   IN1=1,IN2=0 正转 / IN1=0,IN2=1 反转 / 都0 停 / IN1=PWM 正转调速
#define PIN_MOTOR_L1      6
#define PIN_MOTOR_L2      7
#define PIN_MOTOR_R1      15
#define PIN_MOTOR_R2      16

// ---- 电机方向修正 ----
// 实测组装情况：
//   显示屏那面 = 车头，万向轮那端 = 车尾
//   左轮的白色尾盖朝内，右轮的白色尾盖朝外
//   → 两个电机安装方向相反 → 物理"正转"是相反方向
//   → 必须把其中一个的 dir 取反，两者才能同向
//
// 修正方式：给右轮加反相
#define MOTOR_L_INVERT    0    // 左轮：不翻转
#define MOTOR_R_INVERT    1    // 右轮：翻转（因为装反了）

// 左右接线是否互换（如过 IN1/IN2 和 IN3/IN4 接反了就设 1）
#define MOTOR_SWAP_LR     0


// OLED SSD1306 (I2C)
#define PIN_I2C_SDA       8
#define PIN_I2C_SCL       9
#define OLED_ADDR         0x3C
#define OLED_WIDTH        128
#define OLED_HEIGHT       64

// HC-SR04 超声波
//   ⚠️ Echo 是 5V 输出，必须经 1k/2k 分压到 3.3V 再接 ESP32
#define PIN_ULTRASONIC_TRIG   4
#define PIN_ULTRASONIC_ECHO   5

// HC-SR501 人体红外
//   ⚠️ 不能用 GPIO6/7 —— 那两个已被电机 IN1/IN2 占用
#define PIN_PIR               11

// HW-072 光敏模块（数字输出）
#define PIN_LIGHT             12

// 蜂鸣器 / 继电器
#define PIN_ALARM             15

// INMP441 麦克风 (I2S 输入)
#define PIN_I2S_MIC_SCK   17
#define PIN_I2S_MIC_WS    18
#define PIN_I2S_MIC_SD    21

// MAX98357 功放 (I2S 输出)
#define PIN_I2S_SPK_BCLK  41
#define PIN_I2S_SPK_LRC   42
#define PIN_I2S_SPK_DIN   40

// ---- MQTT（PC 端大脑） ----
// 改成你 PC 的局域网 IP（ipconfig 查 WLAN 的 IPv4 地址）
// 查询方法：cmd 里跑 ipconfig，看 WLAN 那块的 IPv4 地址。
// 或者跑 PC 端的 start.py，它会打印出来。
#define MQTT_HOST         "192.168.1.100"
#define MQTT_PORT         1884
#define MQTT_USERNAME     ""
#define MQTT_PASSWORD     ""
#define MQTT_TOPIC_CMD    "robot/cmd"      // PC → 机器人
#define MQTT_TOPIC_STATE  "robot/state"    // 机器人 → PC
#define MQTT_TOPIC_AUDIO  "robot/audio"   // 机器人 → PC（录音）
#define MQTT_TOPIC_SPEAK  "robot/speak"   // PC → 机器人（要播放的音频）

// ---- 行为参数 ----
#define DANGER_DISTANCE_CM    20      // 报警距离阈值
#define OBSTACLE_AVOID_CM      6      // 6cm 内才躲（2-3 太近，15 太远）
#define OBSTACLE_BACK_MS       150     // 后退 150ms ≈ 3-5cm 位移
// 速度：占空比百分比
//   实测 40% 力矩不够会卡住，60% 能转但不快
//   80% 是当前调好的值
//   MX1508 的 VCC 如果改接 9V（不是 5V），动力会再翻倍
#define MOTOR_SPEED_CRUISE    80      // 巡航
#define MOTOR_SPEED_TURN      75      // 转向
#define MOTOR_SPEED_SLOW      70      // 慢速（避障时微调）

// 后退补偿：实测后退动力明显不如前进
//   （MX1508 两路导通电阻不同 + 电机碳刷两向磨损不一致）
//   后退时把占空比再乘这个系数，100 表示不补偿
#define MOTOR_BACK_BOOST     100      // 后退补偿系数（100 = 不加成）

// 电机调试日志（排查方向问题时开了看）
#define MOTOR_DEBUG       0
#define SAMPLE_INTERVAL_MS   300      // 传感器采样周期
#define OBSTACLE_COOLDOWN_MS  1200    // 避障冷却
#define TURN_DURATION_MS      450     // 小角度转开就够

// ---- 音频 ----
#define I2S_SAMPLE_RATE      16000   // ASR 用 16kHz
#define AUDIO_RECORD_SECONDS 4       // 单次录音时长
#define AUDIO_CHUNK_SIZE    1024    // 每次上传的字节数

// ---- 自动对话模式（VAD 语音活动检测）----
// 不用手发 REC，直接说话就能对话
// 麦克风增益各批不同，阈值要按实测调：
//   串口发 CHAT 开对话模式，又说又停，看日志里的 rms
//   静音时 rms 约 X，说话时约 3-5X，阈值取中间
#define VAD_THRESHOLD        1500    // 兜底阈值（校准失败时用）
#define VAD_NOISE_MULT       1.6f   // 底噪倍数（3.0 太严，说话稍弱就掉）
#define VAD_MIN_THRESHOLD     2500   // 阈值下限
#define VAD_SILENCE_MS         700    // 静音 700ms 算说完
#define VAD_MIN_SPEECH_MS      300    // 最短 300ms（100ms 太短被丢）
#define VAD_MAX_RECORD_MS     8000   // 最长 8 秒