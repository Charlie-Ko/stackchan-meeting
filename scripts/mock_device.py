#!/usr/bin/env python3
"""模拟设备：向网关推 PCM，验证端到端链路。

用法：
    python scripts/mock_device.py --url ws://127.0.0.1:8000/ws/device --wav /tmp/asr_test.wav
    python scripts/mock_device.py --url ws://127.0.0.1:8000/ws/device --token xxx --wav a.wav --realtime
"""
import argparse
import asyncio
import json
import os
import sys
import wave

import websockets

# 本机可能配了 HTTP 代理，连本地网关会被代理拦（502），这里清掉
for _k in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
           "http_proxy", "https_proxy", "all_proxy"):
    os.environ.pop(_k, None)


def read_pcm(path):
    with wave.open(path, "rb") as w:
        return w.readframes(w.getnframes()), w.getframerate(), w.getnchannels(), w.getsampwidth()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="ws://127.0.0.1:8000/ws/device")
    ap.add_argument("--token", default="")
    ap.add_argument("--wav", required=True)
    ap.add_argument("--frame-ms", type=int, default=100)
    ap.add_argument("--realtime", action="store_true", help="按真实语速推流（默认半速）")
    args = ap.parse_args()

    pcm, sr, ch, sw = read_pcm(args.wav)
    if (sr, ch, sw) != (16000, 1, 2):
        sys.exit(f"音频需 16k/1ch/16bit，当前 {sr}/{ch}/{sw*8}")
    print(f"[i] 音频 {len(pcm)}B / {len(pcm)/(sr*2):.2f}s")

    url = args.url + (f"?token={args.token}" if args.token else "")
    async with websockets.connect(url, max_size=None) as ws:
        # 等 ready
        first = await asyncio.wait_for(ws.recv(), timeout=20)
        print("[<]", first)
        if '"ready"' not in first:
            sys.exit("[✗] 未就绪")

        step = int(sr * 2 * args.frame_ms / 1000)
        speed = 1.0 if args.realtime else 0.5

        async def pump():
            for i in range(0, len(pcm), step):
                await ws.send(pcm[i:i + step])
                await asyncio.sleep(args.frame_ms / 1000 * speed)
            await ws.send(json.dumps({"t": "stop"}))
            print(f"[i] 推流完成，共 {(len(pcm)+step-1)//step} 帧")

        async def listen():
            async for raw in ws:
                if isinstance(raw, bytes):
                    continue
                d = json.loads(raw)
                t = d.get("t")
                if t == "p":
                    print(f"  … [{d.get('s')}] {d.get('x')}")
                elif t == "f":
                    print(f"  ✔ [{d.get('s')}] {d.get('x')}  ({d.get('b')}-{d.get('e')}ms)")
                elif t == "done":
                    # 网关会随 done 下发手机端链接（设备拿去画二维码），这里打出来方便核对
                    q = d.get("q") or ""
                    if q:
                        print(f"[✓] done  手机端: {q}")
                    else:
                        print("[✓] done")
                    return
                elif t == "err":
                    print("[✗]", d.get("x"))
                    return
                elif t == "pong":
                    pass
                else:
                    print("[<]", d)

        await asyncio.gather(pump(), listen())


if __name__ == "__main__":
    asyncio.run(main())
