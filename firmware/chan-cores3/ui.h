#pragma once

// ============================================================
//  ui.h · 界面原语（配色 / 状态栏 / 圆角按钮 / 折行）
//
//  只做"画"和"命中判断"，不含业务状态。
//  屏幕：CoreS3 320x240 横屏
// ============================================================

#include <M5Unified.h>
#include <vector>
#include "config.h"
#include "terms.h"

namespace ui {

// ---------------- 配色（深色主题，RGB565）----------------
static const uint16_t C_BG      = 0x0884;   // 深蓝黑 背景
static const uint16_t C_PANEL   = 0x1107;   // 面板
static const uint16_t C_CARD    = 0x1969;   // 卡片
static const uint16_t C_LINE    = 0x2965;   // 分隔线
static const uint16_t C_TEXT    = 0xEF7E;   // 主文字
static const uint16_t C_DIM     = 0x8D17;   // 次要文字
static const uint16_t C_GREEN   = 0x262B;   // 主操作
static const uint16_t C_RED     = 0xEA28;   // 录制 / 危险
static const uint16_t C_YELLOW  = 0xFDE4;   // 暂停
static const uint16_t C_CYAN    = 0x2EB7;   // 强调
static const uint16_t C_TERM    = 0x6E7F;   // 专有名词（青绿，加粗后更醒目）
static const uint16_t C_TRACK   = 0x2965;   // 未激活

// 字体（只用 efontCN_16，放大用 textSize，避免依赖不确定的字库）
#define F_BODY  (&fonts::efontCN_16)

struct Rect {
  int16_t x, y, w, h;
  bool hit(int16_t px, int16_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
  int16_t cx() const { return x + w / 2; }
  int16_t cy() const { return y + h / 2; }
};

static M5Canvas *cv = nullptr;
static void attach(M5Canvas *c) { cv = c; }

// ---------------- 颜色工具 ----------------
static inline uint16_t darken(uint16_t c, uint8_t div = 2) {
  uint16_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  return (uint16_t)(((r / div) << 11) | ((g / div) << 5) | (b / div));
}

// ---------------- 文字 ----------------
static inline void text(const char *s, int x, int y, uint16_t color) {
  cv->setTextColor(color);
  cv->setCursor(x, y);
  cv->print(s);
}

// 以 (cx,cy) 为中心画单行文字
static void textCenter(const char *s, int cx, int cy, uint16_t color, float size = 1.0f) {
  cv->setTextSize(size);
  int w = cv->textWidth(s);
  int h = cv->fontHeight();
  cv->setTextColor(color);
  cv->setCursor(cx - w / 2, cy - h / 2);
  cv->print(s);
  cv->setTextSize(1);
}

// 左对齐单行，垂直居中于 cy
static void textLeft(const char *s, int x, int cy, uint16_t color, float size = 1.0f) {
  cv->setTextSize(size);
  int h = cv->fontHeight();
  cv->setTextColor(color);
  cv->setCursor(x, cy - h / 2);
  cv->print(s);
  cv->setTextSize(1);
}

// ---------------- 按钮 ----------------
static void button(const Rect &r, const char *label, uint16_t bg,
                   uint16_t fg = TFT_WHITE, bool pressed = false, float size = 1.0f) {
  uint16_t fill = pressed ? darken(bg, 2) : bg;
  cv->fillRoundRect(r.x, r.y, r.w, r.h, 9, fill);
  cv->drawRoundRect(r.x, r.y, r.w, r.h, 9, darken(bg, 2));
  textCenter(label, r.cx(), r.cy() + 1, fg, size);
}

// 描边按钮（次要操作）
static void buttonGhost(const Rect &r, const char *label, uint16_t lineColor,
                        bool pressed = false, float size = 1.0f) {
  cv->fillRoundRect(r.x, r.y, r.w, r.h, 9, pressed ? C_CARD : C_BG);
  cv->drawRoundRect(r.x, r.y, r.w, r.h, 9, lineColor);
  textCenter(label, r.cx(), r.cy() + 1, lineColor, size);
}

// ---------------- 状态栏 ----------------
static void drawSignal(int x, int y, bool ok) {
  for (int i = 0; i < 3; i++) {
    int h = 4 + i * 3;
    cv->fillRect(x + i * 5, y - h, 3, h, ok ? C_GREEN : 0x4208);
  }
}

static void drawBattery(int x, int y, int pct) {
  cv->drawRect(x, y, 20, 10, C_DIM);
  cv->fillRect(x + 20, y + 3, 2, 4, C_DIM);
  int w = (17 * constrain(pct, 0, 100)) / 100;
  uint16_t c = pct > 20 ? C_GREEN : C_RED;
  if (w > 0) cv->fillRect(x + 1, y + 1, w, 8, c);
}

// 顶栏：left 文字 + 可选闪烁红点 + 右侧 WiFi/电量
static void topBar(const char *left, uint16_t leftColor, bool blink,
                   bool wifiOk, int battery) {
  cv->fillRect(0, 0, UI_SCREEN_W, UI_TOP_H, C_PANEL);

  if (blink) {
    if ((millis() / 500) % 2 == 0) cv->fillCircle(13, UI_TOP_H / 2, 5, C_RED);
    textLeft(left, 25, UI_TOP_H / 2, leftColor);
  } else {
    textLeft(left, 10, UI_TOP_H / 2, leftColor);
  }

  drawBattery(UI_SCREEN_W - 30, 8, battery);

  // 电量数字右对齐到电池左侧
  char b[8];
  snprintf(b, sizeof(b), "%d%%", constrain(battery, 0, 100));
  cv->setTextSize(1);
  int bw = cv->textWidth(b);
  textLeft(b, UI_SCREEN_W - 36 - bw, UI_TOP_H / 2, C_DIM);

  drawSignal(UI_SCREEN_W - 96, UI_TOP_H - 8, wifiOk);

  cv->drawFastHLine(0, UI_TOP_H, UI_SCREEN_W, C_LINE);
}

// ---------------- 折行（按像素宽度）----------------
static inline int _utf8Len(uint8_t c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

static void wrap(const String &s, int maxW, std::vector<String> &out) {
  if (s.isEmpty()) return;
  cv->setTextSize(1);
  String line = "";
  int i = 0;
  while (i < (int)s.length()) {
    int n = _utf8Len((uint8_t)s[i]);
    if (i + n > (int)s.length()) n = 1;
    String ch = s.substring(i, i + n);
    i += n;
    if (ch == "\n") { out.push_back(line); line = ""; continue; }
    if (cv->textWidth((line + ch).c_str()) > maxW && line.length() > 0) {
      out.push_back(line);
      line = ch;
    } else {
      line += ch;
    }
  }
  if (line.length() > 0) out.push_back(line);
}

// ---------------- 专有名词折行（词不拆行）----------------
// 和 wrap 的区别：先把词表命中的词整个当成一个 token，
// 放不下就整体换行，不会出现"银"在上一行、"联"在下一行导致加粗失效。
static void wrapTerms(const String &s, int maxW, std::vector<String> &out) {
  if (s.isEmpty()) return;
  cv->setTextSize(1);
  String line = "";
  int i = 0;
  int n = (int)s.length();

  while (i < n) {
    int m = terms::matchLen(s, i);
    int step;
    String tk;

    if (m > 0) {
      step = m;
      tk   = s.substring(i, i + step);
    } else {
      step = _utf8Len((uint8_t)s[i]);
      if (i + step > n) step = 1;
      tk = s.substring(i, i + step);
    }

    if (tk == "\n") {
      out.push_back(line);
      line = "";
      i += step;
      continue;
    }

    if (line.length() > 0 && cv->textWidth((line + tk).c_str()) > maxW) {
      out.push_back(line);
      line = tk;
    } else {
      line += tk;
    }
    i += step;
  }
  if (line.length() > 0) out.push_back(line);
}

// ---------------- 富文本：专有名词加粗 ----------------
// 位图字体没有粗体字重，用"右移 1px 再画一遍"模拟；
// 320 宽的小屏上，这 1px 的厚度差看起来就是加粗。
static void textRich(const String &s, int x, int y, uint16_t base, uint16_t hl) {
  cv->setTextSize(1);
  int cx = x;
  int i  = 0;
  int n  = (int)s.length();

  while (i < n) {
    int m = terms::matchLen(s, i);

    if (m > 0) {
      String seg = s.substring(i, i + m);
      cv->setTextColor(hl);
      cv->setCursor(cx, y);
      cv->print(seg);
      cv->setCursor(cx + TERMS_BOLD_STEP, y);   // 第二遍 → 视觉加粗
      cv->print(seg);
      cx += cv->textWidth(seg.c_str()) + TERMS_BOLD_STEP;
      i  += m;
    } else {
      int step = _utf8Len((uint8_t)s[i]);
      if (i + step > n) step = 1;
      String ch = s.substring(i, i + step);
      cv->setTextColor(base);
      cv->setCursor(cx, y);
      cv->print(ch);
      cx += cv->textWidth(ch.c_str());
      i  += step;
    }
  }
}

}  // namespace ui
