// StickS3_P0.ino
// 桌面会议记录机器人 - P0：按键触发收音 + 屏幕实时显示"识别文字"（不是波形）
// 硬件：M5Stack StickS3 (K150)
// 链路：单击正面键(BtnA) 开录 → 每 ~1.5s 把音频(WAV) POST 到本地/云端网关
//       → 网关转写文字回传 → 屏幕滚动显示文字；再单击停止，回待机
//
// ⚠️ ESP32 算力跑不了语音识别，文字来自"网关"。本固件只负责采集+显示。
//    网关见同目录 asr_gateway.py：先 `python asr_gateway.py --mock` 跑通，再切真实 ASR。
//
// 依赖（Arduino IDE）：
//   开发板管理器安装 "M5StickS3"（Board Manager >= 3.2.5）
//   库：M5Unified >= 0.2.12 ; M5GFX >= 0.2.18
// 平台：Arduino IDE / PlatformIO（见同目录 platformio.ini）

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ====================== 配置：烧录前改这里 ======================
#define WIFI_SSID      "YOUR_WIFI_SSID"     // ← 改成你的 WiFi
#define WIFI_PASS      "YOUR_WIFI_PASS"     // ← 改成你的 WiFi 密码
#define GATEWAY_HOST   "192.168.x.x"        // ← 跑 asr_gateway.py 的机器 IP（同一 WiFi）
#define GATEWAY_PORT   8000
// ===============================================================

// ---- 音频参数 ----
static constexpr int   SAMPLE_RATE   = 16000;          // 16kHz，满足语音/后续妙记
static constexpr size_t FRAME_SAMPLES = 4000;          // 每 0.25s 录一帧 (0.25*16000)
static constexpr int   CHUNK_FRAMES  = 6;              // 6 帧 ≈ 1.5s 上传一次
static constexpr size_t CHUNK_SAMPLES = FRAME_SAMPLES * CHUNK_FRAMES;

static int16_t  chunkBuf[CHUNK_SAMPLES];
static size_t   chunkIdx = 0;
static uint8_t  wavBuf[44 + CHUNK_SAMPLES * 2];        // WAV 头(44) + 数据

// ---- 状态 ----
enum State { BOOT, WIFI_CONNECTING, IDLE, RECORDING };
static State   state = BOOT;
static bool     recording = false;
static uint32_t rec_start_ms = 0;
static bool     blink = true;
static uint32_t blink_timer = 0;
static String   fullText;                              // 累计识别文字
static bool     textDirty = true;
static bool     netErr = false;
static uint32_t netErrT = 0;

// ---- 屏幕布局（横屏 240x135）----
static int32_t SCR_W = 240;
static int32_t SCR_H = 135;
static const int32_t TOP_H = 18;                       // 顶部状态栏高度

// ---------- 小工具：UTF-8 逐字 ----------
static String nextChar(const String& s, int& i) {
  uint8_t c = s[i];
  int n = (c < 0x80) ? 1 : (c >> 4 == 0xC) ? 2 : (c >> 4 == 0xD) ? 2 : (c >> 4 == 0xE) ? 3 : (c >> 4 == 0xF) ? 4 : 1;
  String out = s.substring(i, i + n);
  i += n;
  return out;
}

// ---------- 画顶部状态栏 ----------
static void drawTopBar() {
  M5.Display.fillRect(0, 0, SCR_W, TOP_H, TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(2, 2);
  if (state == WIFI_CONNECTING) {
    M5.Display.print("WIFI...");
  } else if (state == RECORDING) {
    M5.Display.print("REC");
    if (blink) M5.Display.fillCircle(SCR_W - 40, 8, 6, TFT_RED);
    M5.Display.setCursor(40, 2);
    M5.Display.printf("%lus", (unsigned long)((millis() - rec_start_ms) / 1000));
    M5.Display.setCursor(SCR_W - 30, 2);
    M5.Display.print(netErr ? "NET!" : "NET");
  } else if (state == IDLE) {
    M5.Display.print("BtnA=REC");
  }
}

// ---------- 渲染文字区（按宽度折行，显示末尾若干行） ----------
static void renderText() {
  M5.Display.fillRect(0, TOP_H, SCR_W, SCR_H - TOP_H, TFT_BLACK);
  if (fullText.length() == 0) {
    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(4, TOP_H + 4);
    M5.Display.print("(说话后这里出文字)");
    return;
  }
  const int lineH = 20;
  int maxLines = (SCR_H - TOP_H) / lineH;
  int maxW = SCR_W - 6;
  // 折行
  std::vector<String> lines;
  String cur;
  int i = 0;
  while (i < fullText.length()) {
    String ch = nextChar(fullText, i);
    String test = cur + ch;
    if (M5.Display.textWidth(test) > maxW && cur.length() > 0) {
      lines.push_back(cur);
      cur = ch;
    } else {
      cur = test;
    }
  }
  if (cur.length() > 0) lines.push_back(cur);
  // 只画最后 maxLines 行
  int start = (lines.size() > (size_t)maxLines) ? lines.size() - maxLines : 0;
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  for (size_t k = start; k < lines.size(); k++) {
    int y = TOP_H + (k - start) * lineH + 2;
    M5.Display.setCursor(4, y);
    M5.Display.print(lines[k]);
  }
}

// ---------- 拼 WAV 头 ----------
static size_t buildWav(size_t dataBytes) {
  uint32_t total = 36 + dataBytes;
  uint32_t sr = SAMPLE_RATE;
  memcpy(wavBuf, "RIFF", 4);
  memcpy(wavBuf + 4, &total, 4);
  memcpy(wavBuf + 8, "WAVE", 4);
  memcpy(wavBuf + 12, "fmt ", 4);
  uint32_t sub1 = 16; memcpy(wavBuf + 16, &sub1, 4);
  uint16_t fmt = 1;  memcpy(wavBuf + 20, &fmt, 2);
  uint16_t ch = 1;   memcpy(wavBuf + 22, &ch, 2);
  memcpy(wavBuf + 24, &sr, 4);
  uint32_t br = sr * 2; memcpy(wavBuf + 28, &br, 4);
  uint16_t ba = 2;   memcpy(wavBuf + 32, &ba, 2);
  uint16_t bps = 16; memcpy(wavBuf + 34, &bps, 2);
  memcpy(wavBuf + 36, "data", 4);
  memcpy(wavBuf + 40, &dataBytes, 4);
  return 44 + dataBytes;
}

// ---------- 上传一帧音频，取回文字 ----------
static void postChunk() {
  size_t dataBytes = chunkIdx * 2;
  size_t total = buildWav(dataBytes);
  memcpy(wavBuf + 44, chunkBuf, dataBytes);

  HTTPClient http;
  String url = "http://" + String(GATEWAY_HOST) + ":" + String(GATEWAY_PORT) + "/asr";
  http.begin(url);
  http.addHeader("Content-Type", "audio/wav");
  http.setTimeout(8000);
  int code = http.POST(wavBuf, total);
  if (code == HTTP_CODE_OK) {
    String resp = http.getString();
    int p = resp.indexOf("\"text\"");
    if (p >= 0) {
      int c = resp.indexOf(':', p) + 1;
      while (c < resp.length() && (resp[c] == ' ' || resp[c] == '"')) c++;
      int e = resp.indexOf('"', c);
      if (e > c) {
        String t = resp.substring(c, e);
        t.trim();
        if (t.length() > 0) {
          if (fullText.length() > 0) fullText += " ";
          fullText += t;
          if (fullText.length() > 2000) fullText = fullText.substring(fullText.length() - 1500);
          textDirty = true;
        }
      }
    }
    netErr = false;
  } else {
    netErr = true;
    netErrT = millis();
  }
  http.end();
}

// ---------- 状态切换 ----------
static void startRecording() {
  chunkIdx = 0;
  fullText = "";
  textDirty = true;
  netErr = false;
  rec_start_ms = millis();
  recording = true;
  state = RECORDING;
  M5.Display.fillScreen(TFT_BLACK);
}
static void stopRecording() {
  recording = false;
  state = IDLE;
  M5.Display.fillScreen(TFT_BLACK);
}

void setup() {
  M5.begin();
  M5.Speaker.end();                 // 上电先消音，麦与扬声器共用 ES8311
  M5.Mic.begin();                    // 开麦（避免啸叫，必须先 Speaker.end）
  M5.Display.setRotation(1);         // 横屏 240x135
  M5.Display.setBrightness(128);
  M5.Display.setFont(&fonts::lgfxJapanGothic_16);  // 含汉字字形（日文字体覆盖多数汉字）
  M5.Display.setTextSize(1);
  SCR_W = M5.Display.width();
  SCR_H = M5.Display.height();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(4, 4);
  M5.Display.print("connecting WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  state = WIFI_CONNECTING;
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(300); tries++;
    M5.Display.print(".");
  }
  M5.Display.fillScreen(TFT_BLACK);
  if (WiFi.status() == WL_CONNECTED) {
    state = IDLE;
    drawTopBar();
  } else {
    M5.Display.setCursor(4, 4);
    M5.Display.print("WiFi FAIL");
    M5.Display.setCursor(4, 24);
    M5.Display.print("check config");
    while (true) delay(1000);
  }
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {
    if (state == IDLE)        startRecording();
    else if (state == RECORDING) stopRecording();
  }

  if (state == RECORDING) {
    int16_t frame[FRAME_SAMPLES];
    if (M5.Mic.record(frame, FRAME_SAMPLES, SAMPLE_RATE)) {
      memcpy(chunkBuf + chunkIdx, frame, FRAME_SAMPLES * 2);
      chunkIdx += FRAME_SAMPLES;
      if (chunkIdx >= CHUNK_SAMPLES) {
        postChunk();
        chunkIdx = 0;
      }
    }
    // 每帧刷新状态栏（闪烁/计时/网络）
    if (millis() - blink_timer >= 500) { blink_timer = millis(); blink = !blink; }
    if (netErr && millis() - netErrT > 2000) netErr = false;
    drawTopBar();
    if (textDirty) { renderText(); textDirty = false; }
  }
}
