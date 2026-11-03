#!/usr/bin/env python3
"""百炼 ASR 连通性验证脚本（实时 WebSocket）

用法：
    python scripts/dashscope_asr_verify.py /tmp/asr_test.wav
    python scripts/dashscope_asr_verify.py /tmp/asr_test.wav --model fun-asr-realtime

验证目标：
    1. API Key 能否完成 WS 握手（鉴权）
    2. run-task 能否启动（模型是否可用）
    3. 推流后能否返回识别文字（端到端闭环）

说明：脚本只读 .env，不打印 Key。
"""
import argparse
import json
import os
import sys
import time
import uuid
import wave

import websocket

WS_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
FRAME_MS = 100          # 每帧 100ms
SAMPLE_RATE = 16000


def load_env(path=".env"):
    d = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            d[k.strip()] = v.strip().strip('"').strip("'")
    return d


def read_pcm(path):
    """读 WAV，返回 (pcm_bytes, sample_rate, channels, sampwidth)。"""
    with wave.open(path, "rb") as w:
        ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        return w.readframes(n), sr, ch, sw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--model", default="paraformer-realtime-v2")
    ap.add_argument("--key", default=None, help="不传则读 .env 的 DASHSCOPE_API_KEY")
    args = ap.parse_args()

    key = args.key or load_env().get("DASHSCOPE_API_KEY", "")
    if not key:
        sys.exit("缺少 DASHSCOPE_API_KEY")

    pcm, sr, ch, sw = read_pcm(args.wav)
    print(f"[i] 音频 {len(pcm)} 字节 / {sr}Hz / {ch}ch / {sw*8}bit / "
          f"{len(pcm)/(sr*ch*sw):.2f}s")
    if sr != SAMPLE_RATE or ch != 1 or sw != 2:
        sys.exit(f"格式不符：需 {SAMPLE_RATE}Hz/1ch/16bit，当前 {sr}/{ch}/{sw*8}")

    task_id = uuid.uuid4().hex
    ws = websocket.create_connection(
        WS_URL,
        header=[f"Authorization: bearer {key}", "X-DashScope-DataInspection: enable"],
        timeout=20,
    )
    print("[✓] WS 握手成功（鉴权通过）")

    run_task = {
        "header": {"action": "run-task", "task_id": task_id, "streaming": "duplex"},
        "payload": {
            "task_group": "audio",
            "task": "asr",
            "function": "recognition",
            "model": args.model,
            "parameters": {"format": "pcm", "sample_rate": SAMPLE_RATE,
                           "language_hints": ["zh"]},
            "input": {},
        },
    }
    ws.send(json.dumps(run_task))

    # 等 task-started
    started = False
    t0 = time.time()
    while time.time() - t0 < 15:
        msg = ws.recv()
        if isinstance(msg, bytes):
            continue
        ev = json.loads(msg)
        name = ev.get("header", {}).get("event")
        if name == "task-started":
            started = True
            print("[✓] task-started（模型可用）")
            break
        if name == "task-failed":
            print("[✗] task-failed:", json.dumps(ev, ensure_ascii=False)[:600])
            ws.close()
            sys.exit(1)
    if not started:
        sys.exit("[✗] 等不到 task-started，超时")

    # 按 100ms 分帧推流
    step = int(SAMPLE_RATE * 2 * FRAME_MS / 1000)   # 3200 字节
    sent = 0
    for i in range(0, len(pcm), step):
        ws.send_binary(pcm[i:i + step])
        sent += 1
        time.sleep(FRAME_MS / 1000 * 0.5)           # 半速推，稳妥
    print(f"[i] 已推送 {sent} 帧")

    ws.send(json.dumps({
        "header": {"action": "finish-task", "task_id": task_id, "streaming": "duplex"},
        "payload": {"input": {}},
    }))

    # 结果去重：同一 sentence_id 的 text 是"累积式"的（每次回传整句当前全量），
    # 直接拼接会变成复读机。按 sentence_id 覆盖，sentence_end=true 视为定稿。
    segs = {}       # sentence_id -> text（定稿）
    live = ""       # 当前未定稿句子的实时文本
    live_id = None
    t0 = time.time()
    while time.time() - t0 < 25:
        try:
            msg = ws.recv()
        except Exception:
            break
        if isinstance(msg, bytes):
            continue
        ev = json.loads(msg)
        name = ev.get("header", {}).get("event")
        if name == "result-generated":
            s = ev.get("payload", {}).get("output", {}).get("sentence", {}) or {}
            sid, txt = s.get("sentence_id"), s.get("text", "")
            if not txt:
                continue
            if s.get("sentence_end"):
                segs[sid] = txt
                if sid == live_id:
                    live, live_id = "", None
                print(f"  ✔ [{s.get('begin_time')}-{s.get('end_time')}ms] {txt}")
            else:
                live, live_id = txt, sid
                print(f"  … {txt}")
        elif name == "task-finished":
            print("[✓] task-finished")
            break
        elif name == "task-failed":
            print("[✗] task-failed:", json.dumps(ev, ensure_ascii=False)[:600])
            break
    ws.close()

    final = "".join(segs[k] for k in sorted(segs, key=lambda x: (x is None, x)))
    print("\n=== 定稿结果（按 sentence_id 去重拼接）===")
    print(final or "(空)")
    if final:
        print(f"\n[✓] 端到端跑通，模型={args.model}")


if __name__ == "__main__":
    main()
