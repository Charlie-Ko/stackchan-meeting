#!/usr/bin/env python3
"""直连百炼，打印每一个 result-generated 原始事件，用于定位文本丢失点。

用法：python ds_probe.py /tmp/v_hp.wav [--vocab v_xxx]
"""
import argparse
import asyncio
import json
import os
import sys
import time
import wave

import websockets

API_KEY = os.environ["DASHSCOPE_API_KEY"]
DS_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
MODEL = os.getenv("ASR_MODEL", "paraformer-realtime-v2")
SR = 16000


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--vocab", default="")
    ap.add_argument("--params", default="", help="额外 parameters JSON")
    a = ap.parse_args()

    w = wave.open(a.wav)
    pcm = w.readframes(w.getnframes())
    dur = len(pcm) / (SR * 2)
    print(f"[i] {a.wav} {dur:.2f}s")

    extra = json.loads(a.params) if a.params else {}
    params = {"format": "pcm", "sample_rate": SR, "language_hints": ["zh"], **extra}
    if a.vocab:
        params["vocabulary_id"] = a.vocab
    print("[i] params:", json.dumps(params, ensure_ascii=False))

    hdr = {"Authorization": f"bearer {API_KEY}", "X-DashScope-DataInspection": "enable"}
    try:
        ws = await websockets.connect(DS_URL, additional_headers=hdr, max_size=None)
    except TypeError:
        ws = await websockets.connect(DS_URL, extra_headers=hdr, max_size=None)

    tid = "probe-" + str(int(time.time()))
    await ws.send(json.dumps({
        "header": {"action": "run-task", "task_id": tid, "streaming": "duplex"},
        "payload": {
            "task_group": "audio", "task": "asr", "function": "recognition",
            "model": MODEL, "parameters": params, "input": {},
        },
    }))

    ready = asyncio.Event()

    async def down():
        async for raw in ws:
            if isinstance(raw, bytes):
                continue
            ev = json.loads(raw)
            name = ev.get("header", {}).get("event")
            if name == "task-started":
                ready.set()
                print("[<] task-started")
                continue
            if name in ("task-finished", "task-failed"):
                print("[<]", name, json.dumps(ev.get("payload", {}), ensure_ascii=False)[:300])
                return
            if name != "result-generated":
                print("[<]", name)
                continue
            s = (ev.get("payload", {}).get("output", {}) or {}).get("sentence") or {}
            end = s.get("sentence_end")
            print("   ev sid=%-3s end=%-5s %5s-%5sms  %r" % (
                s.get("sentence_id"), end, s.get("begin_time"), s.get("end_time"), s.get("text")))

    dtask = asyncio.create_task(down())
    try:
        await asyncio.wait_for(ready.wait(), timeout=15)
    except asyncio.TimeoutError:
        print("[✗] 等不到 task-started")
        return

    step = SR * 2 // 10  # 100ms
    t0 = time.time()
    for i in range(0, len(pcm), step):
        await ws.send(pcm[i:i + step])
        el = time.time() - t0
        want = (i + step) / (SR * 2)
        if el < want:
            await asyncio.sleep(want - el)
    await ws.send(json.dumps({
        "header": {"action": "finish-task", "task_id": tid, "streaming": "duplex"},
        "payload": {"input": {}},
    }))
    try:
        await asyncio.wait_for(dtask, timeout=20)
    except asyncio.TimeoutError:
        print("[!] 下行超时")
    await ws.close()


asyncio.run(main())
