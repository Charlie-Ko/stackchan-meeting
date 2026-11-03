#!/usr/bin/env python3
"""飞书接入自检：换 tenant_access_token，验机器人/云空间/流式识别权限。

用法（venv）：
  /Users/charlie/.workbuddy/binaries/python/envs/default/bin/python scripts/feishu_verify.py

安全：只从 .env 读凭证，绝不打印 App Secret / access_token。
"""
import base64
import json
import os
import sys
import uuid

import requests

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = "https://open.feishu.cn/open-apis"


def load_env(path):
    d = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            d[k.strip()] = v.strip().strip('"').strip("'")
    return d


def main():
    env = load_env(os.path.join(ROOT, ".env"))
    app_id = env.get("FEISHU_APP_ID", "")
    app_secret = env.get("FEISHU_APP_SECRET", "")
    if not app_id or not app_secret:
        print("MISSING_CREDENTIALS: .env 里 App ID / Secret 没填")
        return 1
    print(f"app_id={app_id}  (secret 已读取，不显示)")

    # 1) tenant_access_token
    r = requests.post(
        BASE + "/auth/v3/tenant_access_token/internal",
        json={"app_id": app_id, "app_secret": app_secret},
        timeout=15,
    )
    d = r.json()
    print(f"[token] http={r.status_code} code={d.get('code')} msg={d.get('msg')}")
    if d.get("code") != 0:
        print("→ 凭证错误或应用未启用，先查 App ID/Secret 与发布状态")
        return 1
    token = d["tenant_access_token"]
    print(f"[token] expire={d.get('expire')}s (ok，不打印 token)")
    H = {"Authorization": "Bearer " + token}

    # 2) 机器人能力 + im 权限
    try:
        b = requests.get(BASE + "/bot/v3/info", headers=H, timeout=15).json()
        bot = b.get("bot") or {}
        print(f"[bot] code={b.get('code')} msg={b.get('msg')} name={bot.get('app_name')}")
    except Exception as e:
        print(f"[bot] err={e}")

    # 3) 云空间 drive 权限（取根目录 meta）
    try:
        v = requests.get(BASE + "/drive/explorer/v2/root_folder/meta", headers=H, timeout=15).json()
        print(f"[drive] code={v.get('code')} msg={v.get('msg')} root={v.get('data', {}).get('token')}")
    except Exception as e:
        print(f"[drive] err={e}")

    # 4) 流式语音识别（送 200ms 静音，只为验权限/连通，不指望出字）
    # 已实测字段结构（缺 config 会报 field validation failed）：
    #   config: stream_id / sequence_id / action / engine_type / format
    #   speech: data(base64) / is_end
    pcm = b"\x00" * 6400  # 16k * 16bit * 1ch * 0.2s
    payload = {
        "config": {
            "stream_id": "verify_" + uuid.uuid4().hex[:8],
            "sequence_id": 0,
            "action": "start",
            "engine_type": "16k_auto",
            "format": "pcm",
        },
        "speech": {"data": base64.b64encode(pcm).decode(), "is_end": True},
    }
    try:
        s = requests.post(
            BASE + "/speech_to_text/v1/speech/stream_recognize",
            headers=H, json=payload, timeout=20,
        )
        j = s.json()
        print(f"[asr] http={s.status_code} code={j.get('code')} msg={j.get('msg')} "
              f"text={(j.get('data') or {}).get('text', '')!r}")
    except Exception as e:
        print(f"[asr] err={e}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
