// ===========================================================================
// 双节点边缘 AI 安防机器人 - 机器人端固件
// ===========================================================================
//
// 功能：
//   1. WiFi（串口配网，不写死）
//   2. MQTT 连 PC 端大脑，接收动作指令，回报状态
//   3. OLED 表情（随情绪变化）
//   4. 超声波测距 + 自动避障
//   5. I2S 麦克风录音 → 上传 PC 做语音识别
//   6. I2S 功放播放 PC 返回的音频
//
// 指令格式（JSON，来自 MQTT topic: robot/cmd）：
//   {"action":"move_forward","speed":60,"duration_ms":2000,
//    "expression":"happy","speech":"好嘞"}
//
// 状态上报（MQTT topic: robot/state）：
//   {"dist":195,"pir":0,"light":0,"battery":1,"emotion":"idle",
//    "mode":"auto","rssi":-45}
//
// ===========================================================================

#include <Arduino.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Wire.h>
#include <vector>

#include "audio_io.h"
#include "chat_mode.h"
#include "config.h"
#include "face.h"
#include "motor.h"
#include "ultrasonic.h"
#include "wifi_config.h"

// ---- 全局对象 ----
Motor        g_motor;
Ultrasonic   g_ultra;
Face         g_face;
AudioIO      g_audio;
ChatMode     g_chat;
WiFiClient   g_net;
PubSubClient g_mqtt(g_net);

// ---- 状态 ----
enum Mode { MODE_AUTO, MODE_MANUAL };
// 默认手动：上电不乱跑。
// 之前默认 AUTO，一通电就自动避障往前冲，调车时很危险。
static Mode   g_mode = MODE_MANUAL;
static int    g_dist = 999;
static int    g_pir = 0;
static int    g_light = 0;
static uint32_t g_lastAvoid = 0;
static uint32_t g_lastState = 0;
static uint32_t g_actionUntil = 0;      // 当前动作结束时间
static String  g_currentAction = "idle";
static uint32_t g_wifiRetryAt = 0;

// 串口接收缓冲（配网用）
static char g_serialBuf[128];
static uint8_t g_serialLen = 0;

// ===========================================================================
// 表情映射：AI 返回的字符串 → 枚举
// ===========================================================================
static Emotion parseEmotion(const String &e) {
  if (e == "happy")     return EMO_HAPPY;
  if (e == "sad")       return EMO_SAD;
  if (e == "surprised") return EMO_SURPRISED;
  if (e == "thinking")  return EMO_THINKING;
  if (e == "angry")     return EMO_ANGRY;
  if (e == "sleepy")    return EMO_SLEEPY;
  return EMO_IDLE;
}

// ===========================================================================
// 极简 JSON 取值（只取顶层字符串/数字，不引第三方库）
// ===========================================================================
static bool jsonGetString(const String &json, const char *key, String &out) {
  String k = String("\"") + key + "\"";
  int p = json.indexOf(k);
  if (p < 0) return false;
  p = json.indexOf(':', p + k.length());
  if (p < 0) return false;
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
  if (p >= (int)json.length() || json[p] != '"') return false;
  p++;
  out = "";
  while (p < (int)json.length() && json[p] != '"') {
    if (json[p] == '\\' && p + 1 < (int)json.length()) p++;
    out += json[p];
    p++;
  }
  return true;
}

static bool jsonGetInt(const String &json, const char *key, int &out) {
  String k = String("\"") + key + "\"";
  int p = json.indexOf(k);
  if (p < 0) return false;
  p = json.indexOf(':', p + k.length());
  if (p < 0) return false;
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
  out = 0;
  bool neg = false;
  if (p < (int)json.length() && json[p] == '-') { neg = true; p++; }
  while (p < (int)json.length() && json[p] >= '0' && json[p] <= '9') {
    out = out * 10 + (json[p] - '0');
    p++;
  }
  if (neg) out = -out;
  return true;
}

// ===========================================================================
// 执行动作
// ===========================================================================
static void doAction(const String &action, int speed, int durationMs) {
  if (speed < 0) speed = 0;
  if (speed > 100) speed = 100;
  if (durationMs < 0) durationMs = 0;
  if (durationMs > 10000) durationMs = 10000;

  g_currentAction = action;
  g_actionUntil = millis() + durationMs;

  if (action == "move_forward") {
    g_motor.forwardKick(speed);
  } else if (action == "move_back") {
    g_motor.backwardKick(speed);
  } else if (action == "turn_left") {
    g_motor.turnKick(speed, 1);
  } else if (action == "turn_right") {
    g_motor.turnKick(speed, -1);
  } else if (action == "stop" || action == "idle") {
    g_motor.stop();
    g_actionUntil = 0;
  } else {
    g_motor.stop();
    g_actionUntil = 0;
  }
}

// ===========================================================================
// 播放缓冲：PC 分块发来的 PCM 先攒起来，收完再一次性交给 I2S
// ===========================================================================
static uint8_t *g_spkBuf = nullptr;
static size_t   g_spkLen = 0;
static size_t   g_spkCap = 0;
static uint32_t g_spkLast = 0;

static void spkReset() {
  if (g_spkBuf) {
    free(g_spkBuf);
    g_spkBuf = nullptr;
  }
  g_spkLen = 0;
  g_spkCap = 0;
}

static bool spkAppend(const uint8_t *data, size_t len) {
  if (len == 0) return true;
  size_t need = g_spkLen + len;
  if (need > g_spkCap) {
    size_t cap = g_spkCap ? g_spkCap : 65536;
    while (cap < need) cap *= 2;
    if (cap > 512 * 1024) {          // 上限 512KB，够 16 秒语音
      Serial.printf("[SPEAK] 缓冲超限 (%u 字节)\n", (unsigned)need);
      spkReset();
      return false;
    }
    uint8_t *nb = (uint8_t *)realloc(g_spkBuf, cap);
    if (!nb) {
      Serial.println("[SPEAK] 内存不足");
      spkReset();
      return false;
    }
    g_spkBuf = nb;
    g_spkCap = cap;
  }
  memcpy(g_spkBuf + g_spkLen, data, len);
  g_spkLen += len;
  g_spkLast = millis();
  return true;
}
// ===========================================================================
// 原始电机驱动（网页调试用）
//   L/R: -100 ~ 100，正数正转、负数反转、0 停
//   直接按物理方向写，绕过 motor.h 的 invert 修正，
//   用来确定这台车每个轮子朝前/朝后到底需要哪组电平。
// ===========================================================================
static void rawDrive(int L, int R) {
  auto one = [](int in1, int in2, int v) {
    if (v > 0) {
      analogWrite(in1, v * 255 / 100);
      digitalWrite(in2, LOW);
    } else if (v < 0) {
      digitalWrite(in1, LOW);
      analogWrite(in2, (-v) * 255 / 100);
    } else {
      digitalWrite(in1, LOW);
      digitalWrite(in2, LOW);
    }
  };
  one(PIN_MOTOR_L1, PIN_MOTOR_L2, L);
  one(PIN_MOTOR_R1, PIN_MOTOR_R2, R);
}

static void handleSerialLine(const String &line);

static void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  String t = topic;
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

  // ---- 动作指令 ----
  if (t == MQTT_TOPIC_CMD) {
    Serial.printf("[CMD] %s\n", msg.c_str());

    String action, expression, speech;
    int speed = MOTOR_SPEED_CRUISE;
    int duration = 1000;

    jsonGetString(msg, "action", action);
    jsonGetInt(msg, "speed", speed);
    jsonGetInt(msg, "duration_ms", duration);
    jsonGetString(msg, "expression", expression);
    jsonGetString(msg, "speech", speech);

    // ---- 原始电机控制（网页调试用）----
    // {"action":"raw_motor","L":80,"R":-80,"ms":1500}
    // L/R: -100 ~ 100，正数正转，负数反转，0 停
    if (action == "raw_motor") {
      int L = 0, R = 0, ms = 1500;
      jsonGetInt(msg, "L", L);
      jsonGetInt(msg, "R", R);
      jsonGetInt(msg, "ms", ms);
      if (ms < 100) ms = 100;
      if (ms > 5000) ms = 5000;

      g_mode = MODE_MANUAL;
      g_actionUntil = 0;

      Serial.printf("[调试] 原始电机 L=%d R=%d %dms\n", L, R, ms);
      rawDrive(L, R);
      delay(ms);
      g_motor.stop();
      Serial.println("[调试] 停");
      g_currentAction = "idle";
      return;
    }

    // ---- 串口命令转发（网页调试用）----
    // {"action":"serial","cmd":"BACKTEST"}
    if (action == "serial") {
      String c;
      jsonGetString(msg, "cmd", c);
      Serial.printf("[调试] 转发命令: %s\n", c.c_str());
      handleSerialLine(c);
      return;
    }

    // 手动模式下也执行动作，但要自动停下
    doAction(action, speed, duration);

    if (expression.length() > 0) {
      g_face.setEmotion(parseEmotion(expression));
    }
    // speech 由 PC 通过 robot/speak 发音频，这里只记日志
    if (speech.length() > 0) {
      Serial.printf("[CMD] speech: %s\n", speech.c_str());
    }
    return;
  }

  // ---- 要播放的音频（分块累积，收完再播）----
  if (t == MQTT_TOPIC_SPEAK) {
    if (!g_audio.speakerReady()) {
      Serial.println("[SPEAK] 功放未就绪，跳过");
      return;
    }
    spkAppend(payload, len);
    Serial.printf("[SPEAK] 收到 %u 字节，累计 %u\n",
                  (unsigned)len, (unsigned)g_spkLen);
    Serial.flush();
    return;
  }
}

// ===========================================================================
// 上报状态
// ===========================================================================
static void publishState() {
  char json[320];
  const char *emotionNames[] = {
    "idle", "happy", "sad", "surprised", "thinking", "angry", "sleepy"
  };
  int ei = (int)g_face.emotion();
  if (ei < 0 || ei >= EMO_COUNT) ei = 0;

  snprintf(json, sizeof(json),
           "{\"dist\":%d,\"pir\":%d,\"light\":%d,\"emotion\":\"%s\","
           "\"mode\":\"%s\",\"rssi\":%d,\"heap\":%u,\"action\":\"%s\","
           "\"chat\":%d,\"vad\":\"%s\"}",
           g_dist, g_pir, g_light, emotionNames[ei],
           g_mode == MODE_AUTO ? "auto" : "manual",
           WiFi.RSSI(), ESP.getFreeHeap(), g_currentAction.c_str(),
           g_chat.enabled() ? 1 : 0, g_chat.stateName());

  g_mqtt.publish(MQTT_TOPIC_STATE, json);
}

// ===========================================================================
// 录音并上传（VAD 对话模式用的回调）
// ===========================================================================
static bool publishPcm(const std::vector<int16_t> &pcm) {
  if (pcm.empty() || !g_mqtt.connected()) return false;
  size_t bytes = pcm.size() * sizeof(int16_t);

  // 分块发送。原因：
  //   1. PubSubClient 的 buffer 是 uint16_t，最大 65535，一块不能太大
  //   2. 块太小 + delay 太多会导致期间 DMA 被覆盖（噪音）
  const size_t CHUNK = 30000;
  bool ok = true;
  if (g_mqtt.setBufferSize(CHUNK + 512)) {
    size_t offset = 0;
    while (offset < bytes) {
      size_t n = bytes - offset;
      if (n > CHUNK) n = CHUNK;
      if (!g_mqtt.publish(MQTT_TOPIC_AUDIO,
                          (const uint8_t *)pcm.data() + offset, n)) {
        ok = false;
        break;
      }
      offset += n;
      delay(1);
    }
    g_mqtt.setBufferSize(2048);
  } else {
    ok = false;
  }
  return ok;
}

// VAD 触发时调用
static void onSpeechCaptured(const std::vector<int16_t> &pcm) {
  Serial.printf("[对话] 上传 %u 采样 (%.1f 秒)\n",
                (unsigned)pcm.size(), pcm.size() / 16000.0f);
  Serial.flush();

  g_face.setEmotion(EMO_THINKING);
  bool ok = publishPcm(pcm);
  Serial.println(ok ? "[对话] 已上传，等回复" : "[对话] 上传失败");
  Serial.flush();
  // 等 PC 回语音（在 loop 里检测播放完成后 resumeListening）
}

// ===========================================================================
// 录音并上传（REC 命令，固定 4 秒）
// ===========================================================================
static void recordAndUpload() {
  if (!g_audio.micReady()) {
    Serial.println("[REC] 麦克风不可用");
    return;
  }

  Serial.println("[REC] 开始录音...");
  Serial.flush();
  g_face.setEmotion(EMO_THINKING);

  std::vector<int16_t> pcm = g_audio.record();
  if (pcm.empty()) {
    Serial.println("[REC] 没录到数据");
    g_face.setEmotion(EMO_IDLE);
    return;
  }

  size_t bytes = pcm.size() * sizeof(int16_t);
  Serial.printf("[REC] 录到 %u 字节 (%u 采样点)\n",
                (unsigned)bytes, (unsigned)pcm.size());
  Serial.flush();

  bool ok = publishPcm(pcm);
  Serial.println(ok ? "[REC] 上传完成" : "[REC] 上传失败");
  Serial.flush();

  g_face.setEmotion(EMO_IDLE);
}

// ===========================================================================
// 自动避障
// ===========================================================================
static void autoAvoid() {
  uint32_t now = millis();

  // 距离够远就巡航
  if (g_dist > OBSTACLE_AVOID_CM || g_dist == 999) {
    if (g_currentAction != "move_forward") {
      g_motor.forwardKick(MOTOR_SPEED_CRUISE);
      g_currentAction = "move_forward";
    }
    return;
  }

  // 有障碍，但要避开避障冷却期
  if (now - g_lastAvoid < OBSTACLE_COOLDOWN_MS) return;
  g_lastAvoid = now;

  Serial.printf("[避障] 前方 %dcm，转向\n", g_dist);
  g_face.setEmotion(EMO_SURPRISED);

  // 先退一点再转
  g_motor.backwardKick(MOTOR_SPEED_SLOW);
  delay(300);
  g_motor.turnKick(MOTOR_SPEED_TURN, (random(0, 2) ? 1 : -1));
  delay(TURN_DURATION_MS);
  g_motor.forwardKick(MOTOR_SPEED_CRUISE);
}

// ===========================================================================
// ===========================================================================
// 串口命令处理（提取成独立函数，让 MQTT 和网页也能触发同样的命令）
// ===========================================================================
static void handleSerialLine(const String &line) {
  if (line.startsWith("WIFI ")) {
    int sp = line.indexOf(' ', 5);
    if (sp > 0) {
      String ssid = line.substring(5, sp);
      String pass = line.substring(sp + 1);
      saveStoredWiFi(ssid.c_str(), pass.c_str());
      Serial.printf("[WiFi] 已保存: %s，重启生效\n", ssid.c_str());
    }
    return;
  }
  if (line == "RESET_WIFI") {
    clearStoredWiFi();
    Serial.println("[WiFi] 已清除，重启后再配");
    return;
  }
  if (line == "STATUS") {
    Serial.printf("[状态] dist=%d pir=%d light=%d heap=%u\n",
                  g_dist, g_pir, g_light, ESP.getFreeHeap());
    return;
  }
  if (line == "REC") { recordAndUpload(); return; }
  if (line == "CHAT") { g_chat.toggle(); return; }
  if (line == "RMS") {
    Serial.printf("[调试] 阈值 %.0f，底噪 %.0f\n",
                  g_chat.threshold(), g_chat.noiseFloor());
    return;
  }
  if (line == "AUTO") {
    g_mode = MODE_AUTO;
    g_actionUntil = 0;
    g_currentAction = "idle";
    g_motor.stop();
    Serial.println("[模式] 自动（电机先停，等避障接管）");
    return;
  }
  if (line == "MANUAL") {
    g_mode = MODE_MANUAL;
    g_actionUntil = 0;
    g_currentAction = "idle";
    g_motor.stop();
    Serial.println("[模式] 手动（电机已停）");
    return;
  }
  if (line == "STOP" || line == "ALL0") {
    g_actionUntil = 0;
    g_currentAction = "idle";
    g_motor.stop();
    Serial.println("[电机] 已停");
    return;
  }

  // ---- 单轮调试：L+80 / L-80 / R+80 / R-80 / L0 / R0 ----
  if (line.length() >= 2 &&
      (line[0] == 'L' || line[0] == 'R') &&
      (line[1] == '+' || line[1] == '-' || line[1] == '0')) {
    bool isLeft = (line[0] == 'L');
    char op = line[1];
    if (op == '0') {
      if (isLeft) Motor::setWheel(PIN_MOTOR_L1, PIN_MOTOR_L2, 0, 0);
      else        Motor::setWheel(PIN_MOTOR_R1, PIN_MOTOR_R2, 0, 0);
      Serial.printf("[电机] %s轮 停\n", isLeft ? "左" : "右");
      return;
    }
    int dir = (op == '+') ? 1 : -1;
    int pwm = (line.length() > 2) ? line.substring(2).toInt() : 40;
    if (pwm <= 0) pwm = 40;
    if (pwm > 100) pwm = 100;
    if (isLeft) Motor::setWheel(PIN_MOTOR_L1, PIN_MOTOR_L2, dir, pwm,
                                MOTOR_L_INVERT);
    else        Motor::setWheel(PIN_MOTOR_R1, PIN_MOTOR_R2, dir, pwm,
                                MOTOR_R_INVERT);
    Serial.printf("[电机] %s轮 %s %d%%\n", isLeft ? "左" : "右",
                  dir > 0 ? "正转" : "反转", pwm);
    return;
  }

  // ---- 四方向校准 ----
  if (line == "FWD") {
    Serial.printf("[校准] 前进（L_INV=%d R_INV=%d SWAP=%d）\n",
                  MOTOR_L_INVERT, MOTOR_R_INVERT, MOTOR_SWAP_LR);
    g_motor.forwardKick(MOTOR_SPEED_CRUISE);
    delay(2000);
    g_motor.stop();
    Serial.println("[校准] 停");
    return;
  }
  if (line == "BACK") {
    Serial.printf("[校准] 后退（L_INV=%d R_INV=%d BOOST=%d）\n",
                  MOTOR_L_INVERT, MOTOR_R_INVERT, MOTOR_BACK_BOOST);
    g_motor.backwardKick(MOTOR_SPEED_CRUISE);
    delay(2000);
    g_motor.stop();
    Serial.println("[校准] 停");
    return;
  }
  if (line == "LEFT") {
    Serial.println("[校准] 左转");
    g_motor.turnKick(MOTOR_SPEED_TURN, 1);
    delay(1000);
    g_motor.stop();
    Serial.println("[校准] 停");
    return;
  }
  if (line == "RIGHT") {
    Serial.println("[校准] 右转");
    g_motor.turnKick(MOTOR_SPEED_TURN, -1);
    delay(1000);
    g_motor.stop();
    Serial.println("[校准] 停");
    return;
  }

  // ---- 原始 GPIO 诊断（绕过所有方向逻辑）----
  // RAW+  两轮物理正转（左 IN1 高 / 右 IN3 高）
  // RAW-  两轮物理反转（左 IN2 高 / 右 IN4 高）
  // RAWL+ R+ 只左轮
  if (line == "RAW+") {
    Serial.println("[诊断] 两轮物理正转");
    rawDrive(90, 90);
    delay(2000);
    g_motor.stop();
    Serial.println("[诊断] 停");
    return;
  }
  if (line == "RAW-") {
    Serial.println("[诊断] 两轮物理反转");
    rawDrive(-90, -90);
    delay(2000);
    g_motor.stop();
    Serial.println("[诊断] 停");
    return;
  }

  if (line.length() > 0) {
    Serial.println("[串口] 未知命令。可用：");
    Serial.println("  WIFI <ssid> <pass> / RESET_WIFI / STATUS");
    Serial.println("  REC / CHAT / RMS / AUTO / MANUAL / STOP");
    Serial.println("  FWD / BACK / LEFT / RIGHT          （四方向校准）");
    Serial.println("  L+80 / L-80 / R+80 / R-80 / L0 / R0（单轮）");
    Serial.println("  RAW+ / RAW-                        （原始 GPIO）");
  }
}

// ===========================================================================
// 串口读取（读一行，交给 handleSerialLine）
// ===========================================================================
static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (g_serialLen > 0) {
        g_serialBuf[g_serialLen] = 0;
        String line = String(g_serialBuf);
        line.trim();
        g_serialLen = 0;
        handleSerialLine(line);
      }
    } else if (g_serialLen < sizeof(g_serialBuf) - 1) {
      g_serialBuf[g_serialLen++] = c;
    }
  }
}


// ===========================================================================
// setup
// ===========================================================================
void setup() {
  // 先开串口并等它稳定（UART 口上电有几百毫秒不稳定期，
  // 这时候打印的内容会丢，所以必须先等再打印）
  Serial.begin(115200);
  delay(1500);
  for (int i = 0; i < 3; i++) {
    Serial.println();
    Serial.flush();
    delay(100);
  }

  Serial.println("=== BOOT: setup() 开始 ===");
  Serial.flush();

  // PSRAM 检查（N16R8 应该有 8MB）
  Serial.printf("[0/5] PSRAM: 总 %u 字节, 空闲 %u 字节 %s\n",
                ESP.getPsramSize(), ESP.getFreePsram(),
                ESP.getPsramSize() > 0 ? "OK" : "未启用!");
  Serial.printf("       内部堆: %u 字节\n", ESP.getFreeHeap());
  Serial.flush();

  Serial.println("[1/5] 电机...");
  Serial.flush();
  g_motor.begin();

  Serial.println("[2/5] 超声波...");
  Serial.flush();
  g_ultra.begin();

  Serial.println("[3/5] OLED...");
  Serial.flush();
  g_face.begin();
  Serial.println("[3/5] OLED 完成");
  Serial.flush();

  Serial.println("[4/5] 音频...");
  Serial.flush();
  g_audio.begin();
  g_chat.begin(&g_audio);
  g_chat.setUploadFn(onSpeechCaptured);
  Serial.println("[4/5] 音频完成");
  Serial.flush();

  Serial.println("[5/5] WiFi...");
  Serial.flush();
  bool wifi = setupWiFi();
  if (!wifi) {
    Serial.println("[WiFi] 没连上，进入离线模式（本地避障仍可用）");
  } else {
    g_mqtt.setServer(MQTT_HOST, MQTT_PORT);
    g_mqtt.setCallback(onMqttMessage);
    g_mqtt.setKeepAlive(15);
    // 接收 buffer 必须够大：PC 按 30KB 一块发音频，
    // 默认 2KB 会把后面的字节截掉，表现为「只收到 826 字节」。
    // PubSubClient 的 buffer 是 uint16_t，最大 65535。
    g_mqtt.setBufferSize(32768);
    g_mqtt.setSocketTimeout(10);
    Serial.println("[MQTT] 接收 buffer 设为 32768 字节");
  }

  g_face.setEmotion(EMO_IDLE);
  Serial.printf("[系统] 就绪，内部堆 %u 字节\n", ESP.getFreeHeap());
  Serial.println("=== BOOT: setup() 结束 ===");
  Serial.flush();
}

// ===========================================================================
// loop
// ===========================================================================
void loop() {
  handleSerial();

  // ---- WiFi / MQTT 维护 ----
  if (WiFi.status() == WL_CONNECTED) {
    if (!g_mqtt.connected() && millis() > g_wifiRetryAt) {
      g_wifiRetryAt = millis() + 3000;
      Serial.print("[MQTT] 连接中...");
      if (g_mqtt.connect("robot_s3", MQTT_USERNAME, MQTT_PASSWORD)) {
        Serial.println(" 成功");
        g_mqtt.subscribe(MQTT_TOPIC_CMD);
        g_mqtt.subscribe(MQTT_TOPIC_SPEAK);
      } else {
        Serial.printf(" 失败 rc=%d\n", g_mqtt.state());
      }
    }
    g_mqtt.loop();
  } else {
    // WiFi 断了，重连
    if (millis() > g_wifiRetryAt) {
      g_wifiRetryAt = millis() + 5000;
      Serial.println("[WiFi] 重连...");
      WiFi.reconnect();
    }
  }

  // ---- 传感器采样 ----
  static uint32_t lastSample = 0;
  if (millis() - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = millis();
    g_dist  = g_ultra.measure();
    g_pir   = digitalRead(PIN_PIR);
    g_light = digitalRead(PIN_LIGHT);
  }

  // ---- 运动逻辑 ----
  if (g_mode == MODE_AUTO) {
    // 动作到期就交回自动避障
    if (g_actionUntil && millis() > g_actionUntil) {
      g_actionUntil = 0;
      g_currentAction = "idle";
      g_motor.stop();
    }
    // 避障只在没有明确指令时跑
    if (!g_actionUntil) {
      autoAvoid();
    }
  } else {
    // 手动模式：动作到期就停
    if (g_actionUntil && millis() > g_actionUntil) {
      g_actionUntil = 0;
      g_currentAction = "idle";
      g_motor.stop();
    }
  }

  // ---- 播放：攒够后（600ms 没新块）一次性播 ----
  if (g_spkLen > 0 && millis() - g_spkLast > 600) {
    Serial.printf("[SPEAK] 开始播放 %u 字节\n", (unsigned)g_spkLen);
    Serial.flush();
    g_audio.play(g_spkBuf, g_spkLen);
    Serial.println("[SPEAK] 播放完成");
    Serial.flush();
    spkReset();

    // 播放完回到监听（对话模式）
    if (g_chat.enabled()) {
      g_chat.resumeListening();
      g_face.setEmotion(EMO_IDLE);
    }
  }

  // ---- 自动对话模式（VAD）----
  g_chat.tick();

  // ---- 表情动画 ----
  g_face.tick();

  // ---- 状态上报 ----
  if (millis() - g_lastState > 1000) {
    g_lastState = millis();
    if (g_mqtt.connected()) publishState();

    // 串口心跳：每 5 秒打一行，确认板子活着
    static uint32_t lastBeat = 0;
    if (millis() - lastBeat > 5000) {
      lastBeat = millis();
      Serial.printf("[心跳] dist=%dcm 表情=%d 堆=%u 动作=%s\n",
                    g_dist, (int)g_face.emotion(), ESP.getFreeHeap(),
                    g_currentAction.c_str());
      Serial.flush();
    }
  }
}
