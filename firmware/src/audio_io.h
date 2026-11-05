// ===========================================================================
// I2S 音频 - INMP441 麦克风 + MAX98357 功放
// ===========================================================================
// 踩过的坑（重要）：
//
// Arduino 的 I2SClass 不适合「两个设备同时用」：
//   它构造时就把实例绑到某个外设号上，多个实例共享同一份全局状态，
//   第二个 begin() 会去 uninstall 第一个刚装好的驱动，
//   在 i2s_ll_tx_stop 里空指针崩溃（LoadProhibited）。
//
// 所以这里直接用底层 driver API：
//   i2s_driver_install(port, &cfg, 0, NULL)
// 每个外设独立配置，互不干扰。
//
// 硬件对应：
//   I2S_NUM_0 → INMP441  (RX)
//   I2S_NUM_1 → MAX98357 (TX)
// ===========================================================================
#pragma once

#include <Arduino.h>
#include <driver/i2s.h>
#include <vector>

#include "config.h"

class AudioIO {
 public:
  bool begin() {
    m_micOk = initMic();
    m_spkOk = initSpeaker();
    return m_micOk;
  }

  bool micReady() const { return m_micOk; }
  bool speakerReady() const { return m_spkOk; }

  // ---------- 录音 ----------
  // 读 32 位，右移 14 位转成 16 位（INMP441 是 24 位数据在 32 位帧里）
  std::vector<int16_t> record() {
    std::vector<int16_t> buf;
    if (!m_micOk) return buf;

    const size_t totalSamples = (size_t)I2S_SAMPLE_RATE * AUDIO_RECORD_SECONDS;
    buf.resize(totalSamples);

    // 用 32 位缓冲接收
    std::vector<int32_t> raw(512);

    // 丢掉开机时的爆音
    size_t got = 0;
    i2s_read(I2S_NUM_0, raw.data(), raw.size() * 4, &got,
             100 / portTICK_PERIOD_MS);

    size_t readN = 0;
    uint32_t deadline = millis() + (AUDIO_RECORD_SECONDS * 1000) + 2000;
    while (readN < totalSamples && millis() < deadline) {
      size_t n = 0;
      size_t want = (totalSamples - readN);
      if (want > raw.size()) want = raw.size();

      esp_err_t err = i2s_read(I2S_NUM_0, raw.data(), want * 4, &n,
                               500 / portTICK_PERIOD_MS);
      if (err != ESP_OK || n == 0) {
        delay(2);
        continue;
      }
      size_t cnt = n / 4;
      for (size_t i = 0; i < cnt && readN < totalSamples; i++) {
        // INMP441 是 24 位器件，数据在 32 位帧里左对齐。
        // 右移 14 位：把 24 位压到约 10 位有效幅度。
        //   移太少（11）→ 直流偏置占满量程，顶到 32767 削波
        //   移太多（18）→ 有效信号太小，信噪比崩
        // 直流偏置由 PC 端去，这里只做缩放。
        int32_t v = raw[i] >> 14;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        buf[readN++] = (int16_t)v;
      }
    }

    buf.resize(readN);
    return buf;
  }

  // ---------- 播放 ----------
  //
  // 软件音量放大：MAX98357 的增益由 GAIN 引脚决定（悬空 9dB / 接 GND 15dB），
  // 硬件不方便改的情况下，可以在写 I2S 前把 PCM 数字放大。
  // 放大后可能削波，所以带限幅。
  void play(const uint8_t *data, size_t bytes) {
    if (!m_spkOk || bytes == 0) return;

    int vol = m_volume;
    size_t written = 0;
    uint32_t deadline = millis() + 15000;

    while (written < bytes && millis() < deadline) {
      size_t n = 0;

      if (vol <= 100) {
        // 不放大，直接写
        esp_err_t err = i2s_write(I2S_NUM_1, data + written,
                                  bytes - written, &n,
                                  500 / portTICK_PERIOD_MS);
        if (err != ESP_OK || n == 0) {
          delay(5);
          continue;
        }
      } else {
        // 放大：每次处理 512 字节，带限幅防削波
        size_t chunk = bytes - written;
        if (chunk > 512) chunk = 512;
        chunk &= ~1u;                 // 保证偶数（16 位采样）
        if (chunk == 0) break;

        int16_t tmp[256];
        size_t samples = chunk / 2;
        const int16_t *src = (const int16_t *)(data + written);
        for (size_t i = 0; i < samples; i++) {
          int32_t v = (int32_t)src[i] * vol / 100;
          if (v > 32767) v = 32767;
          if (v < -32768) v = -32768;
          tmp[i] = (int16_t)v;
        }
        esp_err_t err = i2s_write(I2S_NUM_1, tmp, samples * 2, &n,
                                  500 / portTICK_PERIOD_MS);
        if (err != ESP_OK || n == 0) {
          delay(5);
          continue;
        }
      }
      written += n;
    }
  }

  // 设置软件音量（100 = 原始，150 = 放大 1.5 倍）
  void setVolume(int v) {
    if (v < 100) v = 100;
    if (v > 200) v = 200;
    m_volume = v;
    Serial.printf("[Audio] 软件音量 %d%%\n", v);
  }

  int volume() const { return m_volume; }

  void stopPlay() {
    if (m_spkOk) i2s_zero_dma_buffer(I2S_NUM_1);
  }

  // ------------------------------------------------------------------
  // 低层读取（给 VAD 对话模式用）
  // 读最多 maxSamples 个 32 位原始采样，返回实际读到的采样数
  // 非阻塞：最多等 timeoutMs
  // ------------------------------------------------------------------
  size_t readRaw(int32_t *out, size_t maxSamples, size_t *got,
                 uint32_t timeoutMs) {
    if (!m_micOk || !out) {
      if (got) *got = 0;
      return 0;
    }
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, out, maxSamples * 4, &bytesRead,
                             timeoutMs / portTICK_PERIOD_MS);
    if (err != ESP_OK) {
      if (got) *got = 0;
      return 0;
    }
    size_t n = bytesRead / 4;
    if (got) *got = n;
    return n;
  }

 private:
  int m_volume = 100;    // 软件音量（100 = 不放大；>100 会削波成电流声，慎用）
  bool m_micOk = false;
  bool m_spkOk = false;

  bool initMic() {
    // 已经装过就先卸掉，避免重入
    i2s_driver_uninstall(I2S_NUM_0);

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = I2S_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    // L/R 接 GND → 左声道
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    // INMP441 要求标准 I2S（Philips）格式：
    //   WS 低 = 左声道，数据在 WS 变化后延迟 1 个 BCLK 才有效
    // 用 MSB 格式（右对齐）会因为位偏移采到噪声
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 16;
    cfg.dma_buf_len = 128;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = false;
    cfg.fixed_mclk = 0;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (err != ESP_OK) {
      Serial.printf("[Audio] 麦克风驱动安装失败: 0x%x\n", err);
      return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_I2S_MIC_SCK;
    pins.ws_io_num = PIN_I2S_MIC_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = PIN_I2S_MIC_SD;

    err = i2s_set_pin(I2S_NUM_0, &pins);
    if (err != ESP_OK) {
      Serial.printf("[Audio] 麦克风引脚设置失败: 0x%x\n", err);
      i2s_driver_uninstall(I2S_NUM_0);
      return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_0);
    Serial.printf("[Audio] 麦克风就绪 (32bit/16kHz/%d)\n", (int)I2S_SAMPLE_RATE);
    return true;
  }

  bool initSpeaker() {
    i2s_driver_uninstall(I2S_NUM_1);

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = I2S_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;   // 没数据时输出静音，不重复最后一段
    cfg.fixed_mclk = 0;

    esp_err_t err = i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
    if (err != ESP_OK) {
      Serial.printf("[Audio] 功放驱动安装失败: 0x%x\n", err);
      return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_I2S_SPK_BCLK;
    pins.ws_io_num = PIN_I2S_SPK_LRC;
    pins.data_out_num = PIN_I2S_SPK_DIN;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    err = i2s_set_pin(I2S_NUM_1, &pins);
    if (err != ESP_OK) {
      Serial.printf("[Audio] 功放引脚设置失败: 0x%x\n", err);
      i2s_driver_uninstall(I2S_NUM_1);
      return false;
    }

    i2s_zero_dma_buffer(I2S_NUM_1);
    Serial.println("[Audio] 功放就绪");
    return true;
  }
};
