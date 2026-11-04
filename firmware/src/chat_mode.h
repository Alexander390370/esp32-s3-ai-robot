// ===========================================================================
// 自动对话模式（VAD 语音活动检测）
// ===========================================================================
// 目标：不用手动发 REC，直接说话就能对话。
//
// 工作原理：
//   1. 一直读麦克风小块（0.1 秒）
//   2. 算 RMS 能量
//   3. 能量超过阈值 → 认为你开始说话了 → 开始攒数据
//   4. 能量低于阈值持续 800ms → 认为你说完了 → 上传给 PC
//   5. PC 识别 + AI 回复 + 合成语音 → 回来播放
//   6. 播放完继续监听
//
// 关键参数（都要能调，因为不同麦克风增益差别很大）：
//   VAD_THRESHOLD   能量阈值，低于这个算静音
//   VAD_SILENCE_MS  静音多久算说完
//   VAD_MIN_SPEECH  最短说话时长，太短忽略（防咳嗽/关门声）
//   VAD_MAX_RECORD  最长录音，防一直说个没完
// ===========================================================================
#pragma once

#include <Arduino.h>
#include <vector>

#include "audio_io.h"
#include "config.h"

class ChatMode {
 public:
  // 状态
  enum State {
    IDLE,        // 没开对话模式
    LISTENING,   // 监听中，等有人说话
    RECORDING,   // 正在录
    UPLOADING,   // 上传中（等 PC 回复）
  };

  // 回调：录完一段要上传时调用
  typedef void (*UploadFn)(const std::vector<int16_t> &pcm);

  void begin(AudioIO *audio) {
    m_audio = audio;
    m_enabled = false;
    m_state = IDLE;
  }

  void setUploadFn(UploadFn fn) { m_upload = fn; }

  bool enabled() const { return m_enabled; }
  State state() const { return m_state; }

  const char *stateName() const {
    switch (m_state) {
      case IDLE:      return "idle";
      case LISTENING: return "listen";
      case RECORDING: return "record";
      case UPLOADING: return "upload";
    }
    return "?";
  }

  void enable() {
    if (!m_audio || !m_audio->micReady()) {
      Serial.println("[对话] 麦克风不可用，无法开启");
      return;
    }
    m_enabled = true;
    m_state = LISTENING;
    m_buf.clear();
    m_speechMs = 0;
    m_silenceMs = 0;
    m_noiseFloor = 0;
    m_calibrated = false;
    m_calibFrames = 0;
    Serial.println("[对话] 已开启");
    Serial.println("[对话] 正在校准环境噪声（保持安静 1 秒）...");
  }

  void disable() {
    m_enabled = false;
    m_state = IDLE;
    m_buf.clear();
    Serial.println("[对话] 已关闭");
  }

  void toggle() {
    if (m_enabled) disable();
    else enable();
  }

  // 每 loop 调一次。非阻塞：每次只读一小块
  void tick() {
    if (!m_enabled || !m_audio) return;
    if (m_state == UPLOADING) return;    // 等 PC 回复，别抢麦克风

    // 读 0.1 秒数据（16kHz × 0.1 = 1600 采样）
    static int32_t raw[CHUNK_SAMPLES];
    size_t got = 0;
    int16_t pcm[CHUNK_SAMPLES];

    size_t n = m_audio->readRaw(raw, CHUNK_SAMPLES, &got, 30);
    if (n == 0) return;

    // 转成 int16 并算 RMS
    int64_t sumSq = 0;
    for (size_t i = 0; i < n; i++) {
      int32_t v = raw[i] >> 14;
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      pcm[i] = (int16_t)v;
      sumSq += (int64_t)pcm[i] * pcm[i];
    }
    float rms = sqrtf((float)sumSq / n);

    bool voiced = (rms > effectiveThreshold());
    m_lastRms = rms;

    // ---- 环境噪声校准 ----
    // 开对话后先测 10 帧（1 秒）的环境噪声，取平均值作为底噪。
    // 然后阈值 = 底噪 × 3，最少不低于 MIN_THRESHOLD。
    // 这样不管麦克风增益多大，阈值都能自适应。
    if (!m_calibrated) {
      if (m_calibFrames == 0) m_noiseFloor = rms;
      else m_noiseFloor = (m_noiseFloor * 0.7f + rms * 0.3f);
      m_calibFrames++;
      if (m_calibFrames >= 10) {
        m_calibrated = true;
        Serial.printf("[对话] 校准完成：环境噪声 %.0f，触发阈值 %.0f\n",
                      m_noiseFloor, effectiveThreshold());
        Serial.println("[对话] 现在可以说话了");
      }
      return;
    }

    if (m_state == LISTENING) {
      // 底噪慢慢跟踪（只在不说话时更新，避免被说话拉高）
      if (!voiced) {
        m_noiseFloor = m_noiseFloor * 0.98f + rms * 0.02f;
      }

      if (voiced) {
        // 开始说话
        m_state = RECORDING;
        m_buf.clear();
        m_buf.insert(m_buf.end(), pcm, pcm + n);
        m_speechMs = 100;
        m_silenceMs = 0;
        Serial.printf("[对话] 检测到说话 (rms=%.0f, 阈值=%.0f)\n",
                      rms, effectiveThreshold());
      }
      return;
    }

    if (m_state == RECORDING) {
      // 把这 0.1 秒加进缓冲
      m_buf.insert(m_buf.end(), pcm, pcm + n);

      if (voiced) {
        m_speechMs += 100;
        m_silenceMs = 0;
      } else {
        m_silenceMs += 100;
      }

      bool tooLong   = (m_speechMs + m_silenceMs) >= VAD_MAX_RECORD_MS;
      bool finished  = (m_silenceMs >= VAD_SILENCE_MS) &&
                       (m_speechMs >= VAD_MIN_SPEECH_MS);

      if (tooLong || finished) {
        // 太短的丢掉（咳嗽、关门声）
        if (m_speechMs < VAD_MIN_SPEECH_MS) {
          Serial.printf("[对话] 太短忽略 (说话 %dms)\n", m_speechMs);
          m_state = LISTENING;
          m_buf.clear();
          return;
        }

        Serial.printf("[对话] 说完 (说话 %dms, 静音 %dms, 共 %u 采样)\n",
                      m_speechMs, m_silenceMs, (unsigned)m_buf.size());
        m_state = UPLOADING;
        if (m_upload) {
          m_upload(m_buf);
        } else {
          m_state = LISTENING;
        }
        m_buf.clear();
      }
    }
  }

  // PC 回复播放完后调用，回到监听
  void resumeListening() {
    if (!m_enabled) return;
    m_state = LISTENING;
    m_buf.clear();
    m_speechMs = 0;
    m_silenceMs = 0;
    Serial.println("[对话] 继续听...");
  }

  // 动态调阈值（噪音大时用）
  void setThreshold(float t) {
    m_manualThreshold = t;
    m_useManual = true;
    Serial.printf("[对话] 手动阈值 %.0f\n", t);
  }

  // 实际生效的阈值
  float effectiveThreshold() const {
    if (m_useManual) return m_manualThreshold;
    // 底噪 × 倍数，但不低于绝对下限
    float t = (m_noiseFloor > 0) ? (m_noiseFloor * VAD_NOISE_MULT) : VAD_THRESHOLD;
    if (t < VAD_MIN_THRESHOLD) t = VAD_MIN_THRESHOLD;
    if (t > 20000) t = 20000;
    return t;
  }

  float threshold() const { return effectiveThreshold(); }
  float noiseFloor() const { return m_noiseFloor; }

  // 当前能量（调试用）
  float lastRms() const { return m_lastRms; }

 private:
  static const size_t CHUNK_SAMPLES = 1600;   // 0.1 秒 @ 16kHz

  AudioIO *m_audio = nullptr;
  UploadFn m_upload = nullptr;
  bool m_enabled = false;
  State m_state = IDLE;

  std::vector<int16_t> m_buf;
  int m_speechMs = 0;
  int m_silenceMs = 0;
  float m_lastRms = 0;

  float m_noiseFloor = 0;       // 环境底噪
  bool  m_calibrated = false;   // 校准完了吗
  int   m_calibFrames = 0;
  float m_manualThreshold = 0;
  bool  m_useManual = false;
};
