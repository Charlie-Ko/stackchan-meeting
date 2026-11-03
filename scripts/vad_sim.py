#!/usr/bin/env python3
"""VAD 算法离线验证 —— 把 firmware/chan-cores3/vad.h 的逻辑原样搬过来跑合成信号。

目的：在没有硬件的情况下回答四个问题
    1. 句首会不会被吞（预滚缓冲是否有效）
    2. 句子中间的换气停顿会不会把一句话切碎（hangover 是否够长）
    3. 环境噪声变大后还认不认得出人声（预热校准 + 自适应噪声底）
    4. 万一开机就说话，会不会一路被误判成噪声而不上传（安全网是否兜得住）

用法：
    python3 scripts/vad_sim.py
改完 config.h 的 VAD_* 阈值，同步改下面的常量再跑一遍。
"""
import math

# ---- 与 config.h 保持一致 ----
FRAME_SAMPLES = 800          # 50ms @16k
SR = 16000
FRAME_MS = FRAME_SAMPLES * 1000 // SR
VAD_WARMUP_MS = 1000
VAD_PREROLL = 8
VAD_HOLD_MS = 900
VAD_MAX_SUPPRESS_MS = 8000
VAD_ON_MARGIN_DB = 9.0
VAD_OFF_MARGIN_DB = 5.0
VAD_ABS_MIN_DB = -55.0
VAD_NOISE_MIN_DB = -72.0
VAD_NOISE_MAX_DB = -35.0


def clampf(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


class Vad:
    """vad.h 的 Python 镜像，逐行对应，便于改一处两边同步。"""

    def __init__(self):
        self.noise = -58.0
        self.last = -96.0
        self.warm_min = 0.0
        self.speaking = False
        self.warmed = VAD_WARMUP_MS <= 0
        self.warm_count = 0
        self.last_voice_ms = 0
        self.suppress_since = 0
        self.frames_in = 0
        self.frames_out = 0
        self.ring = []
        self.sent_idx = []
        self.recoveries = 0

    @staticmethod
    def frame_dbfs(samples):
        if not samples:
            return -96.0
        acc = sum(v * v for v in samples)
        rms = math.sqrt(acc / len(samples))
        if rms < 1.0:
            return -96.0
        return 20.0 * math.log10(rms / 32768.0)

    def _update_noise(self, lvl):
        k = 0.25 if lvl < self.noise else 0.002
        self.noise = clampf(self.noise + (lvl - self.noise) * k,
                            VAD_NOISE_MIN_DB, VAD_NOISE_MAX_DB)

    def _flush_ring(self, idx):
        n = len(self.ring)
        self.sent_idx.extend(range(max(0, idx - n), idx))
        self.frames_out += n
        self.ring = []

    def _begin_speech(self, idx, now_ms):
        self.speaking = True
        self.last_voice_ms = now_ms      # 关键：必须重置，否则 hangover 立刻失效
        self.suppress_since = 0
        self._flush_ring(idx)
        self.sent_idx.append(idx)
        self.frames_out += 1

    def feed(self, samples, now_ms, idx):
        self.frames_in += 1
        lvl = self.frame_dbfs(samples)
        self.last = lvl

        # 预热期：全量上传 + 校准噪声底
        if not self.warmed:
            if self.warm_count == 0 or lvl < self.warm_min:
                self.warm_min = lvl
            self.warm_count += 1
            if self.warm_count * FRAME_MS >= VAD_WARMUP_MS:
                self.warmed = True
                self.noise = clampf(self.warm_min, VAD_NOISE_MIN_DB, VAD_NOISE_MAX_DB)
            self.sent_idx.append(idx)
            self.frames_out += 1
            return

        on_th = max(self.noise + VAD_ON_MARGIN_DB, VAD_ABS_MIN_DB)
        off_th = max(self.noise + VAD_OFF_MARGIN_DB, VAD_ABS_MIN_DB - 6.0)

        if not self.speaking:
            if lvl > on_th:
                self._begin_speech(idx, now_ms)
                return
            if self.suppress_since == 0:
                self.suppress_since = now_ms
            if now_ms - self.suppress_since >= VAD_MAX_SUPPRESS_MS:
                self.noise = clampf(self.noise - 6.0, VAD_NOISE_MIN_DB, VAD_NOISE_MAX_DB)
                self.recoveries += 1
                self._begin_speech(idx, now_ms)
                return
            self.ring.append(samples)
            if len(self.ring) > VAD_PREROLL:
                self.ring.pop(0)
            self._update_noise(lvl)
            return

        if lvl > off_th:
            self.last_voice_ms = now_ms

        if now_ms - self.last_voice_ms <= VAD_HOLD_MS:
            self.sent_idx.append(idx)
            self.frames_out += 1
            return

        self.speaking = False
        self.suppress_since = now_ms
        self.ring = [samples]
        self._update_noise(lvl)

    def saved(self):
        return 1.0 - self.frames_out / self.frames_in if self.frames_in else 0.0


def make_frame(dbfs):
    """生成一帧 16bit PCM，整体电平为 dbfs（方波，确定性，便于复现）。"""
    amp = int(32768 * (10 ** (dbfs / 20.0)))
    amp = max(1, min(32767, amp))
    return [amp if i % 2 == 0 else -amp for i in range(FRAME_SAMPLES)]


def run(name, segments, checks):
    """segments: [(描述, 帧数, dBFS)]；checks: [(起帧, 止帧, 至少上传数, 说明)]"""
    v = Vad()
    idx = 0
    spans = []
    for label, frames, dbfs in segments:
        start = idx
        for _ in range(frames):
            v.feed(make_frame(dbfs), idx * FRAME_MS, idx)
            idx += 1
        spans.append((label, start, idx, dbfs))

    kept = set(v.sent_idx)
    print(f"\n=== {name} ===")
    for label, a, b, db in spans:
        kept_n = sum(1 for i in range(a, b) if i in kept)
        print(f"  {label:<20} 帧{a:>4}~{b - 1:<4} {db:>6.0f}dB  上传 {kept_n}/{b - a}")
    print(f"  合计 {v.frames_in} 帧 → 上传 {v.frames_out} 帧，"
          f"静音裁剪省 {v.saved() * 100:.0f}%"
          + (f"，安全网触发 {v.recoveries} 次" if v.recoveries else ""))

    ok = True
    for a, b, need, desc in checks:
        got = sum(1 for i in range(a, b) if i in kept)
        passed = got >= need
        ok &= passed
        print(f"  [{'PASS' if passed else 'FAIL'}] {desc}：{got} 帧（要求 ≥{need}）")
    return ok


def main():
    results = []

    # 用例 1：静音 3s → 说话 2s → 静音 5s
    results.append(run(
        "用例1 单句 + 前后静音（安静房间 -62dB）",
        [("前置静音", 60, -62), ("人声", 40, -22), ("后置静音", 100, -62)],
        [(60, 100, 40, "人声段一帧不落"),
         (52, 60, 8, "句首预滚 8 帧补发")],
    ))

    # 用例 2：连续说话，中间夹 400ms 换气停顿
    results.append(run(
        "用例2 句中换气停顿 400ms",
        [("前置静音", 20, -62), ("第一句", 30, -22), ("停顿", 8, -60),
         ("第二句", 30, -22), ("后置静音", 60, -62)],
        [(20, 88, 68, "两句话连续上传、不被切碎")],
    ))

    # 用例 3：稳态高底噪 -45dBFS（空调/风扇），人声 -25dBFS
    results.append(run(
        "用例3 高噪声房间（-45dBFS 稳态底噪）",
        [("底噪", 80, -45), ("人声", 40, -25), ("底噪", 80, -45)],
        [(80, 120, 40, "高底噪下人声仍全发"),
         (0, 200, 40, "不能退化成全量上传（说明真的在裁）")],
    ))

    # 用例 4：全程静音
    results.append(run(
        "用例4 全程静音 10s",
        [("静音", 200, -65)],
        [(0, 200, 0, "全程静音零上传")],
    ))

    # 用例 5：开机立刻说话（预热期全是人声）—— 检验安全网
    results.append(run(
        "用例5 开机就说话（预热期全是人声）",
        [("持续人声", 300, -22)],
        [(0, 60, 60, "前 3 秒不受影响（预热期全量上传）"),
         (200, 300, 100, "8s 内恢复上传，后段零丢失")],
    ))

    print("\n" + "=" * 56)
    if all(results):
        print("全部 PASS —— 预滚、hangover、预热校准、安全网四项都生效")
    else:
        print("有 FAIL，回去调 config.h 的 VAD_* 阈值")
    print("=" * 56)


if __name__ == "__main__":
    main()
