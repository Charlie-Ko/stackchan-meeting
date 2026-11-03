#!/usr/bin/env python3
"""会议记录机器人 · 云端网关

职责：
    设备(Chan/StickS3) ──WS 二进制 PCM──▶ 本网关 ──WS──▶ 阿里云百炼 paraformer-realtime-v2
                                        ◀── 精简 JSON ──

设备协议（文本帧 JSON）：
    上行  {"t":"ping"}  / {"t":"stop"}
    下行  {"t":"ready"}                       会话就绪
          {"t":"p","s":1,"x":"当前句"}         中间结果（累积式，整句覆盖）
          {"t":"f","s":1,"x":"定稿","b":0,"e":5280}  定稿句子
          {"t":"done"}                       会话结束
          {"t":"err","x":"..."}              错误
          {"t":"pong"}
二进制帧：16kHz / 16bit / 单声道 PCM，建议每帧 3200 字节（100ms）

鉴权：连接 query 带 ?token=DEVICE_TOKEN（未配置 DEVICE_TOKEN 时跳过校验）

环境变量：
    DASHSCOPE_API_KEY    必填，百炼 API Key
    DEVICE_TOKEN         建议配置，设备接入口令
    GATEWAY_PORT         默认 8000
    ASR_MODEL            默认 qwen-audio-3.0-asr-flash-streaming
"""
import asyncio
import array
import hashlib
import json
import logging
import math
import os
import struct
import sys
import time
import urllib.request
import uuid
from pathlib import Path

import websockets
from fastapi import FastAPI, Query, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse

import meeting as mt

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("gw")

DS_URL = os.getenv("DASHSCOPE_WS_URL", "wss://dashscope.aliyuncs.com/api-ws/v1/inference")
# 模型选择是识别准确率的第一杠杆。paraformer-realtime-v2 在干净录音（TTS）上几乎全对，
# 但换成真实麦克风 + 15dB 底噪就崩：实测同一段音频，paraformer 出「内匿名报料号二手网站消有方时」，
# qwen-audio-3.0 出「这些全部是圈内匿名爆料号的二手网传消息」。两者差的是模型，不是音频。
MODEL = os.getenv("ASR_MODEL", "qwen-audio-3.0-asr-flash-streaming")
SAMPLE_RATE = int(os.getenv("ASR_SAMPLE_RATE", "16000"))
API_KEY = os.getenv("DASHSCOPE_API_KEY", "")
DEVICE_TOKEN = os.getenv("DEVICE_TOKEN", "")
USAGE_FILE = Path(os.getenv("USAGE_FILE", "usage.json"))
FREE_SECONDS_PER_MONTH = 36000  # 百炼每月 10 小时免费额度

# ---- 录音落盘（排障刚需）+ 热词 ----
REC_DIR = Path(os.getenv("RECORD_DIR", "recordings"))
REC_ENABLE = os.getenv("REC_ENABLE", "1") != "0"
TERMS_FILE = Path(os.getenv("TERMS_FILE", "terms.txt"))
VOCAB_FILE = Path(os.getenv("VOCAB_FILE", ".vocab.json"))
DS_HTTP = os.getenv("DASHSCOPE_HTTP_URL", "https://dashscope.aliyuncs.com/api/v1")
VOCAB_WEIGHT = int(os.getenv("VOCAB_WEIGHT", "5"))

app = FastAPI(title="Meeting Recorder Gateway", version="1.0")

# ---- 运行时统计 ----
STATS = {
    "sessions_total": 0,
    "sessions_active": 0,
    "audio_seconds_total": 0.0,
    "last_error": None,
    "started_at": time.time(),
}
_send_locks: dict = {}


def _load_usage() -> dict:
    try:
        return json.loads(USAGE_FILE.read_text(encoding="utf-8"))
    except Exception:
        return {}


def _bump_usage(seconds: float) -> dict:
    month = time.strftime("%Y-%m")
    data = _load_usage()
    cur = data.get(month, {"seconds": 0.0, "sessions": 0})
    cur["seconds"] = round(cur["seconds"] + seconds, 1)
    cur["sessions"] += 1
    data[month] = cur
    try:
        USAGE_FILE.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    except Exception as e:
        log.warning("写用量失败: %s", e)
    return data


async def _connect_dashscope():
    """连百炼 WS，兼容 websockets 新旧版本的 header 参数名。"""
    headers = {
        "Authorization": f"bearer {API_KEY}",
        "X-DashScope-DataInspection": "enable",
    }
    kw = dict(max_size=None, ping_interval=20, ping_timeout=20, open_timeout=10)
    try:
        return await websockets.connect(DS_URL, additional_headers=headers, **kw)
    except TypeError:
        return await websockets.connect(DS_URL, extra_headers=headers, **kw)


# ---- 百炼上行连接预热复用 ----
# 以前每来一次设备会话都重新 TLS+WS 建连，耗时 1s+，是"点开始后等很久"的大头。
# 保一条热连接反复复用（百炼按 task_id 区分会话），但同一时刻只能有一个会话用它——
# 两个 recv 并发读同一条连接会撞 ConcurrencyError，两个活跃 task 会被拒
# （Invalid action('run-task')）。热连接被占用时，后来的会话开专用连接兜底。
_ds_lock = asyncio.Lock()
_ds_ws = None
_ds_busy = False


def _ds_alive(ws) -> bool:
    """websockets 新旧版的状态属性不同：旧版 .open，新版 .state。"""
    try:
        st = getattr(ws, "state", None)
        if st is not None:
            return getattr(st, "name", "") == "OPEN"
        return bool(getattr(ws, "open", False))
    except Exception:
        return False


async def _acquire_dashscope():
    """返回 (ws, shared)。shared=True 是热连接，用完要 _release；False 是专用连接，用完即关。"""
    global _ds_ws, _ds_busy
    async with _ds_lock:
        if not _ds_busy:
            if _ds_ws is not None and not _ds_alive(_ds_ws):
                try:
                    await _ds_ws.close()
                except Exception:
                    pass
                _ds_ws = None
            if _ds_ws is None:
                t0 = time.time()
                _ds_ws = await _connect_dashscope()
                log.info("百炼上行建连 %.2fs", time.time() - t0)
            _ds_busy = True
            return _ds_ws, True
    # 热连接被别的会话占着：开专用连接，不抢不排队
    log.info("热连接占用中，开会话专用连接")
    return await _connect_dashscope(), False


async def _release_dashscope(ws, shared: bool, broken: bool = False):
    global _ds_ws, _ds_busy
    if not shared:
        try:
            await ws.close()
        except Exception:
            pass
        return
    async with _ds_lock:
        _ds_busy = False
        if broken and _ds_ws is ws:
            try:
                await ws.close()
            except Exception:
                pass
            _ds_ws = None


async def _dev_send(device: WebSocket, key: str, payload) -> None:
    """串行化对设备的写操作，避免并发 send 冲突。"""
    lock = _send_locks.setdefault(key, asyncio.Lock())
    async with lock:
        if isinstance(payload, (dict, list)):
            await device.send_text(json.dumps(payload, ensure_ascii=False, separators=(",", ":")))
        else:
            await device.send_text(payload)


# ---------------- 录音落盘：识别不准第一件事是把音频捞出来听 ----------------
# 增量写 PCM，会话结束补 WAV 头。设备挂掉/断网也不会丢已收到的部分。
def _rec_open(tag: str):
    if not REC_ENABLE:
        return None, None
    try:
        REC_DIR.mkdir(parents=True, exist_ok=True)
        name = time.strftime("%Y%m%d-%H%M%S") + f"-{tag}.wav"
        path = REC_DIR / name
        fh = open(path, "wb")
        fh.write(b"\x00" * 44)   # 先占位，结束时回填
        return fh, path
    except Exception as e:
        log.warning("开录音文件失败: %s", e)
        return None, None


def _rec_close(fh, path):
    if not fh:
        return
    try:
        n = fh.tell() - 44
        fh.seek(0)
        fh.write(struct.pack(
            "<4sI4s4sIHHIIHH4sI",
            b"RIFF", 36 + n, b"WAVE", b"fmt ", 16,
            1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16, b"data", n,
        ))
        fh.close()
        log.info("录音已保存 %s (%.1fs)", path, n / (SAMPLE_RATE * 2))
    except Exception as e:
        log.warning("收尾录音文件失败: %s", e)


class _AudioProbe:
    """边转发边统计电平：削波/过小在网关侧就能看出来，不用等下载"""

    def __init__(self):
        self.peak = 0
        self.sumsq = 0
        self.n = 0
        self.clip = 0

    def feed(self, buf: bytes):
        if not buf:
            return
        n = len(buf) // 2
        try:
            a = array.array("h")
            a.frombytes(buf[: n * 2])
            if sys.byteorder != "little":
                a.byteswap()
        except Exception:
            return
        peak = self.peak
        clip = self.clip
        sumsq = self.sumsq
        for v in a:
            if v < 0:
                v = -v
            if v > peak:
                peak = v
            if v >= 32000:
                clip += 1
            sumsq += v * v
        self.peak, self.clip, self.sumsq = peak, clip, sumsq
        self.n += n

    def report(self) -> str:
        if not self.n:
            return "无音频"
        rms = (self.sumsq / self.n) ** 0.5

        def db(x):
            return 20 * math.log10(x / 32768.0) if x > 0 else -90.0

        return (f"peak={db(self.peak):.1f}dB rms={db(rms):.1f}dB "
                f"clip={self.clip * 100.0 / self.n:.2f}% n={self.n}")


# ---------------- 热词 ----------------
# 两种机制，取决于模型：
#   qwen-audio* / fun-asr* → parameters.vocabulary 直接内联 {"词": 权重}，免建表、即时生效
#   paraformer 系列       → 必须先建 speech-biasing 热词表，run-task 带 vocabulary_id
# 内联方式最省事，所以新模型走这条路；老模型的建表逻辑保留着以免有人切回去。
_vocab_id = ""
# qwen-audio 的热词权重：1~5 是普通热词，50 是超级热词（上限 50 个，滥用会误触发）
INLINE_VOCAB_MAX_W = 5


def _inline_vocab_supported() -> bool:
    return MODEL.startswith(("qwen-audio", "fun-asr"))


def _build_inline_vocab() -> dict:
    if not _inline_vocab_supported():
        return {}
    terms = _load_terms()
    if not terms:
        return {}
    return {t: min(VOCAB_WEIGHT, INLINE_VOCAB_MAX_W) for t in terms}


def _load_terms() -> list:
    if not TERMS_FILE.exists():
        return []
    out = []
    try:
        for line in TERMS_FILE.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            # 同一行可用逗号写别名，取第一个作为热词本体
            word = line.split(",")[0].strip()
            if word and len(word) <= 20:
                out.append(word)
    except Exception as e:
        log.warning("读词表失败: %s", e)
    return out[:500]


async def _ds_http(payload: dict) -> dict:
    def _do():
        req = urllib.request.Request(
            f"{DS_HTTP}/services/audio/asr/customization",
            data=json.dumps(payload, ensure_ascii=False).encode(),
            headers={
                "Authorization": f"Bearer {API_KEY}",
                "Content-Type": "application/json",
            },
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.loads(r.read().decode())

    return await asyncio.to_thread(_do)


async def _sync_hotwords():
    """把 terms.txt 同步成百炼热词表；已是最新则跳过。"""
    global _vocab_id
    if _inline_vocab_supported():
        # 内联热词模式：不需要建表，每次 run-task 直接带词表
        log.info("热词走内联模式（%d 词），跳过建表", len(_load_terms()))
        return
    terms = _load_terms()
    if not terms or not API_KEY:
        return
    # 热词表按模型绑定，model 变了要重建
    sig = hashlib.md5(("|".join(terms) + MODEL).encode()).hexdigest()
    cache = {}
    try:
        if VOCAB_FILE.exists():
            cache = json.loads(VOCAB_FILE.read_text(encoding="utf-8"))
    except Exception:
        cache = {}
    # 缓存里的 id 只在"同模型同词表"时才能信：不同模型的热词表互不通用，
    # 拿旧 id 去 update 会改到一张绑着旧模型的表上，识别时静默失效。
    if cache.get("sig") == sig and cache.get("id") and cache.get("model") == MODEL:
        _vocab_id = cache["id"]
        log.info("热词表复用 %s（%d 词）", _vocab_id, len(terms))
        return

    vocab = [{"text": t, "weight": VOCAB_WEIGHT, "lang": "zh"} for t in terms]
    # 热词表是绑定 target_model 创建的。换模型后必须用新表，否则要么报错要么静默不生效，
    # 所以前缀带上模型指纹；旧模型留下的表就自然被绕过了。
    vprefix = "chan-" + hashlib.md5(MODEL.encode()).hexdigest()[:6]
    res = {}
    try:
        # 先找同前缀的旧表，能更新就不新建（避免堆一堆垃圾表）
        old_id = cache.get("id") if cache.get("model") == MODEL else None
        if not old_id:
            lst = await _ds_http({
                "model": "speech-biasing",
                "input": {"action": "list_vocabulary", "prefix": vprefix, "page_no": 0, "page_size": 1},
            })
            arr = ((lst.get("output") or {}).get("vocabularies") or [])
            if arr:
                old_id = arr[0].get("vocabulary_id")
        if old_id:
            await _ds_http({
                "model": "speech-biasing",
                "input": {"action": "update_vocabulary", "vocabulary_id": old_id, "vocabulary": vocab},
            })
            vid = old_id
            log.info("热词表已更新 %s（%d 词）", vid, len(terms))
        else:
            res = await _ds_http({
                "model": "speech-biasing",
                "input": {
                    "action": "create_vocabulary",
                    "target_model": MODEL,
                    "prefix": vprefix,
                    "vocabulary": vocab,
                },
            })
            vid = (res.get("output") or {}).get("vocabulary_id")
            log.info("热词表已创建 %s（%d 词）: %s", vid, len(terms), "、".join(terms[:6]))
        if not vid:
            log.warning("热词表返回里没有 vocabulary_id: %s", json.dumps(res, ensure_ascii=False)[:200])
            return
        _vocab_id = vid
        VOCAB_FILE.write_text(json.dumps({"id": vid, "sig": sig, "terms": len(terms), "model": MODEL}), encoding="utf-8")
    except Exception as e:
        log.warning("热词表同步失败（不影响识别，只是没热词）: %s", e)


@app.get("/health")
async def health():
    usage = _load_usage()
    month = time.strftime("%Y-%m")
    used = usage.get(month, {}).get("seconds", 0.0)
    return {
        "ok": True,
        "model": MODEL,
        "sessions_total": STATS["sessions_total"],
        "sessions_active": STATS["sessions_active"],
        "audio_seconds_total": round(STATS["audio_seconds_total"], 1),
        "month_used_seconds": used,
        "month_free_seconds": FREE_SECONDS_PER_MONTH,
        "month_free_used_pct": round(used / FREE_SECONDS_PER_MONTH * 100, 2),
        "last_error": STATS["last_error"],
        "hotwords": {"mode": "inline" if _inline_vocab_supported() else "vocabulary_id",
                     "vocabulary_id": _vocab_id, "count": len(_load_terms())},
        "recordings": len(list(REC_DIR.glob("*.wav"))) if REC_DIR.exists() else 0,
        "uptime_seconds": round(time.time() - STATS["started_at"]),
    }


@app.get("/usage")
async def usage():
    return _load_usage()


def _check(token: str):
    return not DEVICE_TOKEN or token == DEVICE_TOKEN


@app.get("/recordings")
async def recordings(token: str = Query(default="")):
    """列出已落盘的会议音频，方便下载到本机用 Audacity 听、或拿去别的 ASR 对比。"""
    if not _check(token):
        return JSONResponse({"err": "bad token"}, status_code=403)
    if not REC_DIR.exists():
        return {"count": 0, "items": []}
    items = []
    for p in sorted(REC_DIR.glob("*.wav"), reverse=True)[:50]:
        st = p.stat()
        items.append({
            "name": p.name,
            "seconds": round(max(0, st.st_size - 44) / (SAMPLE_RATE * 2), 1),
            "url": f"/recordings/{p.name}" + (f"?token={token}" if token else ""),
        })
    return {"count": len(items), "items": items}


@app.get("/recordings/{name}")
async def recording_file(name: str, token: str = Query(default="")):
    if not _check(token):
        return JSONResponse({"err": "bad token"}, status_code=403)
    p = REC_DIR / os.path.basename(name)      # 只取文件名，防目录穿越
    if not p.exists():
        return JSONResponse({"err": "not found"}, status_code=404)
    return FileResponse(p, media_type="audio/wav", filename=p.name)


# ---------------- 手机端网页（扫二维码进来） ----------------
# 路径第一段就是访问密钥，二维码才能短到塞得下：
#   /m/<key>            历史列表
#   /m/<key>/<id>       单场详情（纪要 + 文字 + 录音）
def _web_ok(key: str) -> bool:
    return bool(mt.WEB_KEY) and key == mt.WEB_KEY


@app.get("/m/{key}")
async def web_list(key: str):
    if not _web_ok(key):
        return HTMLResponse(mt.render_404(), status_code=404)
    return HTMLResponse(mt.render_list(key))


@app.get("/m/{key}/{mid}")
async def web_detail(key: str, mid: str):
    if not _web_ok(key):
        return HTMLResponse(mt.render_404(), status_code=404)
    rec = mt.load(mid)
    if not rec:
        return HTMLResponse(mt.render_404(), status_code=404)
    return HTMLResponse(mt.render_detail(key, rec))


@app.get("/m/{key}/{mid}/audio")
async def web_audio(key: str, mid: str, dl: int = Query(default=0)):
    if not _web_ok(key):
        return JSONResponse({"err": "forbidden"}, status_code=403)
    rec = mt.load(mid)
    if not rec:
        return JSONResponse({"err": "not found"}, status_code=404)
    p = mt.audio_path(rec)
    if not p:
        return JSONResponse({"err": "no audio"}, status_code=404)
    # 在线播放走 inline，点"下载"才带 attachment
    return FileResponse(p, media_type="audio/wav",
                        filename=("meeting-" + mid + ".wav") if dl else None,
                        content_disposition_type="attachment" if dl else "inline")


@app.websocket("/ws/device")
async def device_ws(ws: WebSocket, token: str = Query(default="")):
    await ws.accept()

    if DEVICE_TOKEN and token != DEVICE_TOKEN:
        await ws.send_text(json.dumps({"t": "err", "x": "bad token"}))
        await ws.close(code=4003)
        log.warning("拒绝接入：token 不匹配")
        return

    key = uuid.uuid4().hex
    STATS["sessions_total"] += 1
    STATS["sessions_active"] += 1
    t_start = time.time()
    audio_bytes = 0
    task_id = uuid.uuid4().hex
    log.info("设备接入 %s", key[:8])

    ds = None
    ds_shared = False
    ds_broken = False       # 会话中途发现连接已死 → 归还时标记重建
    task_started = False    # run-task 成功过 → 异常退出时要补 finish-task，不然热连接上挂着僵尸 task
    rec_fh, rec_path = _rec_open(key[:8])
    probe = _AudioProbe()
    # 一场会议一条记录：文字边识别边攒，结束时落盘并生成纪要
    mrec = mt.create(t_start)
    log.info("会议记录 %s", mrec["id"])

    def persist_meeting() -> str:
        """把当前进度落盘并返回手机端链接。可重复调用（结束时补全时长与录音名）。

        注意：纪要和精转不在这里触发 —— 它们必须等录音文件关闭（WAV 头是最后补写的），
        否则百炼拉到的是一段没有头的残包。统一由 finally 里的 mt.finalize() 拉起。
        """
        mrec["duration"] = round(audio_bytes / (SAMPLE_RATE * 2), 1) if audio_bytes else 0.0
        if rec_path is not None:
            mrec["audio"] = rec_path.name
        mt.save(mrec)
        return mt.meeting_url(mrec)

    # 一句话会先被拆成多版 partial，只有到最后才带 sentence_end。按 sid 记住"最后有字的那版"，
    # 用于兜底：① 定稿文本为空时补字 ② 会话收尾时把没来得及判句尾的半句补成定稿。
    # 定义在 try 外面，finally 里设备掉线也能落盘，保证整场不丢。
    pending: dict = {}        # sid -> 最后一个非空 partial
    committed: set = set()    # 已定稿的 sid

    try:
        t_conn = time.time()
        ds, ds_shared = await _acquire_dashscope()

        run_task_msg = json.dumps({
            "header": {"action": "run-task", "task_id": task_id, "streaming": "duplex"},
            "payload": {
                "task_group": "audio",
                "task": "asr",
                "function": "recognition",
                "model": MODEL,
                "parameters": {
                    "format": "pcm",
                    "sample_rate": SAMPLE_RATE,
                    # 会议里夹杂英文术语是常态（KPI/ROI/AWS）。只标 zh 时，模型会把
                    # 识别不清的英文段直接判成噪音、返回空文本（实测英文对白 16 句里 13 句空）；
                    # 加 en 后中文识别无退化，英文段能捞回来。
                    "language_hints": ["zh", "en"],
                    **({"vocabulary": iv} if (iv := _build_inline_vocab()) else
                       {"vocabulary_id": _vocab_id} if _vocab_id else {}),
                },
                "input": {},
            },
        })

        # 热连接可能"看着活着其实已经死了"：发送失败就报废重建，再试一次
        for attempt in (1, 2):
            try:
                await ds.send(run_task_msg)
                break
            except Exception:
                if attempt == 2:
                    raise
                log.warning("百炼热连接发送失败，重建后重试")
                await _release_dashscope(ds, ds_shared, broken=True)
                ds, ds_shared = await _acquire_dashscope()

        finish_task_msg = json.dumps({
            "header": {"action": "finish-task", "task_id": task_id, "streaming": "duplex"},
            "payload": {"input": {}},
        })

        def mine(ev) -> bool:
            """一条连接上可能漂着上一个会话的迟到事件，只收自己 task_id 的。"""
            tid = ev.get("header", {}).get("task_id")
            return tid in (None, task_id)

        # 等 task-started
        started = False
        deadline = time.time() + 15
        while time.time() < deadline:
            raw = await asyncio.wait_for(ds.recv(), timeout=max(0.1, deadline - time.time()))
            if isinstance(raw, bytes):
                continue
            ev = json.loads(raw)
            if not mine(ev):
                continue
            name = ev.get("header", {}).get("event")
            if name == "task-started":
                started = True
                break
            if name == "task-failed":
                raise RuntimeError(f"task-failed: {json.dumps(ev, ensure_ascii=False)[:300]}")
        if not started:
            raise RuntimeError("等不到 task-started")
        task_started = True

        await _dev_send(ws, key, {"t": "ready"})
        log.info("百炼会话就绪 %s 建会话耗时%.2fs", key[:8], time.time() - t_conn)

        async def uplink():
            nonlocal audio_bytes
            while True:
                msg = await ws.receive()
                if msg.get("type") == "websocket.disconnect":
                    return "device_closed"
                chunk = msg.get("bytes")
                if chunk:
                    audio_bytes += len(chunk)
                    if rec_fh:
                        try:
                            rec_fh.write(chunk)
                        except Exception:
                            pass
                    probe.feed(chunk)
                    await ds.send(chunk)
                    continue
                text = msg.get("text")
                if not text:
                    continue
                try:
                    d = json.loads(text)
                except Exception:
                    continue
                if d.get("t") == "ping":
                    await _dev_send(ws, key, {"t": "pong"})
                elif d.get("t") == "stop":
                    await ds.send(finish_task_msg)
                    return "stopped"

        async def emit_final(sid, txt: str, b, e, why: str = ""):
            committed.add(sid)
            pending.pop(sid, None)
            await _dev_send(ws, key, {"t": "f", "s": sid, "x": txt, "b": b, "e": e})
            # 定稿文本落日志：识别质量能直接从 journalctl 看，不用每次回捞录音重跑
            log.info("定稿%s %s #%s %s", why, key[:8], sid, txt)
            STATS["last_text"] = txt
            mt.add_sentence(mrec, txt, b, e, sid)

        async def downlink():
            async for raw in ds:
                if isinstance(raw, bytes):
                    continue
                ev = json.loads(raw)
                if not mine(ev):
                    continue
                name = ev.get("header", {}).get("event")
                if name == "result-generated":
                    s = (ev.get("payload", {}).get("output", {}) or {}).get("sentence") or {}
                    txt = (s.get("text") or "").strip()
                    sid = s.get("sentence_id")
                    if s.get("sentence_end"):
                        use = txt or pending.get(sid, "")
                        if use:
                            await emit_final(sid, use, s.get("begin_time"),
                                             s.get("end_time"), "" if txt else "(空定稿补字)")
                        else:
                            committed.add(sid)
                            pending.pop(sid, None)
                    elif txt:
                        pending[sid] = txt
                        await _dev_send(ws, key, {"t": "p", "s": sid, "x": txt})
                elif name == "task-finished":
                    # 收尾补定稿：把还挂在 partial 上的句子全部落定（按 sid 顺序，保持语序）
                    for sid in sorted(pending.keys(), key=lambda v: (v is None, v)):
                        if sid not in committed:
                            await emit_final(sid, pending[sid], None, None, "(收尾补定稿)")
                    # 先把记录落盘，再把手机端链接随 done 一起发给设备 ——
                    # 设备拿到就在结束页画二维码，扫开即可看纪要/文字/录音
                    qr = persist_meeting()
                    await _dev_send(ws, key, {"t": "done", "q": qr})
                    return "finished"
                elif name == "task-failed":
                    STATS["last_error"] = json.dumps(ev, ensure_ascii=False)[:300]
                    await _dev_send(ws, key, {"t": "err", "x": STATS["last_error"]})
                    return "failed"

        up = asyncio.create_task(uplink())
        down = asyncio.create_task(downlink())
        done, _running = await asyncio.wait({up, down}, return_when=asyncio.FIRST_COMPLETED)

        # 关键：设备发 stop 后不能立刻掐断下游，必须等百炼把最后一句吐完
        if up in done and down not in done:
            try:
                up_res = up.result()
            except Exception:
                up_res = None
            if up_res == "stopped":
                try:
                    await asyncio.wait_for(asyncio.shield(down), timeout=10)
                except (asyncio.TimeoutError, Exception):
                    down.cancel()
            elif up_res == "device_closed":
                # 设备跑了：连接现在要留给下个会话复用，不能再靠 close 杀 task，
                # 补发 finish-task 让百炼那边正常收尾（不然服务端 task 挂到超时）
                try:
                    await ds.send(finish_task_msg)
                except Exception:
                    pass
                down.cancel()
            else:
                down.cancel()
        elif down in done and up not in done:
            up.cancel()

        for t in (up, down):
            if not t.done():
                t.cancel()
        await asyncio.gather(up, down, return_exceptions=True)
        task_started = False   # 正常收尾路径已发过 finish-task（或 task 已终结），finally 不再补发

        reason = "done"
        for t in (up, down):
            if t.cancelled():
                continue
            try:
                reason = t.result() or reason
            except Exception:
                pass
        log.info("会话结束 %s reason=%s audio=%.1fs", key[:8], reason, audio_bytes / (SAMPLE_RATE * 2))

    except WebSocketDisconnect:
        log.info("设备断开 %s", key[:8])
    except Exception as e:
        STATS["last_error"] = str(e)[:300]
        log.exception("会话异常 %s", key[:8])
        if isinstance(e, websockets.ConnectionClosed):
            ds_broken = True
        try:
            await _dev_send(ws, key, {"t": "err", "x": str(e)[:200]})
        except Exception:
            pass
    finally:
        elapsed = time.time() - t_start
        dur = audio_bytes / (SAMPLE_RATE * 2) if audio_bytes else 0.0
        STATS["sessions_active"] -= 1
        STATS["audio_seconds_total"] += dur
        if dur > 0:
            _bump_usage(dur)
        log.info("释放 %s 用时%.1fs 音频%.1fs", key[:8], elapsed, dur)
        log.info("音频体检 %s %s", key[:8], probe.report())
        _rec_close(rec_fh, rec_path)
        # 设备掉线/断流时 downlink 被取消，句尾定稿事件可能永远不来 ——
        # 把还没定稿的 partial 直接写进会议记录（设备已经断开，发也收不到，只保内容）
        try:
            orphan = [(sid, txt) for sid, txt in pending.items()
                      if sid not in committed and txt.strip()]
            for sid, txt in sorted(orphan, key=lambda kv: (kv[0] is None, kv[0])):
                mt.add_sentence(mrec, txt, None, None, sid)
                log.info("断线补记 %s #%s %s", key[:8], sid, txt)
        except Exception:
            pass
        # 异常退出（设备掉线/断流）也要把已识别的内容存下来，别整场丢掉
        try:
            persist_meeting()
        except Exception:
            pass
        # 录音已关、断线内容已补记 —— 现在才轮到会后精转和纪要。
        # 顺序必须是「先精转（整段重转 + 说话人分离）再生成纪要」，否则纪要还在用流式残句。
        # 精转要拉公网 URL，文件不完整会读到残包，所以不能提前到 done 事件里做。
        try:
            if mrec.get("sentences") or mrec.get("audio"):
                mrec["minutes_status"] = "pending"
                mt.save(mrec)
                asyncio.get_running_loop().create_task(
                    asyncio.to_thread(mt.finalize, mrec))
        except Exception:
            log.exception("精转/纪要启动失败 %s", key[:8])
        _send_locks.pop(key, None)
        # 异常退出的会话：task 还挂在百炼侧，补发 finish-task 收尾。
        # 不然热连接上积一个僵尸 task，下个会话的 run-task 会被拒（Invalid action）。
        if task_started and ds is not None and _ds_alive(ds):
            try:
                await ds.send(finish_task_msg)
            except Exception:
                ds_broken = True
        # 热连接不随会话关闭：还给连接池留给下个会话，省每次 1s+ 的 TLS 建连
        if ds is not None:
            await _release_dashscope(ds, ds_shared, broken=ds_broken)
        try:
            await ws.close()
        except Exception:
            pass


@app.on_event("startup")
async def _startup():
    if not API_KEY:
        log.error("缺少 DASHSCOPE_API_KEY，网关无法工作")
    if not DEVICE_TOKEN:
        log.warning("未配置 DEVICE_TOKEN，任何设备都可接入（公网暴露时不建议）")
    log.info("网关启动 model=%s port=%s", MODEL, os.getenv("GATEWAY_PORT", "8000"))
    if API_KEY:
        # 预热：开机先把热连接建好（不占用），第一个设备会话就不用等 TLS 握手
        async def _prewarm():
            global _ds_ws
            async with _ds_lock:
                if _ds_ws is None:
                    try:
                        t0 = time.time()
                        _ds_ws = await _connect_dashscope()
                        log.info("百炼上行预热建连 %.2fs", time.time() - t0)
                    except Exception as e:
                        log.warning("百炼预热失败（首个会话时会重试）: %s", e)
        asyncio.create_task(_prewarm())
        # 热词表：terms.txt 变了就自动重建，run-task 时带 vocabulary_id
        asyncio.create_task(_sync_hotwords())


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(
        app,
        host="0.0.0.0",
        port=int(os.getenv("GATEWAY_PORT", "8000")),
        log_level="info",
        ws_ping_interval=25,
        ws_ping_timeout=25,
    )
