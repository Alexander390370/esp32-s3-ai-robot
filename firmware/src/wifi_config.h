// ===========================================================================
// WiFi 配网 - 不写死任何 SSID/密码
// ===========================================================================
// 三种方式，任选：
//   1. 串口配网（推荐）：
//      上电后 5 秒内，在串口监视器输入：
//          WIFI <ssid> <password>
//      例如：WIFI MyWiFi mypassword
//      配好后会存进 NVS，下次上电自动连接。
//      换网络就在串口再输一次，不用重新编译。
//
//   2. 编译时指定（-D 上）：
//      pio run -t upload --upload-port COM8 -DWIFI_SSID=\"MyWiFi\" -DWIFI_PASS=\"mypassword\"
//
//   3. 配置文件（config.h 里）：
//      见下方 BUILD_TIME_WIFI
// ===========================================================================
#pragma once

#include <Preferences.h>
#include <WiFi.h>

// 若编译时带了 -DWIFI_SSID / -DWIFI_PASS，优先用它（一次性方案）
#ifdef WIFI_SSID
static const char *BUILD_SSID = WIFI_SSID;
static const char *BUILD_PASS = WIFI_PASS;
#define HAS_BUILD_WIFI 1
#else
static const char *BUILD_SSID = "";
static const char *BUILD_PASS = "";
#define HAS_BUILD_WIFI 0
#endif

static const char *PREF_NS = "robotwifi";
static const char *PREF_SSID = "ssid";
static const char *PREF_PASS = "pass";

static String g_ssid = "";
static String g_pass = "";

// 从 NVS 读已保存的 WiFi
static bool loadStoredWiFi() {
  Preferences prefs;
  if (!prefs.begin(PREF_NS, true)) {
    // 命名空间还不存在 —— 正常，说明还没配过网
    return false;
  }
  g_ssid = prefs.getString(PREF_SSID, "");
  g_pass = prefs.getString(PREF_PASS, "");
  prefs.end();
  return g_ssid.length() > 0;
}

// 保存到 NVS
static void saveStoredWiFi(const char *ssid, const char *pass) {
  Preferences prefs;
  // readOnly=false 会自动创建命名空间
  if (!prefs.begin(PREF_NS, false)) {
    Serial.println("[WiFi] NVS 打不开，配置无法保存");
    return;
  }
  prefs.putString(PREF_SSID, ssid);
  prefs.putString(PREF_PASS, pass);
  prefs.end();
}

// 清除保存的 WiFi（串口输入 RESET_WIFI）
static void clearStoredWiFi() {
  Preferences prefs;
  if (!prefs.begin(PREF_NS, false)) return;
  prefs.clear();
  prefs.end();
}

// ---------------------------------------------------------------------------
// 串口配网：开机 10 秒内可以输入 WIFI <ssid> <pass>
// ---------------------------------------------------------------------------
static void serialWiFiSetup(uint32_t windowMs) {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" WiFi 配网模式");
  Serial.println(" 在接下来的 10 秒内输入：");
  Serial.println("   WIFI <你的WiFi名> <密码>");
  Serial.println(" 例如：WIFI MyWiFi mypassword");
  Serial.println(" 已保存的 WiFi 也可以用 RESET_WIFI 清除");
  Serial.println("========================================");

  uint32_t start = millis();
  while (millis() - start < windowMs) {
    if (Serial.available()) {
      String line = Serial.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;

      Serial.print("收到: ");
      Serial.println(line);

      if (line.startsWith("RESET_WIFI")) {
        clearStoredWiFi();
        Serial.println("已清除保存的 WiFi，重启后再配");
        continue;
      }

      if (line.startsWith("WIFI ")) {
        int sp1 = line.indexOf(' ', 5);
        if (sp1 > 0) {
          String ssid = line.substring(5, sp1);
          String pass = line.substring(sp1 + 1);
          saveStoredWiFi(ssid.c_str(), pass.c_str());
          Serial.printf("已保存 WiFi: %s\n", ssid.c_str());
          Serial.println("请重启板子，现在会自动连接。");
          continue;
        }
      }
      Serial.println("格式不对，应该��� WIFI <ssid> <pass>");
    }
    delay(20);
  }
}

// ---------------------------------------------------------------------------
// 连接 WiFi，返回是否成功
// ---------------------------------------------------------------------------
static bool connectWiFi(uint32_t timeoutMs = 20000) {
  if (g_ssid.length() == 0) {
    Serial.println("[WiFi] 没有已保存的配置");
    return false;
  }

  Serial.printf("[WiFi] 连接 %s", g_ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);       // 关省电，降低 MQTT 延迟
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] 已连接 IP=%s RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }

  Serial.println("[WiFi] 连接失败");
  return false;
}

// ---------------------------------------------------------------------------
// 完整的 WiFi 启动流程
//   1. 有编译期配置就用编译期的
//   2. 否则读 NVS
//   3. 都没有就进串口配网
// ---------------------------------------------------------------------------
static bool setupWiFi() {
#if HAS_BUILD_WIFI
  Serial.println("[WiFi] 使用编译期配置");
  g_ssid = BUILD_SSID;
  g_pass = BUILD_PASS;
  if (connectWiFi()) return true;
  Serial.println("[WiFi] 编译期配置连不上，尝试已保存的配置");
#endif

  if (loadStoredWiFi()) {
    if (connectWiFi()) return true;
    Serial.println("[WiFi] 已保存的配置也连不上");
  }

  // 尝试串口配网
  Serial.println();
  Serial.println("[WiFi] 进入串口配网模式");
  serialWiFiSetup(10000);

  // 配网后重新尝试
  if (loadStoredWiFi()) {
    Serial.println("[WiFi] 尝试用新配置连接");
    if (connectWiFi()) return true;
  }

  return false;
}
