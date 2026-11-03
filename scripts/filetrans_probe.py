#!/usr/bin/env python3
"""qwen-audio-3.1-asr-flash-filetrans 探针 —— 验证说话人分离的真实返回结构。

在网关机器上跑（要能读到 .env 和 recordings）：
    python3 scripts/filetrans_probe.py <meeting_id> [--no-diar]

只用 urllib，不引第三方依赖（服务器上没装 requests）。
"""
import argparse
import json
import sys
import time
import urllib.request
from pathlib import Path

MODEL = "qwen-audio-3.1-asr-flash-filetrans"
ENV = Path("/opt/meeting-gw/.env")


def load_env():
    d = {}
    for line in ENV.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            d[k.strip()] = v.strip().strip('"').strip("'")
    return d


def post(url, body, key):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), headers={
        "Authorization": f"Bearer {key}",
        "Content-Type": "application/json",
        "X-DashScope-Async": "enable",
    }, method="POST")
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode())


def get(url, key):
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {key}"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mid")
    ap.add_argument("--no-diar", action="store_true")
    ap.add_argument("--speakers", type=int, default=0, help="speaker_count 参考值")
    a = ap.parse_args()

    env = load_env()
    key = env.get("DASHSCOPE_API_KEY", "")
    ws = env.get("DASHSCOPE_WORKSPACE_ID", "")
    base = (f"https://{ws}.cn-beijing.maas.aliyuncs.com" if ws
            else "https://dashscope.aliyuncs.com")

    # 会议 JSON 里拿录音文件名
    rec = json.loads((Path("/opt/meeting-gw/meetings") / f"{a.mid}.json").read_text())
    audio_url = f"{env['PUBLIC_BASE']}/{env['WEB_KEY']}/{a.mid}/audio"
    print(f"[i] 会议 {a.mid}  录音 {rec.get('audio')}  {round(rec.get('duration', 0))}s")
    print(f"[i] URL {audio_url}")
    print(f"[i] 端点 {base}")

    params = {"channel_id": [0], "language_hints": ["zh", "en"]}
    if not a.no_diar:
        params["diarization_enabled"] = True
        if a.speakers:
            params["speaker_count"] = a.speakers
    # 复用线上那 26 个热词
    vocab = {}
    vp = Path("/opt/meeting-gw/vocab.json")
    if vp.exists():
        try:
            raw = json.loads(vp.read_text())
            vocab = raw if isinstance(raw, dict) else {}
        except Exception:
            pass
    if vocab:
        params["vocabulary"] = vocab
        print(f"[i] 热词 {len(vocab)} 个")

    t0 = time.time()
    r = post(f"{base}/api/v1/services/audio/asr/transcription", {
        "model": MODEL,
        "input": {"file_urls": [audio_url]},
        "parameters": params,   # 必传，空对象也要传
    }, key)
    print("[<] 提交:", json.dumps(r, ensure_ascii=False)[:300])

    tid = (r.get("output") or {}).get("task_id")
    if not tid:
        sys.exit("没拿到 task_id")

    status, out = "PENDING", {}
    for _ in range(120):
        time.sleep(2)
        q = get(f"{base}/api/v1/tasks/{tid}", key)
        out = q.get("output") or {}
        status = out.get("task_status")
        print(f"    {time.time()-t0:5.1f}s {status}")
        if status in ("SUCCEEDED", "FAILED"):
            break
    if status != "SUCCEEDED":
        print(json.dumps(out, ensure_ascii=False)[:1000])
        sys.exit(f"任务失败 {status}")

    results = out.get("results") or []
    if not results or results[0].get("subtask_status") != "SUCCEEDED":
        print(json.dumps(results, ensure_ascii=False)[:800])
        sys.exit("子任务失败")

    turl = results[0]["transcription_url"]
    with urllib.request.urlopen(turl, timeout=60) as r:
        res = json.loads(r.read().decode())

    tr = (res.get("transcripts") or [{}])[0]
    sents = tr.get("sentences") or []
    print(f"\n[i] 文件属性: {json.dumps(res.get('properties', {}), ensure_ascii=False)[:300]}")
    print(f"[i] 共 {len(sents)} 句，耗时 {time.time()-t0:.1f}s")
    ids = {}
    for s in sents:
        ids[s.get("speaker_id")] = ids.get(s.get("speaker_id"), 0) + 1
    print(f"[i] speaker_id 分布: {ids}")
    print("\n--- 前 12 句 ---")
    for s in sents[:12]:
        print(f"  [{s.get('begin_time')}-{s.get('end_time')}] spk={s.get('speaker_id')} "
              f"{s.get('text')}")
    print("\n--- 原始字段名 ---")
    print(sorted(sents[0].keys()) if sents else "(无句子)")
    Path(f"/tmp/ft_{a.mid}.json").write_text(json.dumps(res, ensure_ascii=False, indent=1))
    print(f"[i] 完整结果已存 /tmp/ft_{a.mid}.json")


main()
