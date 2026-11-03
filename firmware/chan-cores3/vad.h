#pragma once
// ============================================================
//  本地 VAD（语音活动检测）· 只决定"这一帧要不要上传"
//
//  设计原则：
//    1. 绝不改变会话状态 —— 静音只是不上传，不断流、不发 stop
//    2. 绝不吞字 —— 三个安全网：
//         a) 人声起始补发预滚缓冲（默认 400ms）
//         b) 录音开头 1s 预热期全部上传，同时校准环境噪声底
//         c) 连续抑制超过 8s 视为"噪声底估错了"，自动下调 6dB 并放行
//    3. 阈值跟着环境噪声走 —— 自适应噪声底，不是写死的 dBFS
//       会议室空调、笔记本风扇大小差别很大，固定阈值必然误判
//
//  为什么需要预热（这是实测踩出来的）：
//    噪声底初值若写死 -58dBFS，遇到 -45dBFS 的稳态底噪，第一帧就被判成人声；
//    而"说话态"里噪声底是冻结的，于是永远醒不过来，VAD 退化成全量上传。
//    预热期先听 1 秒，取最低电平当噪声底，稳态噪声就正常了。
//    万一用户开机就说话（预热期全是人声），靠安全网 c) 在 8~16s 内自动恢复。
//
//  算力：每帧一次 RMS + 一次 log（50ms 一帧），ESP32-S3 可忽略
//  内存：VAD_PREROLL × FRAME_SAMPLES × 2 字节 = 8 × 800 × 2 = 12.8KB（静态）
//
//  验证：scripts/vad_sim.py 是本文件逻辑的 Python 镜像，改阈值后先跑它
//  调试：VAD_DEBUG=1 打印人声起止与噪声底，现场调阈值时开
// ============================================================

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "config.h"

namespace vad {

// 送帧回调：需要上传的帧通过它送出
typedef void (*Sink)(const int16_t *pcm, size_t samples);

struct Stats {
  float    noiseDbfs;    // 当前估计的环境噪声底
  float    lastDbfs;     // 上一帧电平
  float    warmMin;      // 预热期最低电平
  bool     speaking;     // 当前是否判定为人声
  bool     warmed;       // 预热是否完成
  uint16_t warmCount;    // 预热已收帧数
  uint32_t lastVoiceMs;  // 最后一次"有声"的时间戳
  uint32_t suppressSince; // 本轮连续抑制的起点（0 = 未抑制）
  uint32_t framesIn;     // 收到帧数
  uint32_t framesOut;    // 上传帧数
  uint8_t  ringPos;      // 预滚环形缓冲写指针
  uint8_t  ringFill;     // 预滚缓冲已有帧数
};

static Stats    s;
static int16_t  s_ring[VAD_PREROLL][FRAME_SAMPLES];

static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// ---- 单帧电平（dBFS，满量程 0dB，越小越安静）----
static inline float frameDbfs(const int16_t *pcm, size_t n) {
  if (!n) return -96.0f;
  uint64_t acc = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t v = pcm[i];
    acc += (uint64_t)(v * v);
  }
  float rms = sqrtf((float)acc / (float)n);
  if (rms < 1.0f) return -96.0f;
  return 20.0f * log10f(rms / 32768.0f);
}

// ---- 自适应噪声底：安静时快降、变吵时慢升；说话期间冻结 ----
static inline void updateNoise(float lvl) {
  float k = (lvl < s.noiseDbfs) ? 0.25f : 0.002f;
  s.noiseDbfs = clampf(s.noiseDbfs + (lvl - s.noiseDbfs) * k,
                       VAD_NOISE_MIN_DB, VAD_NOISE_MAX_DB);
}

static inline void reset() {
  s.noiseDbfs     = -58.0f;
  s.lastDbfs      = -96.0f;
  s.warmMin       = 0.0f;
  s.speaking      = false;
  s.warmed        = (VAD_WARMUP_MS <= 0);
  s.warmCount     = 0;
  s.lastVoiceMs   = 0;
  s.suppressSince = 0;
  s.framesIn      = 0;
  s.framesOut     = 0;
  s.ringPos       = 0;
  s.ringFill      = 0;
}

static inline void pushRing(const int16_t *pcm, size_t n) {
  memcpy(s_ring[s.ringPos], pcm, n * sizeof(int16_t));
  s.ringPos = (uint8_t)((s.ringPos + 1) % VAD_PREROLL);
  if (s.ringFill < VAD_PREROLL) s.ringFill++;
}

// 把预滚缓冲按时间顺序全部送出
static inline void flushRing(Sink sink, size_t n) {
  uint8_t start = (uint8_t)((s.ringPos + VAD_PREROLL - s.ringFill) % VAD_PREROLL);
  for (uint8_t i = 0; i < s.ringFill; i++) {
    sink(s_ring[(start + i) % VAD_PREROLL], n);
    s.framesOut++;
  }
  s.ringFill = 0;
}

static inline void beginSpeech(const int16_t *pcm, size_t n, Sink sink,
                               uint32_t nowMs, float lvl, float th, const char *why) {
  s.speaking      = true;
  s.lastVoiceMs   = nowMs;
  s.suppressSince = 0;
  flushRing(sink, n);      // 先把句首那 400ms 捞回来
  sink(pcm, n);            // 再送当前帧
  s.framesOut++;
#if VAD_DEBUG
  Serial.printf("[VAD] ↑ 起(%s) lvl=%.1f nf=%.1f th=%.1f pre=%u\n",
                why, lvl, s.noiseDbfs, th, (unsigned)s.ringFill);
#endif
}

// ---- 主入口：喂一帧，需要上传的帧会被 sink 立刻送出 ----
static inline void feed(const int16_t *pcm, size_t n, Sink sink, uint32_t nowMs) {
  s.framesIn++;
  float lvl = frameDbfs(pcm, n);
  s.lastDbfs = lvl;

  // ---------- 预热期：全量上传 + 校准噪声底 ----------
  if (!s.warmed) {
    if (s.warmCount == 0 || lvl < s.warmMin) s.warmMin = lvl;
    if (++s.warmCount * (uint32_t)FRAME_MS >= VAD_WARMUP_MS) {
      s.warmed    = true;
      s.noiseDbfs = clampf(s.warmMin, VAD_NOISE_MIN_DB, VAD_NOISE_MAX_DB);
#if VAD_DEBUG
      Serial.printf("[VAD] 预热完成 %u帧 nf=%.1f min=%.1f\n",
                    (unsigned)s.warmCount, s.noiseDbfs, s.warmMin);
#endif
    }
    sink(pcm, n);
    s.framesOut++;
    return;
  }

  // 门限 = 噪声底 + 余量，且不低于绝对下限
  float onTh  = s.noiseDbfs + VAD_ON_MARGIN_DB;
  float offTh = s.noiseDbfs + VAD_OFF_MARGIN_DB;
  if (onTh  < VAD_ABS_MIN_DB)       onTh  = VAD_ABS_MIN_DB;
  if (offTh < VAD_ABS_MIN_DB - 6.f) offTh = VAD_ABS_MIN_DB - 6.f;

  // ---------- 静音态 ----------
  if (!s.speaking) {
    if (lvl > onTh) {
      beginSpeech(pcm, n, sink, nowMs, lvl, onTh, "阈值");
      return;
    }
    // 抑制过久 → 说明噪声底估高了，强行下调 6dB 并放行
    if (s.suppressSince == 0) s.suppressSince = nowMs;
    if (nowMs - s.suppressSince >= VAD_MAX_SUPPRESS_MS) {
      s.noiseDbfs = clampf(s.noiseDbfs - 6.0f, VAD_NOISE_MIN_DB, VAD_NOISE_MAX_DB);
#if VAD_DEBUG
      Serial.printf("[VAD] ! 抑制超时，噪声底下调到 %.1f 并放行\n", s.noiseDbfs);
#endif
      beginSpeech(pcm, n, sink, nowMs, lvl, onTh, "超时放行");
      return;
    }
    pushRing(pcm, n);
    updateNoise(lvl);
    return;
  }

  // ---------- 人声态：hangover 之内都上传，盖住句子中间的换气停顿 ----------
  if (lvl > offTh) s.lastVoiceMs = nowMs;

  if (nowMs - s.lastVoiceMs <= VAD_HOLD_MS) {
    sink(pcm, n);
    s.framesOut++;
    return;
  }

  // 静音超过 hold：停发，当前帧转入预滚缓冲
  s.speaking      = false;
  s.suppressSince = nowMs;
  s.ringPos       = 0;
  s.ringFill      = 0;
  pushRing(pcm, n);
  updateNoise(lvl);
#if VAD_DEBUG
  Serial.printf("[VAD] ↓ 止 lvl=%.1f nf=%.1f th=%.1f\n", lvl, s.noiseDbfs, offTh);
#endif
}

static inline bool     speaking()   { return s.speaking; }
static inline bool     warmed()     { return s.warmed; }
static inline float    noiseDbfs()  { return s.noiseDbfs; }
static inline float    levelDbfs()  { return s.lastDbfs; }
static inline uint32_t framesIn()   { return s.framesIn; }
static inline uint32_t framesOut()  { return s.framesOut; }

// 省下的上行比例（0~1），用于结束页展示"静音裁剪省了 xx%"
static inline float savedRatio() {
  if (!s.framesIn) return 0.0f;
  return 1.0f - (float)s.framesOut / (float)s.framesIn;
}

}  // namespace vad
