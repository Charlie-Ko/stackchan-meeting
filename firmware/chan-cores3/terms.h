#pragma once

// ============================================================
//  terms.h · 专有名词词表（屏显加粗用）
//
//  三级来源，优先级从低到高：
//    1. 内置默认（编译进固件，改这里要重烧）
//    2. SD 卡 /terms.txt（一行一个词，# 开头是注释，可热插拔）
//    3. 配网网页里粘贴保存（NVS，手机就能改，不用拔卡）
//
//  为什么要有词表：ASR 对人名、内部黑话经常同音错字，
//  屏上把已知词加粗，一眼就能看出"这个词我认得，别怀疑"。
// ============================================================

#include <Arduino.h>
#include <vector>
#include <algorithm>
#include <M5Unified.h>
#include "config.h"
#include "settings.h"

#if TERMS_SD_ENABLE
#include <SPI.h>
#include <SD.h>
#endif

namespace terms {

static std::vector<String> gList;
static uint8_t  gSource   = 0;      // 0=内置 1=SD 2=网页
static uint32_t gLoadMs   = 0;

// 内置兜底词：这些是占位，按你实际会议改成自己的
static const char *kBuiltin[] = {
  "项目", "需求", "排期", "上线", "联调", "测试", "发布",
  "分公司", "总公司", "接口", "版本", "回滚", "评审", "渠道",
  // 测试词：验证同音字加粗与长词优先匹配（没插 SD 卡也能生效）
  "郭靖", "杨康", "降龙十八掌",
};

static inline const char *sourceName() {
  switch (gSource) {
    case 1:  return "SD";
    case 2:  return "网页";
    default: return "内置";
  }
}

static void addTerm(String t) {
  t.trim();
  if (t.length() < 2) return;                 // 单字节（英文单字母）不要
  if ((int)t.length() > TERMS_MAX_LEN) return;
  if ((int)gList.size() >= TERMS_MAX) return;
  for (auto &e : gList) if (e == t) return;   // 去重
  gList.push_back(t);
}

static void clear() { gList.clear(); }

static void sortByLen() {
  std::sort(gList.begin(), gList.end(),
            [](const String &a, const String &b) { return a.length() > b.length(); });
}

// 一段文本 → 词表（换行分隔，# 开头跳过）
static int parseText(const String &text) {
  int cnt = 0;
  int start = 0;
  while (start <= (int)text.length()) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = text.length();
    String line = text.substring(start, nl);
    start = nl + 1;

    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    // 允许 "词,别名" 或 "词 别名" 的写法，逗号/制表符切分
    int cut = 0;
    while (cut < (int)line.length()) {
      int sep = cut;
      while (sep < (int)line.length() && line[sep] != ',' && line[sep] != '\t') sep++;
      String w = line.substring(cut, sep);
      int before = (int)gList.size();
      addTerm(w);
      if ((int)gList.size() > before) cnt++;
      cut = sep + 1;
    }
  }
  return cnt;
}

#if TERMS_SD_ENABLE
// 从 SD 卡读；读不到就返回 false，绝不影响开机
static bool loadFromSd() {
  int sclk = M5.getPin(m5::pin_name_t::sd_spi_sclk);
  int miso = M5.getPin(m5::pin_name_t::sd_spi_miso);
  int mosi = M5.getPin(m5::pin_name_t::sd_spi_mosi);
  int ss   = M5.getPin(m5::pin_name_t::sd_spi_ss);
  if (sclk < 0 || miso < 0 || mosi < 0 || ss < 0) {
    Serial.println("[TERM] no sd pins");
    return false;
  }

  static SPIClass sdSpi(HSPI);
  sdSpi.begin(sclk, miso, mosi, ss);
  if (!SD.begin(ss, sdSpi, 10000000)) {
    Serial.println("[TERM] sd mount failed");
    sdSpi.end();
    return false;
  }

  File f = SD.open(TERMS_SD_FILE, FILE_READ);
  if (!f) {
    Serial.println("[TERM] no " TERMS_SD_FILE);
    SD.end();
    sdSpi.end();
    return false;
  }

  String all;
  unsigned int cap = (unsigned int)f.size() + 1;
  if (cap > 4096) cap = 4096;
  all.reserve(cap);
  while (f.available() && all.length() < 4096) all += (char)f.read();
  f.close();
  SD.end();          // 读完立刻放总线，别跟屏幕抢 SPI
  sdSpi.end();

  int n = parseText(all);
  Serial.printf("[TERM] sd loaded %d\n", n);
  return n > 0;
}
#endif

// 启动时调一次
static void load() {
  gLoadMs = millis();
  clear();
  gSource = 0;

  // 1) 内置兜底
  for (auto w : kBuiltin) addTerm(String(w));

  // 2) SD 卡覆盖
#if TERMS_SD_ENABLE
  {
    std::vector<String> saved = gList;
    clear();
    if (loadFromSd()) {
      gSource = 1;
    } else {
      gList = saved;
    }
  }
#endif

  // 3) 网页保存的优先级最高
  {
    const String &nvs = settings::get().termsText;
    if (nvs.length() >= 2) {
      std::vector<String> saved = gList;
      uint8_t savedSrc = gSource;
      clear();
      if (parseText(nvs) > 0) {
        gSource = 2;
      } else {
        gList = saved;
        gSource = savedSrc;
      }
    }
  }

  sortByLen();
  Serial.printf("[TERM] total %u from %s\n", (unsigned)gList.size(), sourceName());
}

static inline int      count()  { return (int)gList.size(); }
static inline uint8_t  source() { return gSource; }

// 在 s 的位置 i 尝试匹配某个词，返回命中的字节长度（0 = 没命中）
// 词表已按长度降序，先命中的就是最长匹配
static int matchLen(const String &s, int i) {
  if (gList.empty()) return 0;
  int n = (int)s.length();
  if (i >= n) return 0;
  const char c0 = s[i];
  const char *base = s.c_str() + i;
  for (auto &t : gList) {
    int tl = (int)t.length();
    if (tl == 0 || i + tl > n) continue;
    if (t[0] != c0) continue;
    if (memcmp(base, t.c_str(), tl) == 0) return tl;
  }
  return 0;
}

// 整串里是否包含任何词（给"这行有重点"之类的判断用）
static bool hasAny(const String &s) {
  for (int i = 0; i < (int)s.length(); i++) {
    if (matchLen(s, i) > 0) return true;
  }
  return false;
}

}  // namespace terms
