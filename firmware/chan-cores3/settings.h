#pragma once

// ============================================================
//  settings.h · 运行时配置持久化（NVS）
//
//  一次配网，永久生效；断电不丢。
//  命名空间 chanrec，命名空间内的 key 与字段一一对应。
// ============================================================

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

namespace settings {

struct Config {
  String   wifiSsid;
  String   wifiPass;
  String   gwHost;
  uint16_t gwPort;
  String   gwPath;
  String   gwToken;
  String   deviceName;
  bool     servoEnabled;   // 舵机总开关（默认关，录音优先）
  String   termsText;      // 配网网页保存的词表（一行一个）
};

static Config      gCfg;
static Preferences gPrefs;
static bool        gLoaded = false;

static inline String _s(const char *key, const char *def) {
  return gPrefs.getString(key, def);
}

// 从 NVS 读入内存，只做一次
static void load() {
  if (gLoaded) return;
  gPrefs.begin("chanrec", true);           // 只读打开
  gCfg.wifiSsid   = _s("wifi_ssid", DEFAULT_WIFI_SSID);
  gCfg.wifiPass   = _s("wifi_pass", DEFAULT_WIFI_PASS);
  gCfg.gwHost     = _s("gw_host",   DEFAULT_GW_HOST);
  gCfg.gwPort     = (uint16_t)gPrefs.getUShort("gw_port", DEFAULT_GW_PORT);
  gCfg.gwPath     = _s("gw_path",   DEFAULT_GW_PATH);
  gCfg.gwToken    = _s("gw_token",  DEFAULT_GW_TOKEN);
  gCfg.deviceName = _s("dev_name",  DEFAULT_DEVICE_NAME);
  gCfg.servoEnabled = gPrefs.getBool("servo_on", DEFAULT_SERVO_ENABLED != 0);
  gCfg.termsText    = _s("terms", "");
  gPrefs.end();

  if (gCfg.gwPort == 0) gCfg.gwPort = DEFAULT_GW_PORT;
  if (gCfg.gwPath.length() == 0) gCfg.gwPath = DEFAULT_GW_PATH;
  gLoaded = true;
}

static inline Config &get() { load(); return gCfg; }

static inline bool hasWiFi() { load(); return gCfg.wifiSsid.length() > 0; }

// 配网页面保存（全量写入，然后由调用方重启）
static void saveAll(const String &ssid, const String &pass,
                    const String &host, uint16_t port,
                    const String &path, const String &token) {
  gPrefs.begin("chanrec", false);
  gPrefs.putString("wifi_ssid", ssid);
  gPrefs.putString("wifi_pass", pass);
  gPrefs.putString("gw_host",   host);
  gPrefs.putUShort("gw_port",   port);
  gPrefs.putString("gw_path",   path);
  gPrefs.putString("gw_token",  token);
  gPrefs.end();

  gCfg.wifiSsid = ssid;
  gCfg.wifiPass = pass;
  gCfg.gwHost   = host;
  gCfg.gwPort   = port;
  gCfg.gwPath   = path;
  gCfg.gwToken  = token;
  gLoaded = true;
}

// 舵机开关：设置页点一下就切换，立即写 NVS（断电不丢）
static void setServoEnabled(bool on) {
  load();
  gPrefs.begin("chanrec", false);
  gPrefs.putBool("servo_on", on);
  gPrefs.end();
  gCfg.servoEnabled = on;
}

// 词表：配网网页保存后调用（重启生效）
static void setTerms(const String &t) {
  load();
  gPrefs.begin("chanrec", false);
  gPrefs.putString("terms", t);
  gPrefs.end();
  gCfg.termsText = t;
}

// 恢复出厂：只清 WiFi，网关设置保留（省得每次配网重填）
static void clearWifi() {
  gPrefs.begin("chanrec", false);
  gPrefs.remove("wifi_ssid");
  gPrefs.remove("wifi_pass");
  gPrefs.end();
  gCfg.wifiSsid = "";
  gCfg.wifiPass = "";
}

static void factoryReset() {
  gPrefs.begin("chanrec", false);
  gPrefs.clear();
  gPrefs.end();
  gLoaded = false;
  load();
}

// 网关口令打码，设置页展示用
static inline String tokenMask() {
  load();
  if (gCfg.gwToken.length() <= 8) return gCfg.gwToken.length() ? "****" : "(空)";
  return gCfg.gwToken.substring(0, 4) + "..." + gCfg.gwToken.substring(gCfg.gwToken.length() - 4);
}

}  // namespace settings
