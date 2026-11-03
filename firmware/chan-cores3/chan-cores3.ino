// ============================================================
//  Chan 桌面机器人 · 会议记录固件（M5Stack CoreS3）
//  v0.4.0
//
//  相对 v0.3 新增：
//    1. 舵机可开关（默认关）；录音期间自动卸扭矩 + 断 VM_EN 电源 → 彻底无声
//    2. 专有名词词表：SD 卡 /terms.txt 或配网页粘贴，命中即加粗高亮
//
//  v0.3 已有：
//    WiFi 手机配网页改配置存 NVS，不用重烧；六屏状态机
//
//  交互总览：
//    主界面      [开始录音] [设置]      长按 2.5 秒 → 配网
//    录音中      [暂停]     [结束]
//    暂停中      [继续]     [结束]
//    总结页      [新建会议] [重启设备]   ⬅ 左侧显示本场数据 + 右侧二维码，
//                                        手机扫码看纪要 / 文字记录 / 下载录音
//    设置页      [重新配网] [重启] [返回]   点「舵机」那一行切开关
//
//  硬件要点（CoreS3）：
//    · 双麦 ES7210，M5Unified 的 Mic 类
//    · 麦克风与扬声器共用 I2S_NUM_1，录音前必须 Speaker.end()
//    · 无物理按键，屏下是触摸虚拟键；本版只用屏幕坐标判定
//    · 舵机在 StackChan 底座：SCS0009 串行总线 UART1@1Mbps，TX=G6/RX=G7，
//      电源由 PY32 IO 扩展器（I2C 0x6F）pin0 控制
//
//  烧录：Arduino IDE（板卡 M5Stack CoreS3 + M5Unified≥0.2.12）
//        或 PlatformIO（pio run -t upload）
//  配置：不用改代码，开机手机配网
// ============================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <vector>
#include "qrcode.h"

#include "config.h"
#include "settings.h"
#include "terms.h"
#include "motion.h"
#include "ui.h"
#include "provision.h"
#include "vad.h"

// ---------------- 全局对象 ----------------
M5Canvas canvas(&M5.Display);
WebSocketsClient ws;

enum State {
  ST_BOOT,
  ST_HOME,
  ST_CONNECTING,
  ST_WAIT_READY,
  ST_RECORDING,
  ST_PAUSING,
  ST_PAUSED,
  ST_STOPPING,
  ST_SUMMARY,
  ST_SETTINGS,
  ST_PROVISION,
  ST_ERROR
};

static State state = ST_BOOT;

// 按钮热区（底部按钮栏，320x240）
static ui::Rect R_MAIN  = {8,   UI_BTN_Y, 226, UI_BTN_H};   // 主按钮
static ui::Rect R_GEAR  = {240, UI_BTN_Y, 72,  UI_BTN_H};   // 设置
static ui::Rect R_LEFT  = {8,   UI_BTN_Y, 150, UI_BTN_H};
static ui::Rect R_RIGHT = {166, UI_BTN_Y, 146, UI_BTN_H};
static ui::Rect R_B1    = {8,   UI_BTN_Y, 99,  UI_BTN_H};
static ui::Rect R_B2    = {111, UI_BTN_Y, 99,  UI_BTN_H};
static ui::Rect R_B3    = {214, UI_BTN_Y, 98,  UI_BTN_H};
static ui::Rect R_SERVO = {8, 97, 304, 24};                 // 设置页「舵机」整行热区

static int16_t micBuf[FRAME_SAMPLES];

// ---------------- WebSocket 生命周期 ----------------
// ⚠️ 大坑：WebSocketsClient::loop() 的判断是
//      if (millis() - _lastConnectionFail < _reconnectInterval) return;
// 也就是说 setReconnectInterval(0) = "失败后 0ms 就重连"，即无限速重连，不是"关闭自动重连"。
// 之前所有想"关掉重连"的地方都写的 0，于是空闲页一旦冒出连接就变成风暴：
// 实测 2 分钟 319 次接入，每次都让网关开一个百炼 task（白烧额度、还会撞 QPS 影响真会话）。
// 正确做法：把间隔设成 1 天 ≈ 关闭；真正要建会话时 connectSession() 会改回 3000。
static void wsStopReconnect() {
  ws.setReconnectInterval(WS_RECONNECT_OFF_MS);
}

// VAD 判定要上传的帧，最终都从这里出去（二进制裸 PCM，不 base64，不做 wav 封装）
static void pcmSink(const int16_t *pcm, size_t samples) {
  if (ws.isConnected()) ws.sendBIN((uint8_t *)pcm, samples * sizeof(int16_t));
}

static String   gCommitted = "";     // 已定稿字幕
static String   gPartial   = "";     // 当前句（会反复刷新）
static String   gErrMsg    = "";
static bool     gDirty     = true;
static uint32_t gLastDraw  = 0;

static uint32_t gAccumMs   = 0;      // 已录音累计（暂停过会累加）
static uint32_t gSegStart  = 0;      // 当前段起点
static uint16_t gSentCount = 0;
static uint32_t gCharCount = 0;
// 会议结束页的二维码内容：网关在 done 事件里下发（形如 http://<host>/m/<key>/<id>）。
// 手机扫开就能看纪要、文字记录、下载录音。
static String   gMeetingUrl = "";
static uint32_t gMarkAt    = 0;      // 各状态超时起点

static bool     gWifiOk    = false;
static uint32_t gWifiTryAt = 0;
static bool     gWifiStarted = false;

static uint32_t gPressStart = 0;
static bool     gPressFired = false;
static bool     gWantDisconnect = false;   // 在 loop 里断开，避免回调内重入

static bool     gOtaStarted = false;       // ArduinoOTA 已注册
static bool     gOtaActive  = false;       // 正在接收固件，此时不录音

static int      gBattery   = 100;
static uint32_t gBatAt     = 0;
static uint32_t gTick      = 0;

// ---------------- 小工具 ----------------
static inline void dirty() { gDirty = true; }

static int batteryPct() {
  if (millis() - gBatAt > 3000) {
    gBatAt = millis();
    int v = M5.Power.getBatteryLevel();
    if (v >= 0 && v <= 100) gBattery = v;
  }
  return gBattery;
}

static String fmtDur(uint32_t ms) {
  uint32_t s = ms / 1000;
  char b[16];
  if (s >= 3600) snprintf(b, sizeof(b), "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60), (unsigned)(s % 60));
  else           snprintf(b, sizeof(b), "%02u:%02u",    (unsigned)(s / 60),   (unsigned)(s % 60));
  return String(b);
}

static uint32_t utf8Count(const String &s) {
  uint32_t n = 0;
  for (size_t i = 0; i < s.length(); i++)
    if (((uint8_t)s[i] & 0xC0) != 0x80) n++;
  return n;
}

static uint32_t recMs() {
  return gAccumMs + (state == ST_RECORDING ? (millis() - gSegStart) : 0);
}

static void trimCommitted() {
  if ((int)gCommitted.length() <= MAX_COMMIT_CHARS) return;
  int cut = gCommitted.length() - MAX_COMMIT_CHARS;
  while (cut < (int)gCommitted.length() && ((uint8_t)gCommitted[cut] & 0xC0) == 0x80) cut++;
  gCommitted = gCommitted.substring(cut);
}

// 把一段文字并入正文。定稿、收尾兜底都走这里，保证计数与裁剪只有一处逻辑。
static void commitText(const String &s) {
  if (!s.length()) return;
  gCommitted += s;
  gCharCount += utf8Count(s);
  gSentCount++;
  trimCommitted();
}

static bool isPressed(const ui::Rect &r) {
  if (!M5.Touch.isEnabled()) return false;
  auto td = M5.Touch.getDetail();
  return td.isPressed() && r.hit(td.x, td.y);
}

// ---------------- 录音 ----------------
static void beginMic() {
  // 先让舵机彻底断电：不保位、不带电，省得电机高频啸叫混进麦克风
  motion::lock();

  M5.Speaker.end();
  delay(20);

  auto cfg = M5.Mic.config();
  cfg.sample_rate = SAMPLE_RATE;
  cfg.stereo = false;
  cfg.magnification = 16;
  cfg.noise_filter_level = 0;
  // 512×24 = 12288 采样 ≈ 0.77s 缓冲。整屏重绘/网络抖动会占几十 ms，
  // 缓冲太浅（旧值 256×16=256ms）一旦溢出就丢音 → 识别错字、断词
  cfg.dma_buf_len = 512;
  cfg.dma_buf_count = 24;
  M5.Mic.config(cfg);

  if (!M5.Mic.begin()) {
    gErrMsg = "麦克风初始化失败，请重启设备";
    state = ST_ERROR;
    dirty();
    return;
  }
  gSegStart = millis();
  state = ST_RECORDING;
  dirty();
  Serial.printf("[REC] start @%uHz\n", (unsigned)SAMPLE_RATE);
}

static void endMic() {
  if (M5.Mic.isEnabled()) M5.Mic.end();
  delay(10);
}

// ---------------- 音频体检 ----------------
// 识别不准的时候先看这三个数，别急着怪模型：
//   peak 长期在 -1dB 以上 / clip 有比例 → 削波破音，必错字（离麦太近或增益太大）
//   rms 长期低于 -50dB → 声音太小，信噪比不够（离麦太远或增益不足）
//   正常说话 10~30cm：peak ≈ -6dB、rms ≈ -25~-35dB、clip < 0.1%
#if AUD_STATS
static void audStats(const int16_t *pcm, size_t n) {
  static uint32_t tAt = 0, clip = 0, total = 0;
  static int16_t  peak = 0;
  static uint64_t sumsq = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t v = pcm[i];
    int32_t a = v < 0 ? -v : v;
    if (a > peak) peak = (int16_t)(a > 32767 ? 32767 : a);
    if (a >= 32000) clip++;
    sumsq += (uint64_t)(v * v);
  }
  total += n;
  if (millis() - tAt < 2000) return;
  tAt = millis();
  float rms = total ? sqrtf((float)sumsq / (float)total) : 0.0f;
  Serial.printf("[AUD] peak=%.1fdB rms=%.1fdB clip=%.2f%%\n",
                peak ? 20.0f * log10f(peak / 32768.0f) : -90.0f,
                rms  ? 20.0f * log10f(rms  / 32768.0f) : -90.0f,
                total ? clip * 100.0f / total : 0.0f);
  peak = 0; sumsq = 0; total = 0; clip = 0;
}
#endif

// ---------------- OTA（WiFi 无线烧录）----------------
// 只在 WiFi 连上后注册一次；录音期间收到升级请求会先停录音再收包
static void otaEnsure() {
  if (gOtaStarted || WiFi.status() != WL_CONNECTED) return;

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setPort(OTA_PORT);

  ArduinoOTA.onStart([]() {
    gOtaActive = true;
    if (M5.Mic.isEnabled()) M5.Mic.end();
    motion::lock();
    wsStopReconnect();
    ws.disconnect();
    Serial.println("[OTA] start — 已停录音/网关，开始接收固件");
    M5.Display.setFont(F_BODY);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.fillRect(0, 0, UI_SCREEN_W, UI_TOP_H);
    M5.Display.drawString("OTA 升级中…", 8, 5);
  });

  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static int last = -1;
    int pct = total ? (int)((uint64_t)done * 100 / total) : 0;
    if (pct / 5 == last / 5) return;
    last = pct;
    Serial.printf("[OTA] %d%%\n", pct);
    M5.Display.fillRect(0, 24, UI_SCREEN_W, 24);
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.drawString(String(pct) + "%", 8, 24);
  });

  ArduinoOTA.onEnd([]() { Serial.println("[OTA] 完成，重启"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] 失败 code=%u\n", (unsigned)e); });

  Serial.println("[OTA] begin() ...");
  ArduinoOTA.begin();
  gOtaStarted = true;
  Serial.printf("[OTA] ready  %s.local  ip=%s  port=%u\n",
                OTA_HOSTNAME, WiFi.localIP().toString().c_str(), OTA_PORT);}

// ---------------- 无线 / 会话 ----------------
// WiFi 事件打点：连不上时能看到 reason code，别再靠猜
// 常见 reason：2=AUTH_EXPIRE 4=AP 不在(201=NO_AP) 15=4WAY 握手超时(多为密码错/信号差) 8=离开覆盖区
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WiFi] disconnected reason=%u\n", (unsigned)info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] got ip %s rssi=%d\n",
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      break;
    default:
      break;
  }
}

static void wifiBegin() {
  settings::Config &c = settings::get();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // 实时音频流必须关 modem 睡眠：默认 PS_MIN_MODEM 会把上行帧憋到 DTIM 信标，
  // 平白多出 100~300ms 抖动 —— 这就是"回显慢半拍"的主要来源之一
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
  WiFi.begin(c.wifiSsid.c_str(), c.wifiPass.c_str());
  gWifiTryAt = millis();
  gWifiStarted = true;
  gWifiOk = false;
  dirty();
  Serial.printf("[WiFi] begin ssid=%s\n", c.wifiSsid.c_str());
}

static bool wifiFailed() {
  return gWifiStarted && !gWifiOk && (millis() - gWifiTryAt > WIFI_CONNECT_MS);
}

static void connectSession() {
  gErrMsg = "";
  if (WiFi.status() != WL_CONNECTED) {
    gErrMsg = "WiFi 未连接，请先配网";
    state = ST_ERROR;
    dirty();
    return;
  }
  settings::Config &c = settings::get();
  String path = c.gwPath;
  path += (path.indexOf('?') >= 0 ? "&" : "?");
  path += "token=" + c.gwToken;

  ws.setReconnectInterval(3000);
  ws.begin(c.gwHost.c_str(), c.gwPort, path.c_str());

  state = ST_CONNECTING;
  gMarkAt = millis();
  dirty();
  Serial.printf("[WS] begin %s:%u%s\n", c.gwHost.c_str(), c.gwPort, path.c_str());
}

static void startSession() {
  if (!settings::hasWiFi() || wifiFailed()) {
    // 没配网 / 连不上：直接进配网
    provision::begin();
    state = ST_PROVISION;
    dirty();
    return;
  }
  gCommitted = "";
  gPartial = "";
  gErrMsg = "";
  gAccumMs = 0;
  gSegStart = 0;
  gSentCount = 0;
  gCharCount = 0;
  gMeetingUrl = "";      // 上一场的二维码必须清掉，否则新会议没结束时还挂着旧链接
#if VAD_ENABLE
  vad::reset();
#endif
  connectSession();
}

static void printVadStats(const char *tag) {
#if VAD_ENABLE
  Serial.printf("[VAD] %s 收%u帧/发%u帧 省%.0f%%\n", tag,
                (unsigned)vad::framesIn(), (unsigned)vad::framesOut(),
                vad::savedRatio() * 100.0f);
#endif
}

static void pauseSession() {
  if (state != ST_RECORDING) return;
  gAccumMs += millis() - gSegStart;
  endMic();
  printVadStats("暂停");
  ws.sendTXT("{\"t\":\"stop\"}");
  state = ST_PAUSING;
  gMarkAt = millis();
  dirty();
  Serial.println("[REC] pause");
}

static void stopSession() {
  if (state != ST_RECORDING) return;
  gAccumMs += millis() - gSegStart;
  endMic();
  printVadStats("结束");
  ws.sendTXT("{\"t\":\"stop\"}");
  state = ST_STOPPING;
  gMarkAt = millis();
  dirty();
  Serial.println("[REC] stop");
}

static void resumeSession() {
  if (state != ST_PAUSED) return;
  if (gCommitted.length()) gCommitted += "\n";   // 段落分隔
  gPartial = "";
  connectSession();
}

static void newMeeting() {
  gCommitted = "";
  gPartial = "";
  gSentCount = 0;
  gCharCount = 0;
  gMeetingUrl = "";
  gAccumMs = 0;
#if VAD_ENABLE
  vad::reset();
#endif
  connectSession();
}

static void reboot() {
  Serial.println("[SYS] reboot");
  canvas.fillSprite(ui::C_BG);
  ui::textCenter("正在重启…", 160, 120, ui::C_TEXT);
  canvas.pushSprite(0, 0);
  delay(300);
  ESP.restart();
}

// ---------------- WS 事件 ----------------
static void handleJson(const char *t, JsonDocument &doc) {
  if (strcmp(t, "ready") == 0) {
    if (state == ST_WAIT_READY) beginMic();
    // 录音中收到 ready = 断线重连后网关新建了会话，继续推流即可
  } else if (strcmp(t, "p") == 0) {
    gPartial = String((const char *)(doc["x"] | ""));
    dirty();
  } else if (strcmp(t, "f") == 0) {
    String s = String((const char *)(doc["x"] | ""));
    commitText(s);
    gPartial = "";
    dirty();
  } else if (strcmp(t, "done") == 0) {
    // 收尾兜底：网关补发的定稿可能没赶上 / 最后半句还在 partial —— 先并入正文再清，
    // 否则那句灰字就这么凭空消失，手机上（会议记录）也查不到
    if (gPartial.length()) {
      commitText(gPartial);
      gPartial = "";
    }
    // 网关随 done 一起下发手机端链接；没拿到就退回旧的纯统计页
    gMeetingUrl = String((const char *)(doc["q"] | ""));
    if (gMeetingUrl.length()) Serial.printf("[QR] %s\n", gMeetingUrl.c_str());
    wsStopReconnect();
    gWantDisconnect = true;          // 真正的 disconnect 放到 loop 里做
    if (state == ST_PAUSING || state == ST_PAUSED)      state = ST_PAUSED;
    else if (state == ST_STOPPING)                      state = ST_SUMMARY;
    else                                                state = ST_HOME;
    dirty();
    Serial.println("[WS] done");
  } else if (strcmp(t, "err") == 0) {
    gErrMsg = String((const char *)(doc["x"] | "网关错误"));
    state = ST_ERROR;
    dirty();
  }
}

static void onWsEvent(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      // 只有用户真的发起会话时才该连上。空闲/结束页若冒出连接，是库的自动重连在补连，
      // 网关每接一条连接就开一个百炼 task（app.py 里 run-task），白烧额度 —— 立刻关掉。
      if (state == ST_HOME || state == ST_SUMMARY || state == ST_ERROR || state == ST_BOOT) {
        wsStopReconnect();
        gWantDisconnect = true;
        Serial.println("[WS] unexpected connected -> closing");
        break;
      }
      Serial.println("[WS] connected");
      if (state == ST_CONNECTING) {
        state = ST_WAIT_READY;
        gMarkAt = millis();
      }
      dirty();
      break;

    case WStype_DISCONNECTED:
      Serial.println("[WS] disconnected");
      if (state == ST_RECORDING) {
        endMic();
        gErrMsg = "与网关断开，请检查网络后重试";
        state = ST_ERROR;
        dirty();
      } else if (state == ST_CONNECTING || state == ST_WAIT_READY) {
        // 交给 loop 里的超时处理，库会自动重连
        dirty();
      }
      break;

    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, len) != DeserializationError::Ok) break;
      handleJson(doc["t"] | "", doc);
      break;
    }

    default:
      break;
  }
}

// ---------------- 绘制 ----------------
static void drawSubtitles(int top, int bottom) {
  canvas.setTextSize(1);
  std::vector<String> lines;

  // 加粗会多占 1px，折行宽度先扣掉，免得出界
  const int maxW = UI_SCREEN_W - UI_MARGIN * 2 - TERMS_BOLD_STEP;

  ui::wrapTerms(gCommitted, maxW, lines);
  int partialStart = (int)lines.size();
  ui::wrapTerms(gPartial, maxW, lines);

  if (lines.empty()) {
    ui::textCenter("请开始说话…", UI_SCREEN_W / 2, top + 36, ui::C_DIM);
    return;
  }

  int cap = (bottom - top) / UI_LINE_H;
  if (cap <= 0) return;
  int total = (int)lines.size();
  int start = total > cap ? total - cap : 0;

  for (int i = start; i < total; i++) {
    bool isPartial = (i >= partialStart);
    // 定稿句：词表命中 → 青绿加粗；未定稿句：只加粗、同灰，别抢定稿的视觉
    ui::textRich(lines[i], UI_MARGIN, top + (i - start) * UI_LINE_H,
                 isPartial ? ui::C_DIM : ui::C_TEXT,
                 isPartial ? ui::C_DIM : ui::C_TERM);
  }
}

static void drawHome() {
  ui::topBar("会议记录", ui::C_TEXT, false, gWifiOk, batteryPct());

  const int cx = 8, cy = 38, cw = 304, ch = 96;
  canvas.fillRoundRect(cx, cy, cw, ch, 12, ui::C_CARD);
  canvas.drawRoundRect(cx, cy, cw, ch, 12, ui::C_LINE);

  bool failed = wifiFailed();

  if (gSentCount > 0 || gCommitted.length() > 0) {
    ui::textLeft("上次会议", cx + 18, cy + 22, ui::C_DIM);
    ui::textLeft(fmtDur(gAccumMs).c_str(), cx + 18, cy + 54, ui::C_TEXT, 2.0f);
    char b[64];
    snprintf(b, sizeof(b), "%u 句 · %lu 字", gSentCount, (unsigned long)gCharCount);
    ui::textLeft(b, cx + 18, cy + 82, ui::C_CYAN);
  } else if (!settings::hasWiFi()) {
    ui::textCenter("还没配网", cx + cw / 2, cy + 34, ui::C_YELLOW, 2.0f);
    ui::textCenter("点下方按钮，手机连热点配置", cx + cw / 2, cy + 68, ui::C_DIM);
  } else if (failed) {
    ui::textCenter("WiFi 连不上", cx + cw / 2, cy + 34, ui::C_YELLOW, 2.0f);
    ui::textCenter("点下方按钮重新配网", cx + cw / 2, cy + 68, ui::C_DIM);
  } else if (!gWifiOk) {
    ui::textCenter("正在连接 WiFi…", cx + cw / 2, cy + 42, ui::C_TEXT, 1.4f);
    ui::textCenter(settings::get().wifiSsid.c_str(), cx + cw / 2, cy + 72, ui::C_DIM);
  } else {
    ui::textCenter("准备就绪", cx + cw / 2, cy + 34, ui::C_TEXT, 2.0f);
    ui::textCenter("点按下方按钮开始记录", cx + cw / 2, cy + 68, ui::C_DIM);
  }

  if (failed || !settings::hasWiFi()) {
    ui::button(R_MAIN, "去配网", ui::C_YELLOW, TFT_BLACK, isPressed(R_MAIN));
  } else {
    ui::button(R_MAIN, "开始录音", ui::C_GREEN, TFT_BLACK, isPressed(R_MAIN));
  }
  ui::buttonGhost(R_GEAR, "设置", ui::C_DIM, isPressed(R_GEAR));
}

static void drawLive(bool paused) {
  String t = fmtDur(recMs());
  if (paused) {
    ui::topBar((String("已暂停 ") + t).c_str(), ui::C_YELLOW, false, gWifiOk, batteryPct());
  } else {
    ui::topBar(t.c_str(), ui::C_TEXT, true, gWifiOk, batteryPct());
  }

  int bottom = paused ? (UI_BTN_Y - 26) : (UI_BTN_Y - 6);
  drawSubtitles(UI_BODY_TOP, bottom);

  if (paused) {
    int y = UI_BTN_Y - 22;
    canvas.fillRoundRect(8, y, 304, 18, 6, ui::darken(ui::C_YELLOW, 3));
    ui::textCenter("已暂停，字幕停止更新", UI_SCREEN_W / 2, y + 9, ui::C_YELLOW);
  }

  ui::button(R_LEFT, paused ? "继续" : "暂停",
             paused ? ui::C_GREEN : ui::C_YELLOW, TFT_BLACK, isPressed(R_LEFT));
  ui::button(R_RIGHT, "结束", ui::C_RED, TFT_WHITE, isPressed(R_RIGHT));
}

static void drawWorking(const char *label) {
  ui::topBar(label, ui::C_CYAN, false, gWifiOk, batteryPct());
  drawSubtitles(UI_BODY_TOP, UI_BTN_Y - 34);

  int y = UI_BTN_Y - 28;
  canvas.fillRoundRect(8, y, 304, 20, 7, ui::C_CARD);
  ui::textCenter("正在等待最后一句定稿…", UI_SCREEN_W / 2, y + 10, ui::C_DIM);
}

static void drawConnecting() {
  const char *label = (state == ST_CONNECTING) ? "连接网关" : "建立会话";
  ui::topBar(label, ui::C_CYAN, false, gWifiOk, batteryPct());

  drawSubtitles(UI_BODY_TOP, UI_BTN_Y - 34);

  int y = UI_BTN_Y - 28;
  canvas.fillRoundRect(8, y, 304, 20, 7, ui::C_CARD);
  ui::textCenter(gCommitted.length() ? "正在重建识别会话…" : "正在建立识别会话…",
                 UI_SCREEN_W / 2, y + 10, ui::C_DIM);

  ui::buttonGhost(R_MAIN, "取消", ui::C_DIM, isPressed(R_MAIN));
}

// ---------------- 二维码（结束页把手机端链接给出去） ----------------
// 用 ricmoo/QRCode 现算。版本按内容长度自动选 3~10，纠错等级 LOW：
// 屏幕上二维码是近距离扫的，不需要高纠错，等级越低同样面积下模块越大、越容易扫。
// 版本 10 是 57×57 模块，(57*57+7)/8 = 407 字节；取 512 留余量。
// 注意 qrcode_getBufferSize() 是函数不是宏，不能拿来做静态数组长度。
static uint8_t gQrBuf[512];

static bool drawQr(const String &text, int boxX, int boxY, int boxW, int boxH) {
  if (!text.length()) return false;
  QRCode qr;
  bool ok = false;
  for (int v = 3; v <= 10; v++) {
    if (qrcode_initText(&qr, gQrBuf, v, ECC_LOW, text.c_str()) == 0) { ok = true; break; }
  }
  if (!ok) {
    Serial.printf("[QR] 内容太长，编不出来（%u 字节）\n", (unsigned)text.length());
    return false;
  }
  // 静区留 4 个模块（二维码规范要求，少了扫不出来）
  int n = qr.size + 8;
  int scale = min(boxW, boxH) / n;
  if (scale < 1) scale = 1;
  int side = n * scale;
  int x0 = boxX + (boxW - side) / 2 + 4 * scale;
  int y0 = boxY + (boxH - side) / 2 + 4 * scale;

  canvas.fillRect(x0, y0, qr.size * scale, qr.size * scale, TFT_WHITE);
  for (int y = 0; y < qr.size; y++) {
    for (int x = 0; x < qr.size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        canvas.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, TFT_BLACK);
      }
    }
  }
  Serial.printf("[QR] v=%d size=%d scale=%d\n", qr.version, qr.size, scale);
  return true;
}

static void drawSummary() {
  ui::topBar("会议结束", ui::C_GREEN, false, gWifiOk, batteryPct());

  char b[64];
  snprintf(b, sizeof(b), "%u 句 · %lu 字", gSentCount, (unsigned long)gCharCount);

  if (gMeetingUrl.length()) {
    // 左：本场数据    右：二维码
    ui::textLeft("本次时长", 12, 52, ui::C_DIM);
    ui::textLeft(fmtDur(gAccumMs).c_str(), 12, 78, ui::C_TEXT, 2.0f);
    ui::textLeft(b, 12, 108, ui::C_CYAN);
    canvas.drawFastHLine(12, 126, 128, ui::C_LINE);
    ui::textLeft("手机扫码", 12, 146, ui::C_TEXT);
    ui::textLeft("纪要 · 文字", 12, 168, ui::C_DIM);

    const int bx = 158, by = 34, bs = 150;
    canvas.fillRoundRect(bx, by, bs, bs, 10, TFT_WHITE);
    if (!drawQr(gMeetingUrl, bx, by, bs, bs)) {
      canvas.fillRoundRect(bx, by, bs, bs, 10, ui::C_CARD);
      ui::textCenter("二维码生成失败", bx + bs / 2, by + bs / 2, ui::C_DIM);
    }
  } else {
    // 没拿到链接（比如没联网就开始录）：退回纯统计卡片
    const int cx = 8, cy = 36, cw = 304, ch = 142;
    canvas.fillRoundRect(cx, cy, cw, ch, 12, ui::C_CARD);
    canvas.drawRoundRect(cx, cy, cw, ch, 12, ui::C_LINE);
    ui::textLeft("本次时长", cx + 18, cy + 24, ui::C_DIM);
    ui::textLeft(fmtDur(gAccumMs).c_str(), cx + 18, cy + 56, ui::C_TEXT, 2.0f);
    ui::textLeft(b, cx + 18, cy + 90, ui::C_CYAN);
    canvas.drawFastHLine(cx + 18, cy + 110, cw - 36, ui::C_LINE);
    ui::textLeft("未连网关，本次记录未同步", cx + 18, cy + 126, ui::C_YELLOW);
  }

  ui::button(R_LEFT, "新建会议", ui::C_GREEN, TFT_BLACK, isPressed(R_LEFT));
  ui::buttonGhost(R_RIGHT, "重启设备", ui::C_DIM, isPressed(R_RIGHT));
}

static void drawSettings() {
  ui::topBar("设置", ui::C_TEXT, false, gWifiOk, batteryPct());
  settings::Config &c = settings::get();

  int y = 40;
  const int rowH = 23;
  const int labelX = 14;
  const int valueX = 60;

  ui::textLeft("WiFi", labelX, y, ui::C_DIM);
  ui::textLeft(c.wifiSsid.length() ? c.wifiSsid.c_str() : "(未配置)", valueX, y,
               c.wifiSsid.length() ? ui::C_TEXT : ui::C_YELLOW);
  y += rowH;

  ui::textLeft("IP", labelX, y, ui::C_DIM);
  ui::textLeft(gWifiOk ? WiFi.localIP().toString().c_str() : "-", valueX, y,
               gWifiOk ? ui::C_TEXT : ui::C_DIM);
  y += rowH;

  ui::textLeft("网关", labelX, y, ui::C_DIM);
  String gw = c.gwHost + ":" + String(c.gwPort);
  ui::textLeft(gw.c_str(), valueX, y, ui::C_TEXT);
  y += rowH;

  // ---- 舵机开关：整行可点 ----
  bool sOn = c.servoEnabled;
  const char *sub = !sOn ? "已关闭 · 录音优先"
                        : (motion::available() ? "录音时自动断电" : "已开 · 未检测到底座");
  uint16_t subColor = !sOn ? ui::C_DIM
                            : (motion::available() ? ui::C_TEXT : ui::C_YELLOW);
  ui::textLeft("舵机", labelX, y, ui::C_DIM);
  ui::textLeft(sub, valueX, y, subColor);

  int swW = 48, swH = 18;
  int swX = UI_SCREEN_W - swW - 12;
  int swY = y - swH / 2;
  canvas.fillRoundRect(swX, swY, swW, swH, 9, sOn ? ui::C_GREEN : ui::C_TRACK);
  canvas.drawRoundRect(swX, swY, swW, swH, 9, sOn ? ui::C_GREEN : ui::C_LINE);
  ui::textCenter(sOn ? "开" : "关", swX + swW / 2, swY + swH / 2, sOn ? TFT_BLACK : ui::C_DIM);
  y += rowH;

  // ---- 专有名词词表 ----
  ui::textLeft("词表", labelX, y, ui::C_DIM);
  char tb[56];
  snprintf(tb, sizeof(tb), "%d 条 · %s", terms::count(), terms::sourceName());
  ui::textLeft(tb, valueX, y, terms::count() ? ui::C_TEXT : ui::C_YELLOW);
  y += rowH;

  ui::textLeft("版本", labelX, y, ui::C_DIM);
  ui::textLeft("v" FW_VERSION, valueX, y, ui::C_TEXT);

  ui::textLeft("长按主界面 2.5 秒可直进配网", labelX, y + 22, ui::C_DIM);

  ui::button(R_B1, "重新配网", ui::C_CYAN, TFT_BLACK, isPressed(R_B1));
  ui::button(R_B2, "重启", ui::C_CARD, ui::C_TEXT, isPressed(R_B2));
  ui::buttonGhost(R_B3, "返回", ui::C_DIM, isPressed(R_B3));
}

static void drawProvision() {
  ui::topBar("配网模式", ui::C_CYAN, false, gWifiOk, batteryPct());

  String ssid = provision::name();

  ui::textLeft("热点名称", 14, 46, ui::C_DIM);
  ui::textLeft(ssid.c_str(), 14, 66, ui::C_TEXT, 1.6f);

  ui::textLeft("热点密码", 14, 92, ui::C_DIM);
  ui::textLeft(AP_PASS, 14, 112, ui::C_TEXT, 1.6f);

  ui::textLeft("浏览器打开", 14, 138, ui::C_DIM);
  ui::textLeft(AP_IP_STR, 14, 156, ui::C_CYAN, 1.5f);

#if UI_SHOW_QR
  String qr = String("WIFI:T:WPA;S:") + ssid + ";P:" + AP_PASS + ";;";
  canvas.fillRect(196, 40, 108, 108, TFT_WHITE);
  canvas.qrcode(qr.c_str(), 200, 44, 100);
#endif

  char b[48];
  snprintf(b, sizeof(b), "手机连热点后打开网页配置 (已连 %d)", provision::clients());
  ui::textCenter(b, UI_SCREEN_W / 2, 176, ui::C_DIM);

  ui::button(R_MAIN, "退出配网", ui::C_CARD, ui::C_TEXT, isPressed(R_MAIN));
}

static void drawError() {
  ui::topBar("出错了", ui::C_RED, false, gWifiOk, batteryPct());

  std::vector<String> lines;
  ui::wrap(gErrMsg.length() ? gErrMsg : "未知错误", UI_SCREEN_W - 24, lines);
  for (size_t i = 0; i < lines.size() && i < 6; i++)
    ui::text(lines[i].c_str(), 12, 48 + (int)i * 22, ui::C_TEXT);

  ui::button(R_LEFT, "重试", ui::C_GREEN, TFT_BLACK, isPressed(R_LEFT));
  ui::buttonGhost(R_RIGHT, "去设置", ui::C_DIM, isPressed(R_RIGHT));
}

static void drawBoot(const char *msg) {
  canvas.fillSprite(ui::C_BG);
  ui::textCenter("Chan 会议记录", UI_SCREEN_W / 2, 92, ui::C_TEXT, 1.8f);
  ui::textCenter("v" FW_VERSION, UI_SCREEN_W / 2, 122, ui::C_DIM);
  if (msg) ui::textCenter(msg, UI_SCREEN_W / 2, 164, ui::C_CYAN);
  canvas.pushSprite(0, 0);
}

static void draw() {
  static uint8_t n = 0;
  bool trace = (n < 5);
  if (trace) { n++; Serial.printf("[DR] draw#%u state=%d enter\n", n, (int)state); }
  canvas.setTextSize(1);
  canvas.setFont(F_BODY);
  canvas.setTextWrap(false);
  canvas.fillSprite(ui::C_BG);

  switch (state) {
    case ST_HOME:       drawHome(); break;
    case ST_RECORDING:  drawLive(false); break;
    case ST_PAUSED:     drawLive(true); break;
    case ST_PAUSING:    drawWorking("暂停收尾"); break;
    case ST_STOPPING:   drawWorking("结束收尾"); break;
    case ST_SUMMARY:    drawSummary(); break;
    case ST_SETTINGS:   drawSettings(); break;
    case ST_PROVISION:  drawProvision(); break;
    case ST_ERROR:      drawError(); break;
    case ST_CONNECTING:
    case ST_WAIT_READY: drawConnecting(); break;
    default: break;
  }
  canvas.pushSprite(0, 0);
  if (trace) Serial.printf("[DR] draw#%u pushed\n", n);
}

// ---------------- 输入 ----------------
static void handleTap(int16_t x, int16_t y) {
  switch (state) {
    case ST_HOME:
      if (R_MAIN.hit(x, y)) startSession();
      else if (R_GEAR.hit(x, y)) { state = ST_SETTINGS; dirty(); }
      break;

    case ST_RECORDING:
      if (R_LEFT.hit(x, y))       pauseSession();
      else if (R_RIGHT.hit(x, y)) stopSession();
      break;

    case ST_PAUSED:
      if (R_LEFT.hit(x, y))       resumeSession();
      else if (R_RIGHT.hit(x, y)) { state = ST_SUMMARY; dirty(); }
      break;

    case ST_CONNECTING:
    case ST_WAIT_READY:
      if (R_MAIN.hit(x, y)) {
        wsStopReconnect();
        ws.disconnect();
        state = ST_HOME;
        dirty();
      }
      break;

    case ST_SUMMARY:
      if (R_LEFT.hit(x, y))       newMeeting();
      else if (R_RIGHT.hit(x, y)) reboot();
      break;

    case ST_SETTINGS:
      if (R_SERVO.hit(x, y)) {
        // 舵机开关：立即生效并写 NVS，不用重启、更不用重烧
        bool on = !settings::get().servoEnabled;
        settings::setServoEnabled(on);
        if (on) motion::begin();
        else    motion::end();
        Serial.printf("[SET] servo=%d ready=%d\n", on ? 1 : 0, motion::available() ? 1 : 0);
        dirty();
      } else if (R_B1.hit(x, y)) {
        provision::begin();
        state = ST_PROVISION;
        dirty();
      } else if (R_B2.hit(x, y)) {
        reboot();
      } else if (R_B3.hit(x, y)) {
        state = ST_HOME;
        dirty();
      }
      break;

    case ST_PROVISION:
      if (R_MAIN.hit(x, y)) {
        provision::stop();
        if (settings::hasWiFi()) wifiBegin();
        state = ST_HOME;
        dirty();
      }
      break;

    case ST_ERROR:
      if (R_LEFT.hit(x, y))       { state = ST_HOME; dirty(); }
      else if (R_RIGHT.hit(x, y)) {
        provision::begin();
        state = ST_PROVISION;
        dirty();
      }
      break;

    default:
      break;
  }
}

// ---------------- 物理按键 ----------------
// 主操作键：按一下 = 开始录音 / 结束录音 / 新建会议（按当前界面决定）。
// M5Unified 0.2.26 的按键轮询表里没有 CoreS3 分支，M5.BtnA 收不到数据，
// 所以这里同时监听「M5Unified 按键」和「直接读 GPIO」两条路，谁先命中算谁。
// StackChan 底部按键可能挂在 AW9523（0x58）上，先只打印寄存器变化，实测确认后再接动作。
#define BTN_STR2(x) #x
#define BTN_STR(x)  BTN_STR2(x)
static uint32_t gKeyLastMs = 0;

static void mainKey(const char *src) {
  uint32_t now = millis();
  if (now - gKeyLastMs < BTN_REPEAT_MS) return;      // 防连击
  gKeyLastMs = now;
  Serial.printf("[BTN] %s  state=%d\n", src, (int)state);
  switch (state) {
    case ST_HOME:      startSession();           break;   // 主界面按一下 → 开录
    case ST_RECORDING: stopSession();            break;   // 录音中按一下 → 结束
    case ST_PAUSED:    resumeSession();          break;
    case ST_SUMMARY:   newMeeting();             break;
    case ST_ERROR:     state = ST_HOME; dirty(); break;
    default: break;                                       // 连接中/暂停中/OTA 不响应
  }
}

static void pollButtons() {
  static uint32_t lastPoll = 0;
  static bool prevA = false, prevB = false, prevC = false;
#if BTN_SCAN_EXPANDER
  static uint8_t prevP0 = 0xFF, prevP1 = 0xFF;
#endif
  if (millis() - lastPoll < 20) return;
  lastPoll = millis();

  bool a = M5.BtnA.isPressed();
  if (a && !prevA) mainKey("BtnA");
  prevA = a;

  bool b = M5.BtnB.isPressed();
  if (b && !prevB) mainKey("BtnB");
  prevB = b;

  bool c = M5.BtnC.isPressed();
  if (c && !prevC) mainKey("BtnC");
  prevC = c;

  // 电源键（AXP2101 PWRKEY）：M5.update() 已把 PEK 中断映射成 BtnPWR 的 click/hold 事件，
  // 这里的 isPressed 只在事件同一帧有效，所以用 wasClicked / wasPressed 取边沿。
  if (M5.BtnPWR.wasClicked() || M5.BtnPWR.wasPressed()) mainKey("BtnPWR");

#if BTN_MAIN_GPIO >= 0
  static bool prevG = false;
  bool g = (digitalRead(BTN_MAIN_GPIO) == LOW);
  if (g && !prevG) mainKey("GPIO" BTN_STR(BTN_MAIN_GPIO));
  prevG = g;
#endif

#if BTN_SCAN_EXPANDER
  // 只观测不动作：P1_2 = 触摸 INT、P0_2 = 功放使能，都不是按键。
  uint8_t p0 = M5.In_I2C.readRegister8(0x58, 0x00, 400000);
  uint8_t p1 = M5.In_I2C.readRegister8(0x58, 0x01, 400000);
  if (p0 != prevP0 || p1 != prevP1) {
    if (prevP0 != 0xFF || prevP1 != 0xFF) {
      Serial.printf("[BTN] AW9523 P1P0 %02X%02X -> %02X%02X\n", prevP1, prevP0, p1, p0);
    }
    prevP0 = p0;
    prevP1 = p1;
  }
#endif

#if BTN_TRACE
  static uint32_t tLast = 0;
  if (millis() - tLast > 3000) {
    tLast = millis();
    Serial.printf("[BTNRAW] A=%d B=%d C=%d PWRclick=%d PWRpress=%d\n",
                  (int)a, (int)b, (int)c,
                  (int)M5.BtnPWR.wasClicked(), (int)M5.BtnPWR.isPressed());
  }
#endif
}

static void pollInput() {
  if (!M5.Touch.isEnabled()) return;
  auto td = M5.Touch.getDetail();

  if (td.wasClicked()) {
    bool swallow = gPressFired;     // 长按已处理过，松手这一下吞掉
    gPressStart = 0;
    gPressFired = false;
    if (!swallow) handleTap(td.x, td.y);
    return;
  }

  if (td.isPressed()) {
    if (gPressStart == 0) {
      gPressStart = millis();
      gPressFired = false;
    }
    // 主界面长按任意位置 → 配网（不用先点设置再点配网）
    if (!gPressFired && state == ST_HOME && millis() - gPressStart > LONG_PRESS_MS) {
      gPressFired = true;
      Serial.println("[UI] long press -> provision");
      provision::begin();
      state = ST_PROVISION;
      dirty();
    }
    dirty();   // 让按钮按下有高亮反馈
  } else {
    gPressStart = 0;
    gPressFired = false;
  }
}

// ---------------- setup / loop ----------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  delay(150);
  Serial.printf("\n=== Chan MeetingRecorder v%s ===\n", FW_VERSION);

  // CoreS3 是 320x240 横屏，M5GFX 面板定义里默认 _rotation = 1（offset_rotation = 3）。
  // 写 0 = 竖屏 240x320，320 宽的画布会被转 90° 显示，触摸坐标也跟着错位。
  M5.Display.setRotation(1);
  M5.Display.setBrightness(80);
  M5.Display.fillScreen(TFT_BLACK);
  Serial.printf("[LCD] %dx%d rot=%u board=%d\n", (int)M5.Display.width(), (int)M5.Display.height(),
                (unsigned)M5.Display.getRotation(), (int)M5.Display.getBoard());

#if DISPLAY_SELFTEST
  // 逐帧刷纯色：屏幕只要"动"过一次，就说明 SPI/面板通路是通的
  struct { uint16_t c; const char *n; } seq[] = {
    {TFT_RED, "RED"}, {TFT_GREEN, "GREEN"}, {TFT_BLUE, "BLUE"}, {TFT_MAGENTA, "MAGENTA"},
  };
  for (int i = 0; i < 4; i++) {
    Serial.printf("[LCD] fill %s\n", seq[i].n);
    M5.Display.fillScreen(seq[i].c);
    M5.Display.setTextColor(TFT_WHITE, seq[i].c);
    M5.Display.setFont(F_BODY);
    M5.Display.drawString(seq[i].n, 12, 12);
    delay(1200);
  }
  Serial.println("[LCD] direct text");
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString("直绘 OK", 12, 40);
  delay(1200);
  Serial.println("[LCD] sprite push");
  M5Canvas probe(&M5.Display);
  probe.setColorDepth(16);
  if (probe.createSprite(UI_SCREEN_W, UI_SCREEN_H)) {
    probe.fillSprite(TFT_NAVY);
    probe.setTextColor(TFT_YELLOW, TFT_NAVY);
    probe.setFont(F_BODY);
    probe.drawString("精灵 OK", 12, 40);
    probe.pushSprite(0, 0);
    Serial.println("[LCD] sprite pushed");
  } else {
    Serial.println("[LCD] sprite createSprite FAILED");
  }
  delay(1200);
  probe.deleteSprite();
  M5.Display.fillScreen(TFT_BLACK);
#endif

#if BTN_MAIN_GPIO >= 0
  pinMode(BTN_MAIN_GPIO, INPUT_PULLUP);   // 仅在明确指定 GPIO 时才配；GPIO0 是麦克风 MCK，碰不得
#endif

  canvas.setColorDepth(16);
  canvas.createSprite(UI_SCREEN_W, UI_SCREEN_H);
  canvas.setFont(F_BODY);
  canvas.setTextWrap(false);
  ui::attach(&canvas);

  settings::load();
  settings::Config &c = settings::get();

  Serial.println("[TR] step terms");
  drawBoot("载入词表…");
  terms::load();

  Serial.println("[TR] step motion");
  drawBoot("初始化舵机…");
  motion::begin();      // 开关关着 / 没底座 → 直接返回 false，不影响录音

  Serial.println("[TR] step boot3");
  drawBoot("正在启动…");

  // WS 回调只需注册一次
  ws.onEvent(onWsEvent);
  WiFi.onEvent(onWiFiEvent);

  if (!settings::hasWiFi()) {
    Serial.println("[BOOT] no wifi config -> provision");
    provision::begin();
    state = ST_PROVISION;
  } else {
    Serial.printf("[BOOT] wifi=%s gateway=%s:%u\n",
                  c.wifiSsid.c_str(), c.gwHost.c_str(), c.gwPort);
    wifiBegin();
    state = ST_HOME;
  }
  dirty();
  Serial.println("[TR] first draw start");
  draw();
  Serial.println("[TR] first draw done");
}

void loop() {
  M5.update();

  // 心跳：确认 loop 真的在跑、卡在哪一步（排障用，稳定后可删）
  {
    static uint32_t hb = 0;
    static uint32_t n = 0;
    if (millis() - hb > 2000) {
      hb = millis();
      Serial.printf("[LP] #%lu state=%d dirty=%d wifi=%d heap=%u\n",
                    (unsigned long)++n, (int)state, (int)gDirty,
                    (int)WiFi.status(), (unsigned)ESP.getFreeHeap());
    }
  }

  // ---------- 配网模式：只跑 web server ----------
  if (state == ST_PROVISION) {
    provision::loop();
    if (provision::exitRequested()) {
      provision::stop();
      if (settings::hasWiFi()) wifiBegin();
      state = ST_HOME;
      dirty();
    }
    static int lastClients = -1;
    int cn = provision::clients();
    if (cn != lastClients) { lastClients = cn; dirty(); }

    // 之前这里提前 return，触摸永远到不了 handleTap，「退出配网」按钮形同虚设
    pollInput();

    if (gDirty && millis() - gLastDraw > 200) {
      draw();
      gDirty = false;
      gLastDraw = millis();
    }
    delay(2);
    return;
  }

  ws.loop();

  if (gWantDisconnect) {
    gWantDisconnect = false;
    ws.disconnect();
  }

  // ---------- WiFi 状态跟踪 ----------
  bool ok = (WiFi.status() == WL_CONNECTED);
  if (ok != gWifiOk) {
    gWifiOk = ok;
    dirty();
    if (ok) Serial.printf("[WiFi] connected %s\n", WiFi.localIP().toString().c_str());
  }

  // ---------- WiFi 断线自愈 ----------
  // 连不上不躺平等用户：每 WIFI_RETRY_MS 重试一次直到连上。
  // 路由器抽风/信号波动时 10 秒内自己恢复，不再"卡在开机动画看运气"。
  if (gWifiStarted && !gWifiOk && millis() - gWifiTryAt > WIFI_RETRY_MS) {
    settings::Config &c = settings::get();
    Serial.printf("[WiFi] retry ssid=%s\n", c.wifiSsid.c_str());
    WiFi.disconnect(false);   // 清掉上一次半死不活的连接状态，不擦 NVS
    WiFi.begin(c.wifiSsid.c_str(), c.wifiPass.c_str());
    gWifiTryAt = millis();
  }

  // ---------- OTA：连上 WiFi 后开一个无线烧录口 ----------
  if (ok) {
    otaEnsure();
    if (gOtaStarted) ArduinoOTA.handle();
  }

  // ---------- 录音泵：一帧 50ms ----------
  if (state == ST_RECORDING && !gOtaActive) {
    if (M5.Mic.record(micBuf, FRAME_SAMPLES, SAMPLE_RATE)) {
#if AUD_STATS
      audStats(micBuf, FRAME_SAMPLES);   // 只统计不干预，判削波/增益用
#endif
#if VAD_ENABLE
      // 静音帧直接丢掉（不上传、不占带宽、不烧 ASR 额度），人声前 400ms 补发回来
      vad::feed(micBuf, FRAME_SAMPLES, pcmSink, millis());
#else
      pcmSink(micBuf, FRAME_SAMPLES);
#endif
    }
  }

  // ---------- 超时兜底 ----------
  switch (state) {
    case ST_CONNECTING:
      if (millis() - gMarkAt > WS_CONNECT_MS) {
        gErrMsg = "连不上网关，请检查网络或网关设置";
        state = ST_ERROR;
        wsStopReconnect();
        ws.disconnect();
        dirty();
      }
      break;

    case ST_WAIT_READY:
      if (millis() - gMarkAt > WS_CONNECT_MS) {
        gErrMsg = "会话建立超时，稍后重试";
        state = ST_ERROR;
        wsStopReconnect();
        ws.disconnect();
        dirty();
      }
      break;

    case ST_PAUSING:
      if (millis() - gMarkAt > STOP_TIMEOUT_MS) {
        // 等不到 done 就超时了：把还挂在 partial 的半句并进正文，别直接清掉
        commitText(gPartial);
        state = ST_PAUSED;
        gPartial = "";
        dirty();
      }
      break;

    case ST_STOPPING:
      if (millis() - gMarkAt > STOP_TIMEOUT_MS) {
        commitText(gPartial);
        state = ST_SUMMARY;
        gPartial = "";
        dirty();
      }
      break;

    default:
      break;
  }

  // ---------- 舵机 ----------
  // 录音期间保持断电（lock），只有在待机/结束页才恢复
  if (state == ST_HOME || state == ST_SUMMARY) motion::unlock();
  motion::update();

  // ---------- 输入 & 重绘 ----------
  pollButtons();
  pollInput();

  if (millis() - gTick > 500) {
    gTick = millis();
    if (state == ST_RECORDING || state == ST_PAUSED) dirty();   // 计时刷新
  }
  if (gDirty && millis() - gLastDraw > 120) {
    draw();
    gDirty = false;
    gLastDraw = millis();
  }

  delay(2);
}
