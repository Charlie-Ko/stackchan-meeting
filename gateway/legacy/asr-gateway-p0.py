#!/usr/bin/env python3
# asr_gateway.py
# 桌面会议记录机器人 - P0 网关
# 接收 StickS3 上传的 WAV 音频，转成文字回传（{"text": "..."}），设备端滚动显示。
#
# 用法：
#   1) 快速验证（无需任何密钥，立刻看到屏上出字）：
#        python3 asr_gateway.py --backend mock
#   2) 真实识别（阿里云一句话识别，需先配环境变量）：
#        export ALIYUN_AK_ID=xxxx
#        export ALIYUN_AK_SECRET=xxxx
#        export ALIYUN_APP_KEY=xxxx        # 在智能语音交互控制台创建项目拿到的 AppKey
#        python3 asr_gateway.py --backend aliyun
#
# 设备端把 GATEWAY_HOST 填成跑本脚本的机器 IP（同一 WiFi）。
# 依赖：pip install flask requests

import argparse
import base64
import hashlib
import hmac
import json
import os
import time
import uuid

from flask import Flask, request, Response

app = Flask(__name__)


# ---------------- Mock 后端（开箱即跑，验证设备→网关→屏幕链路） ----------------
MOCK_LINES = [
    "你好，这是一段识别文字。",
    "我们在讨论下季度的商户拓展计划。",
    "请把待办事项记到会议纪要里。",
    "这个方案的成本要再算一遍。",
    "OK，那就先按这个方向推进。",
]
_mock_i = 0


def mock_asr(wav_bytes: bytes) -> str:
    global _mock_i
    line = MOCK_LINES[_mock_i % len(MOCK_LINES)]
    _mock_i += 1
    return line


# ---------------- 阿里云一句话识别后端 ----------------
_nls_token = {"id": None, "expire": 0}


def _aliyun_rpc_signature(params: dict, secret: str) -> str:
    # 阿里云 RPC 签名（HMAC-SHA1）
    keys = sorted(params.keys())
    canonical = "&".join(
        f"{_pops(k)}={_pops(params[k])}" for k in keys
    )
    string_to_sign = "GET&%2F&" + _pops(canonical)
    key = (secret + "&").encode("utf-8")
    sig = hmac.new(key, string_to_sign.encode("utf-8"), hashlib.sha1).digest()
    return base64.b64encode(sig).decode("ascii")


def _pops(s: str) -> str:
    # 百分比编码（阿里云规范）
    import urllib.parse

    return urllib.parse.quote(str(s), safe="")


def get_nls_token() -> str:
    ak_id = os.environ["ALIYUN_AK_ID"]
    ak_secret = os.environ["ALIYUN_AK_SECRET"]
    if _nls_token["id"] and time.time() < _nls_token["expire"] - 60:
        return _nls_token["id"]
    import urllib.request

    params = {
        "AccessKeyId": ak_id,
        "Action": "CreateToken",
        "Format": "JSON",
        "RegionId": "cn-shanghai",
        "SignatureMethod": "HMAC-SHA1",
        "SignatureNonce": str(uuid.uuid4()),
        "SignatureVersion": "1.0",
        "Timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "Version": "2019-02-28",
    }
    params["Signature"] = _aliyun_rpc_signature(params, ak_secret)
    url = "https://nls-meta.cn-shanghai.aliyuncs.com/?" + "&".join(
        f"{k}={params[k]}" for k in sorted(params.keys())
    )
    with urllib.request.urlopen(url, timeout=10) as r:
        body = json.loads(r.read().decode("utf-8"))
    _nls_token["id"] = body["Token"]["Id"]
    _nls_token["expire"] = body["Token"]["ExpireTime"]
    return _nls_token["id"]


def aliyun_asr(wav_bytes: bytes) -> str:
    import urllib.request

    token = get_nls_token()
    app_key = os.environ["ALIYUN_APP_KEY"]
    qs = (
        f"appkey={app_key}&token={token}&format=wav"
        f"&sample_rate=16000&enable_punctuation=true&version=v1"
    )
    url = f"https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/asr?{qs}"
    req = urllib.request.Request(url, data=wav_bytes, method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    with urllib.request.urlopen(req, timeout=15) as r:
        body = json.loads(r.read().decode("utf-8"))
    if body.get("status") == 20000000:
        return body.get("result", "")
    return ""


BACKEND = {"mode": "mock"}


@app.route("/asr", methods=["POST"])
def asr():
    audio = request.get_data()
    try:
        if BACKEND["mode"] == "aliyun":
            text = aliyun_asr(audio)
        else:
            text = mock_asr(audio)
    except Exception as e:  # 出错不让设备卡死，返回空文本
        app.logger.error("ASR error: %s", e)
        return Response(json.dumps({"text": ""}), mimetype="application/json")
    return Response(json.dumps({"text": text}), mimetype="application/json")


@app.route("/health", methods=["GET"])
def health():
    return Response(json.dumps({"ok": True, "backend": BACKEND["mode"]}),
                    mimetype="application/json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--backend", choices=["mock", "aliyun"], default="mock")
    args = parser.parse_args()
    BACKEND["mode"] = args.backend
    if args.backend == "aliyun":
        for k in ("ALIYUN_AK_ID", "ALIYUN_AK_SECRET", "ALIYUN_APP_KEY"):
            if k not in os.environ:
                raise SystemExit(f"缺少环境变量 {k}")
    print(f"[asr_gateway] backend={args.backend} 监听 http://{args.host}:{args.port}")
    app.run(host=args.host, port=args.port, threaded=True)
