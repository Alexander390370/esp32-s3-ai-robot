// ===========================================================================
// OLED 表情系统 - SSD1306 128x64
// ===========================================================================
// 表情用「眼睛」画：两个椭圆，参数化控制大小/位置/形状。
// 6 种情绪：
//   idle     待机    普通圆眼，偶尔眨
//   happy    开心    弯月眼（向上弧）
//   sad      难过    下垂眼 + 嘴
//   surprised 惊讶   大圆眼
//   thinking 思考    半闭眼 + 偏移
//   angry    生气    斜眼 + 皱眉
//   sleepy   困倦    闭眼一条线
// ===========================================================================
#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include "config.h"

enum Emotion {
  EMO_IDLE,
  EMO_HAPPY,
  EMO_SAD,
  EMO_SURPRISED,
  EMO_THINKING,
  EMO_ANGRY,
  EMO_SLEEPY,
  EMO_COUNT
};

class Face {
 public:
  Adafruit_SSD1306 dsp;

  void begin() {
    dsp.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    dsp.clearDisplay();
    dsp.setTextColor(SSD1306_WHITE);
    dsp.display();
    setEmotion(EMO_IDLE);
  }

  void setEmotion(Emotion e) {
    if (e != m_emotion) {
      m_emotion = e;
      m_emotionUntil = millis() + 2500;   // 表情保持 2.5 秒后回待机
    }
  }

  Emotion emotion() const { return m_emotion; }

  // 每帧调用，负责动画
  void tick() {
    // 表情超时回待机
    if (m_emotion != EMO_IDLE && millis() > m_emotionUntil) {
      m_emotion = EMO_IDLE;
    }

    // 眨眼：待机时随机眨一下
    if (m_emotion == EMO_IDLE && m_nextBlink == 0) {
      m_nextBlink = millis() + 2500 + random(0, 3000);
    }
    bool blinking = (m_nextBlink && millis() < m_nextBlink + 150);

    drawFace(m_emotion, blinking);
    m_nextBlink = 0;
  }

 private:
  Emotion m_emotion = EMO_IDLE;
  uint32_t m_emotionUntil = 0;
  uint32_t m_nextBlink = 0;

  void eye(int cx, int cy, int rx, int ry, int style) {
    switch (style) {
      case 0:   // 圆
        dsp.fillEllipse(cx, cy, rx, ry, SSD1306_WHITE);
        break;
      case 1:   // 弯月（向上弧，开心）
        for (int x = -rx; x <= rx; x++) {
          int y = (int)(sqrt((float)(rx * rx - x * x)) * 0.6);
          dsp.drawPixel(cx + x, cy - y, SSD1306_WHITE);
          dsp.drawPixel(cx + x, cy - y + 1, SSD1306_WHITE);
        }
        break;
      case 2:   // 下垂（难过）
        for (int x = -rx; x <= rx; x++) {
          int y = (int)(sqrt((float)(rx * rx - x * x)) * 0.6);
          dsp.drawPixel(cx + x, cy + y, SSD1306_WHITE);
          dsp.drawPixel(cx + x, cy + y - 1, SSD1306_WHITE);
        }
        break;
      case 3:   // 直线（闭眼/困倦）
        dsp.fillRect(cx - rx, cy, rx * 2, 2, SSD1306_WHITE);
        break;
      case 4:   // 半闭
        dsp.fillEllipse(cx, cy + 1, rx, ry / 2, SSD1306_WHITE);
        break;
    }
  }

  void mouth(int cy, int style) {
    switch (style) {
      case 0:   // 微笑
        for (int x = -12; x <= 12; x++) {
          int y = (int)(sqrt((float)(144 - x * x)) * 0.25);
          dsp.drawPixel(64 + x, cy - y, SSD1306_WHITE);
        }
        break;
      case 1:   // 皱眉
        for (int x = -12; x <= 12; x++) {
          int y = (int)(sqrt((float)(144 - x * x)) * 0.25);
          dsp.drawPixel(64 + x, cy + y, SSD1306_WHITE);
        }
        break;
      case 2:   // 张嘴（惊讶）
        dsp.fillEllipse(64, cy, 5, 7, SSD1306_WHITE);
        break;
      case 3:   // 小圆（思考）
        dsp.fillEllipse(64, cy, 3, 3, SSD1306_WHITE);
        break;
    }
  }

  void drawFace(Emotion e, bool blinking) {
    dsp.clearDisplay();

    int lx = 42, rx = 86, ey = 26;
    int erx = 8, ery = 10;
    int eyeStyle = 0, mouthStyle = 0, mouthY = 48;

    switch (e) {
      case EMO_IDLE:
        if (blinking) { eyeStyle = 3; }
        else { eyeStyle = 0; }
        mouthStyle = 3;
        break;

      case EMO_HAPPY:
        eyeStyle = 1;
        mouthStyle = 0;
        break;

      case EMO_SAD:
        eyeStyle = 2;
        mouthStyle = 1;
        break;

      case EMO_SURPRISED:
        erx = 11; ery = 13;
        eyeStyle = 0;
        mouthStyle = 2;
        break;

      case EMO_THINKING:
        eyeStyle = 4;
        mouthStyle = 3;
        // 眼睛往上看
        ey = 22;
        break;

      case EMO_ANGRY:
        eyeStyle = 0;
        mouthStyle = 1;
        // 眉毛斜线
        for (int i = 0; i < 10; i++) {
          dsp.drawPixel(lx - 8 + i, ey - 12 + i / 2, SSD1306_WHITE);
          dsp.drawPixel(rx + 8 - i, ey - 12 + i / 2, SSD1306_WHITE);
        }
        break;

      case EMO_SLEEPY:
        eyeStyle = 3;
        mouthStyle = 3;
        break;

      default:
        break;
    }

    eye(lx, ey, erx, ery, eyeStyle);
    eye(rx, ey, erx, ery, eyeStyle);
    mouth(mouthY, mouthStyle);

    dsp.display();
  }
};
