#!/usr/bin/env python3
"""qwen3-asr-flash-realtime 探针（OpenAI Realtime 兼容协议）。

与 grep 里的 run-task 协议（ds_probe.py）完全不同：
    旧：run-task → result-generated(sentence) → finish-task
    新：session.update → input_audio_buffer.append(base64) → session.finish
        ← conversation.item.input_audio_transcription.text / .completed

用法：
    python scripts/realtime_probe.py /tmp/t50.wav                # 按真实速率发（测延迟）
    python scripts/realtime_probe.py /tmp/t50.wav --speed 4      # 4 倍速发（只测准确度）
    python scripts/realtime_probe.py /tmp/t50.wav --raw          # 打印全部原始事件
"""
import argparse
import asyncio
import base64
import json
import os
import sys
import time
import wave
from pathlib import Path

import websockets

ROOT = Path(__file__).resolve().parent.parent
URL = os.getenv("REALTIME_WS_URL", "wss://dashscope.aliyuncs.com/api-ws/v1/realtime")
MODEL = os.getenv("REALTIME_MODEL", "qwen3-asr-flash-realtime")
CHUNK = 3200  # 100ms @ 16k/16bit/mono


def load_env():
    d = {}
    p = ROOT / ".env"
    if p.exists():
        for line in p.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            d[k.strip()] = v.strip().strip('"').strip("'")
    d.update(os.environ)
    return d


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--speed", type=float, default=1.0, help="发送倍速，1.0=真实速率")
    ap.add_argument("--raw", action="store_true", help="打印全部原始事件")
    ap.add_argument("--lang", default="zh")
    a = ap.parse_args()

    key = load_env().get("DASHSCOPE_API_KEY", "")
    if not key:
        sys.exit("缺少 DASHSCOPE_API_KEY")

    w = wave.open(a.wav)
    if w.getframerate() != 16000 or w.getnchannels() != 1 or w.getsampwidth() != 2:
        sys.exit(f"音频格式不对: {w.getframerate()}Hz {w.getnchannels()}ch {w.getsampwidth()*8}bit")
    pcm = w.readframes(w.getnframes())
    dur = len(pcm) / 32000
    print(f"[i] {a.wav} {dur:.2f}s, {len(pcm)//CHUNK} chunks, speed={a.speed}x")

    url = f"{URL}?model={MODEL}"
    hdr = {"Authorization": f"Bearer {key}"}
    try:
        ws = await websockets.connect(url, additional_headers=hdr, max_size=None)
    except TypeError:
        ws = await websockets.connect(url, extra_headers=hdr, max_size=None)

    t0 = time.time()
    first_text = None
    finals = []
    partials = 0
    last_partial = ""

    async def down():
        nonlocal first_text, partials, last_partial
        async for raw in ws:
            if isinstance(raw, bytes):
                continue
            ev = json.loads(raw)
            typ = ev.get("type", "")
            if a.raw:
                print(f"  [{time.time()-t0:6.2f}s] {typ} {json.dumps({k: v for k, v in ev.items() if k != 'type'}, ensure_ascii=False)[:240]}")

            if typ.endswith("input_audio_transcription.text"):
                # 注意：增量文本落在 stash，text 常为空串
                txt = ev.get("text") or ev.get("stash") or ev.get("delta") or ""
                if txt:
                    if first_text is None:
                        first_text = time.time() - t0
                        print(f"[<] 首个增量 @ {first_text:.2f}s")
                    partials += 1
                    last_partial = txt
                    print(f"    ~ {txt!r}")
            elif typ.endswith("input_audio_transcription.completed"):
                txt = ev.get("transcript", "")
                finals.append(txt)
                print(f"[<] 定稿 @ {time.time()-t0:6.2f}s  {txt!r}")
            elif typ in ("session.created", "session.updated"):
                print(f"[<] {typ}")
            elif typ == "session.finished":
                print(f"[<] session.finished @ {time.time()-t0:.2f}s")
                return
            elif typ == "error" or "error" in typ:
                print(f"[!] {typ} {json.dumps(ev, ensure_ascii=False)[:400]}")

    dtask = asyncio.create_task(down())
    await asyncio.sleep(0.3)

    await ws.send(json.dumps({
        "type": "session.update",
        "session": {
            "input_audio_format": "pcm",  # 注意：不是 pcm16，服务端只认 pcm
            "sample_rate": 16000,
            "input_audio_transcription": {"model": MODEL, "language": a.lang},
        },
    }))
    await asyncio.sleep(0.3)

    t_send = time.time()
    for i in range(0, len(pcm), CHUNK):
        chunk = pcm[i:i + CHUNK]
        await ws.send(json.dumps({
            "type": "input_audio_buffer.append",
            "audio": base64.b64encode(chunk).decode(),
        }))
        if a.speed > 0:
            await asyncio.sleep(CHUNK / 32000 / a.speed)

    send_done = time.time() - t_send
    print(f"[i] 音频发完，用时 {send_done:.2f}s（音频 {dur:.2f}s）")
    await ws.send(json.dumps({"type": "session.finish"}))

    try:
        await asyncio.wait_for(dtask, timeout=25)
    except asyncio.TimeoutError:
        print("[!] 25s 内没收到 session.finished")
        dtask.cancel()

    print("\n" + "=" * 60)
    print(f"首字延迟  : {first_text:.2f}s" if first_text else "首字延迟  : 未返回")
    print(f"发送完成  : {send_done:.2f}s")
    print(f"增量次数  : {partials}")
    print(f"定稿句数  : {len(finals)}")
    print("=" * 60)
    full = "".join(finals)
    print(full)
    print("=" * 60)
    print(f"共 {len(full)} 字")


asyncio.run(main())
