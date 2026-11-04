// ===========================================================================
// 超声波测距 - HC-SR04
// ===========================================================================
// 两个坑：
//
// 1. 串扰：触发太快，上一发回波还没消散，Echo 一直高，pulseIn 一直等到
//    超时，读数卡死。数据手册要求两次测量间隔 ≥ 60ms。
//    这里测 3 次取中位数（不是平均值——一个 999 就能把平均值毁掉）。
//
// 2. 回波抖动：真实回波会因为气流温度浮动 ±20us，单次读数不可信。
// ===========================================================================
#pragma once

#include <Arduino.h>
#include "config.h"

class Ultrasonic {
 public:
  void begin() {
    pinMode(PIN_ULTRASONIC_TRIG, OUTPUT);
    pinMode(PIN_ULTRASONIC_ECHO, INPUT);
    digitalWrite(PIN_ULTRASONIC_TRIG, LOW);
  }

  // 单次测量，返回距离 cm；没回波返回 999
  int measureOnce() {
    // Trig 拉低足够久，让上一发回波消散
    digitalWrite(PIN_ULTRASONIC_TRIG, LOW);
    delayMicroseconds(5);

    // 10us 脉冲触发
    digitalWrite(PIN_ULTRASONIC_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_ULTRASONIC_TRIG, LOW);

    // 等回波，最长 25ms ≈ 4.25m
    long dur = pulseIn(PIN_ULTRASONIC_ECHO, HIGH, 25000UL);
    if (dur == 0) return 999;

    int cm = (int)(dur * 0.034f / 2.0f);
    if (cm <= 2 || cm > 400) return 999;   // 太近或不合理
    return cm;
  }

  // 三次取中位数。返回 999 表示没测到
  int measure() {
    int a = measureOnce();
    delay(60);
    int b = measureOnce();
    delay(60);
    int c = measureOnce();

    int mid;
    if ((a >= b && a <= c) || (a <= b && a >= c))      mid = a;
    else if ((b >= a && b <= c) || (b <= a && b >= c)) mid = b;
    else                                                mid = c;

    // 三次全超时 = 真的没目标
    if (a == 999 && b == 999 && c == 999) return 999;

    // 有有效值但中位数是 999，用有效的那个
    if (mid == 999) {
      if (a != 999) mid = a;
      else if (b != 999) mid = b;
      else mid = c;
    }
    return mid;
  }
};
