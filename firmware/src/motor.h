// ===========================================================================
// 电机驱动 - MX1508
// ===========================================================================
// MX1508 没有 ENA/ENB 使能脚，方向和调速都用两个 IN 引脚组合：
//
//   IN1  IN2   轮子
//   ───  ────
//    1    0     正转
//    0    1     反转
//    0    0     停止
//   PWM   0     正转调速
//
// 电流余量不大（标称 0.8A/路），所以堵转时要及时停。
//
// ⚠️ 关于「装反了」
//   实际组装时电机可能装反、线可能接反。为了不拆车重焊，
//   在 config.h 里用 MOTOR_L_INVERT / MOTOR_R_INVERT / MOTOR_SWAP_LR
//   做逻辑修正，所有方向问题都在那一处调，不用动这里的代码。
// ===========================================================================
#pragma once

#include <Arduino.h>
#include "config.h"

class Motor {
 public:
  void begin() {
    pinMode(PIN_MOTOR_L1, OUTPUT);
    pinMode(PIN_MOTOR_L2, OUTPUT);
    pinMode(PIN_MOTOR_R1, OUTPUT);
    pinMode(PIN_MOTOR_R2, OUTPUT);
    stop();
  }

  // 单轮控制。dir: +1 正转, -1 反转, 0 停。speed: 0-100
  // invert: 该轮是否方向取反（来自 config.h）
  static void setWheel(int in1, int in2, int dir, int speed,
                       int invert = 0) {
    if (dir == 0 || speed <= 0) {
      digitalWrite(in1, LOW);
      digitalWrite(in2, LOW);
      return;
    }
    if (invert) dir = -dir;      // 方向修正

    if (dir > 0) {
      // 正转：IN1 = PWM, IN2 = 0
      analogWrite(in1, speed * 255 / 100);
      digitalWrite(in2, LOW);
    } else {
      // 反转：IN1 = 0, IN2 = PWM
      digitalWrite(in1, LOW);
      analogWrite(in2, speed * 255 / 100);
    }
  }

  // 按 config.h 的修正配置驱动某一侧
  // side: 0 = 左, 1 = 右
  // boost: 额外增益百分比（100 = 不增益），上限 100% 占空比
  static void driveSide(int side, int dir, int speed, int boost = 100) {
#if MOTOR_SWAP_LR
    side = 1 - side;             // 左右互换
#endif
    int s = speed * boost / 100;
    if (s > 100) s = 100;
    if (side == 0) {
      setWheel(PIN_MOTOR_L1, PIN_MOTOR_L2, dir, s, MOTOR_L_INVERT);
    } else {
      setWheel(PIN_MOTOR_R1, PIN_MOTOR_R2, dir, s, MOTOR_R_INVERT);
    }
  }

  // ---------------- 基础动作 ----------------

  void forward(int speed) {
    driveSide(0, 1, speed);
    driveSide(1, 1, speed);
  }

  void backward(int speed) {
    // 后退就是两侧都反向。
    // 必须让每侧各自走自己的 invert 逻辑（driveSide 内部会处理），
    // 不能在这里统一改成 +1 —— 否则右轮会被 R_INVERT 再翻一次，
    // 变成一侧往前一侧往后 → 原地打转。
#if MOTOR_DEBUG
    int s = speed * MOTOR_BACK_BOOST / 100;
    if (s > 100) s = 100;
    Serial.printf("[后退] speed=%d boost=%d -> %d%% | L写%s R写%s\n",
                  speed, MOTOR_BACK_BOOST, s,
                  (MOTOR_L_INVERT ? "IN1" : "IN2"),
                  (MOTOR_R_INVERT ? "IN1" : "IN2"));
#endif
    driveSide(0, -1, speed, MOTOR_BACK_BOOST);
    driveSide(1, -1, speed, MOTOR_BACK_BOOST);
  }

  // 原地转圈。dir: +1 左转（逆时针），-1 右转
  void turn(int speed, int dir) {
    driveSide(0, dir, speed);
    driveSide(1, -dir, speed);
  }

  // 弧线转。左转时右轮快、左轮慢
  void turnArc(int speed, int dir) {
    int outer = speed;
    int inner = speed / 3;
    if (dir > 0) {
      driveSide(0, 1, inner);
      driveSide(1, 1, outer);
    } else {
      driveSide(0, 1, outer);
      driveSide(1, 1, inner);
    }
  }

  void stop() {
    // 用 analogWrite(0) 而不是 digitalWrite(LOW)。
    // 因为之前用 analogWrite 设过 PWM，硬切 LOW 有可能残留占空比寄存器，
    // 导致下一次换个方向时 MX1508 收到冲突电平（表现为不动或抖动）。
    analogWrite(PIN_MOTOR_L1, 0);
    analogWrite(PIN_MOTOR_L2, 0);
    analogWrite(PIN_MOTOR_R1, 0);
    analogWrite(PIN_MOTOR_R2, 0);
    digitalWrite(PIN_MOTOR_L1, LOW);
    digitalWrite(PIN_MOTOR_L2, LOW);
    digitalWrite(PIN_MOTOR_R1, LOW);
    digitalWrite(PIN_MOTOR_R2, LOW);
  }

  // ---------------- 诊断 ----------------
  // setWheel 计算出"应该"写什么之后打印出来，方便对比物理方向。
  // 注意不能用 digitalRead 读——analogWrite 是 PWM，读回来只有 0/1。
  static void dumpIntent(const char *tag, int side, int dir, int speed,
                         int invert, int pwmApplied) {
    // 算出最终会写哪个引脚
    int d = invert ? -dir : dir;
    const char *which = (d > 0) ? "IN1(HIGH)" : (d < 0 ? "IN2(HIGH)" : "两个都LOW");
    Serial.printf("[力矩] %s side=%s dir=%+d speed=%d%% inv=%d -> %s @%d/255\n",
                  tag, side == 0 ? "左" : "右", dir, speed, invert,
                  which, pwmApplied);
  }

  // ---------------- 带满力矩起步 ----------------
  //
  // TT 电机 1:48 减速箱静摩擦很大，直接用 60% 以下的占空比启动，
  // 力矩不够，表现为「吱吱响但轮子不动，手拨一下才转」。
  // 先给 100% 冲 kickMs 突破静摩擦，再降到目标速度。

  void forwardKick(int speed, int kickMs = 250) {
    if (speed <= 0) { stop(); return; }
    driveSide(0, 1, 100);
    driveSide(1, 1, 100);
    delay(kickMs);
    forward(speed);
  }

  void backwardKick(int speed, int kickMs = 250) {
    // ⚠️ 实测发现：后退不能加 kick。
    //
    // 原因：MOTOR_BACK_BOOST 已经让后退的实际占空比顶到 100%，
    // kick 再用 100% 冲一次，等于连续满功率启动两次。
    // MX1508 在这种情况会触发过流保护，表现为「吱吱响但轮子不动」，
    // 反而不如直接给 100% 一次到位。
    //
    // 所以后退直接走 backward()，不做 kick。
    (void)kickMs;
    backward(speed);
  }

  void turnKick(int speed, int dir, int kickMs = 250) {
    if (speed <= 0) { stop(); return; }
    driveSide(0, dir, 100);
    driveSide(1, -dir, 100);
    delay(kickMs);
    turn(speed, dir);
  }
};
