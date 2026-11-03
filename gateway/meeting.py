"""会议记录：存储 / 纪要生成 / 手机端网页。

设计要点：
- 一场会议 = meetings/<id>.json 一个文件。id 是 8 位十六进制时间戳，可排序、够短。
- 音频沿用 recordings/<名>.wav，只把文件名记在 JSON 里，不搬文件。
- 纪要由大模型异步生成，页面在 pending 时自动刷新；生成失败不影响文字和录音。
- 网页鉴权用一段短 key 放在路径里（二维码要短，塞不下 32 位 token）。
"""
from __future__ import annotations

import html
import json
import logging
import os
import time
import urllib.request
from pathlib import Path

log = logging.getLogger("gw.meeting")

MEET_DIR = Path(os.getenv("MEETING_DIR", "meetings"))
REC_DIR = Path(os.getenv("RECORD_DIR", "recordings"))
# 手机端访问密钥：放在 URL 路径里，8 位足够（不对外暴露、也不是账号体系）
WEB_KEY = os.getenv("WEB_KEY", "")
# 二维码里要编的地址前缀，例如 http://gw.example.com/m
PUBLIC_BASE = os.getenv("PUBLIC_BASE", "").rstrip("/")
# 纪要用的模型。走 OpenAI 兼容协议，所以换供应商只改环境变量：
#   百炼   MINUTES_BASE_URL=https://dashscope.aliyuncs.com/compatible-mode/v1  MINUTES_MODEL=qwen-plus
#   DeepSeek MINUTES_BASE_URL=https://api.deepseek.com/v1  MINUTES_MODEL=deepseek-chat
#   智谱   MINUTES_BASE_URL=https://open.bigmodel.cn/api/paas/v4  MINUTES_MODEL=glm-4-flash
MINUTES_MODEL = os.getenv("MINUTES_MODEL", "qwen-plus")
MINUTES_BASE_URL = os.getenv(
    "MINUTES_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1").rstrip("/")
MINUTES_API_KEY = os.getenv("MINUTES_API_KEY") or os.getenv("DASHSCOPE_API_KEY", "")
CHAT_URL = MINUTES_BASE_URL + "/chat/completions"

MINUTES_PROMPT = """你是一名专业会议秘书。下面是某场会议的语音转写记录，请输出一份中文会议纪要。

要求：
1. 第一行用一句话概括会议主题（不加标题）
2. 「## 主要讨论」：分点列出讨论内容，可合并重复表述，保留关键数字与专有名词
3. 「## 结论」：分点列出明确达成的结论；没有就写"本次会议未形成明确结论"
4. 「## 待办」：分点列出行动项；尽量注明责任人与时间，没提到的写"未指定"
5. 只依据记录内容，不要编造。记录里明显听错或不完整的地方，如实保留原意即可，不要硬猜
6. 如果记录带「发言人N：」前缀，说明是谁提的就写谁；同一人多句可合并，不要逐句复述
7. 直接输出正文，不要用代码块包裹，不要写"以下是会议纪要"这类开场白"""

# ---------------------------------------------------------------- 会后精转
# 流式识别受"必须边听边出字"限制，用不上后文语境。录音既然已经落盘，
# 会后用离线大模型整段重跑一遍，顺带做说话人分离 —— 这是准确率唯一还能提的地方。
FILETRANS_MODEL = os.getenv("FILETRANS_MODEL",
                            "qwen-audio-3.1-asr-flash-filetrans")
FILETRANS_ON = os.getenv("FILETRANS_ENABLE", "1") != "0"
# 新版业务空间域名更稳；没配 workspace id 就退回通用域名（仍可用）
_WS = os.getenv("DASHSCOPE_WORKSPACE_ID", "")
DS_BASE = (f"https://{_WS}.cn-beijing.maas.aliyuncs.com" if _WS
           else "https://dashscope.aliyuncs.com").rstrip("/")
DS_KEY = os.getenv("DASHSCOPE_API_KEY", "")
# 说话人数参考值。不填让算法自己判断（2~100 才是合法值，填 1 会报错）
try:
    SPEAKER_HINT = int(os.getenv("SPEAKER_COUNT", "0"))
except ValueError:
    SPEAKER_HINT = 0


# ---------------------------------------------------------------- 存储

def new_id(ts: float | None = None) -> str:
    """8 位十六进制时间戳：唯一（同一秒不会开两场会）、可排序、够短。"""
    return "%08x" % int(ts if ts is not None else time.time())


def _path(mid: str) -> Path:
    return MEET_DIR / f"{mid}.json"


def _safe(mid: str) -> bool:
    return bool(mid) and len(mid) <= 16 and all(c in "0123456789abcdef" for c in mid)


def create(started_at: float | None = None) -> dict:
    ts = started_at if started_at is not None else time.time()
    return {
        "id": new_id(ts),
        "started_at": round(ts, 1),
        "duration": 0.0,
        "audio": "",
        "sentences": [],       # [{t,b,e,spk}]  spk 是 1 起的发言人编号，精转后才有
        "chars": 0,
        "minutes": "",
        "minutes_status": "",  # "" 未开始 / pending / done / failed
        "minutes_error": "",
        "refine_status": "",   # "" 未做 / pending / done / failed（会后精转）
        "refine_error": "",
        "speaker_count": 0,
    }


def add_sentence(rec: dict, text: str, b=None, e=None, sid=None) -> None:
    if not text:
        return
    rec["sentences"].append({"s": sid, "t": text, "b": b, "e": e})
    rec["chars"] += len(text)


def full_text(rec: dict) -> str:
    return "".join(s.get("t", "") for s in rec.get("sentences") or [])


def save(rec: dict) -> None:
    try:
        MEET_DIR.mkdir(parents=True, exist_ok=True)
        tmp = _path(rec["id"]).with_suffix(".json.tmp")
        tmp.write_text(json.dumps(rec, ensure_ascii=False), encoding="utf-8")
        tmp.replace(_path(rec["id"]))
    except Exception as e:
        log.warning("存会议失败 %s: %s", rec.get("id"), e)


def load(mid: str) -> dict | None:
    if not _safe(mid):
        return None
    p = _path(mid)
    if not p.exists():
        return None
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return None


def all_meetings() -> list[dict]:
    """按时间倒序。只读每个文件的前若干字段太麻烦，直接全读（量级很小）。"""
    out = []
    if not MEET_DIR.exists():
        return out
    for p in MEET_DIR.glob("*.json"):
        try:
            d = json.loads(p.read_text(encoding="utf-8"))
        except Exception:
            continue
        out.append({
            "id": d.get("id") or p.stem,
            "started_at": d.get("started_at", 0),
            "duration": d.get("duration", 0),
            "chars": d.get("chars", 0),
            "minutes_status": d.get("minutes_status", ""),
            "refine_status": d.get("refine_status", ""),
            "speaker_count": d.get("speaker_count", 0),
            "sentences": len(d.get("sentences") or []),
        })
    out.sort(key=lambda x: x.get("started_at", 0), reverse=True)
    return out


def audio_path(rec: dict) -> Path | None:
    name = rec.get("audio") or ""
    if not name:
        return None
    p = REC_DIR / name
    return p if p.exists() else None


def meeting_url(rec: dict) -> str:
    if not PUBLIC_BASE or not WEB_KEY:
        return ""
    return f"{PUBLIC_BASE}/{WEB_KEY}/{rec['id']}"


def list_url(key: str) -> str:
    return f"{PUBLIC_BASE}/{key}" if PUBLIC_BASE else "/m"


# ---------------------------------------------------------------- 纪要

def _chat(messages: list[dict]) -> str:
    body = json.dumps({
        "model": MINUTES_MODEL,
        "messages": messages,
        "temperature": 0.3,
    }, ensure_ascii=False).encode()
    req = urllib.request.Request(CHAT_URL, data=body, headers={
        "Authorization": f"Bearer {MINUTES_API_KEY}",
        "Content-Type": "application/json",
    }, method="POST")
    with urllib.request.urlopen(req, timeout=120) as r:
        d = json.loads(r.read().decode())
    return (d["choices"][0]["message"]["content"] or "").strip()


# ---- 兜底：没有可用大模型时的本地抽取式摘要 ----
# 不假装是"大模型纪要"：只按句子挑要点，页面上也会标注来源。
_TODO_WORDS = ("要", "需要", "得", "安排", "负责", "跟进", "推进", "落实", "准备",
               "下周", "明天", "后天", "之后", "回头", "再对", "确认", "整理")
_CONC_WORDS = ("所以", "因此", "结论", "总而言之", "总的来说", "定下来", "定了",
               "确定", "就是", "说明", "意味着", "归结")


def local_summary(rec: dict) -> str:
    sents = [(s.get("t") or "").strip() for s in rec.get("sentences") or []]
    sents = [s for s in sents if len(s) >= 4]
    if not sents:
        return ""
    # 去掉完全重复的句子（识别偶发重复很常见）
    seen, uniq = set(), []
    for s in sents:
        k = s.replace(" ", "")
        if k in seen:
            continue
        seen.add(k)
        uniq.append(s)

    topic = uniq[0].rstrip("。，、 ")
    topic = topic[:40] + ("…" if len(topic) > 40 else "")

    def pick(words, limit, maxlen=56):
        out, used = [], set()
        for s in uniq:
            if not any(w in s for w in words):
                continue
            if s in used:
                continue
            used.add(s)
            out.append("- " + (s if len(s) <= maxlen else s[:maxlen] + "…"))
            if len(out) >= limit:
                break
        return out

    # 讨论：按长度优先，信息密度更高
    body = sorted(uniq[1:], key=len, reverse=True)[:6]
    body = ["- " + (s if len(s) <= 56 else s[:56] + "…") for s in body] or ["- （记录过短）"]
    todo = pick(_TODO_WORDS, 5)
    conc = pick(_CONC_WORDS, 4)

    parts = [topic, "", "## 主要讨论"] + body
    parts += ["", "## 结论"] + (conc or ["- 本次会议未形成明确结论"])
    if todo:
        parts += ["", "## 待办"] + todo
    return "\n".join(parts)


def generate_minutes(rec: dict) -> None:
    """同步调用（由 asyncio.to_thread 包起来跑，别阻塞事件循环）。"""
    text = dialogue_text(rec)
    if not text.strip():
        rec["minutes_status"] = "failed"
        rec["minutes_error"] = "本次会议没有识别到文字"
        save(rec)
        return
    rec["minutes_status"] = "pending"
    save(rec)
    try:
        t0 = time.time()
        out = _chat([
            {"role": "system", "content": MINUTES_PROMPT},
            {"role": "user", "content": text[:12000]},
        ])
        rec["minutes"] = out
        rec["minutes_status"] = "done"
        rec["minutes_mode"] = "llm"
        log.info("纪要生成完成 %s %.1fs %d字 model=%s", rec["id"], time.time() - t0, len(out), MINUTES_MODEL)
    except Exception as e:
        # 大模型不可用（欠费/限流/网络）不该让这一页空着：退回本地抽取，页面会标注来源
        rec["minutes"] = local_summary(rec)
        rec["minutes_status"] = "done"
        rec["minutes_mode"] = "local"
        rec["minutes_error"] = f"大模型不可用（{str(e)[:80]}），以下为本地要点抽取"
        log.warning("纪要改走本地抽取 %s: %s", rec["id"], e)
    save(rec)


# ---------------------------------------------------------------- 会后精转

def audio_url(rec: dict) -> str:
    """录音的公网地址。百炼要自己来拉，所以必须是外网可达的绝对 URL。"""
    if not PUBLIC_BASE or not WEB_KEY or not rec.get("audio"):
        return ""
    return f"{PUBLIC_BASE}/{WEB_KEY}/{rec['id']}/audio"


def _ds_call(path: str, body: dict | None = None) -> dict:
    url = DS_BASE + path
    data = json.dumps(body).encode() if body is not None else None
    hdr = {"Authorization": f"Bearer {DS_KEY}"}
    if body is not None:
        hdr["Content-Type"] = "application/json"
        hdr["X-DashScope-Async"] = "enable"
    req = urllib.request.Request(url, data=data, headers=hdr,
                                 method="POST" if body is not None else "GET")
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode())


def _load_vocab() -> dict:
    """复用流式那套热词，省得两份词表各说各话。"""
    try:
        p = Path(os.getenv("VOCAB_FILE", "vocab.json"))
        if p.exists():
            d = json.loads(p.read_text(encoding="utf-8"))
            return d if isinstance(d, dict) else {}
    except Exception:
        pass
    return {}


def refine(rec: dict) -> bool:
    """整段录音重转 + 说话人分离。同步实现，调用方负责丢线程。"""
    if not FILETRANS_ON or not DS_KEY:
        return False
    url = audio_url(rec)
    if not url:
        rec["refine_status"] = "failed"
        rec["refine_error"] = "没有可用的公网录音地址"
        save(rec)
        return False

    rec["refine_status"] = "pending"
    save(rec)
    t0 = time.time()
    tid = ""
    try:
        params = {
            "channel_id": [0],
            "language_hints": ["zh", "en"],
            "diarization_enabled": True,
        }
        if SPEAKER_HINT >= 2:
            params["speaker_count"] = SPEAKER_HINT
        vocab = _load_vocab()
        if vocab:
            params["vocabulary"] = vocab

        r = _ds_call("/api/v1/services/audio/asr/transcription", {
            "model": FILETRANS_MODEL,
            "input": {"file_urls": [url]},
            # 注意：新版域名下 parameters 必传，即使空对象也要给，否则任务会静默失败
            "parameters": params,
        })
        tid = ((r.get("output") or {}).get("task_id")) or ""
        if not tid:
            raise RuntimeError(f"没拿到 task_id: {str(r)[:160]}")

        out, status = {}, "PENDING"
        for _ in range(150):          # 最多等 5 分钟，长会议够用
            time.sleep(2)
            out = _ds_call(f"/api/v1/tasks/{tid}").get("output") or {}
            status = out.get("task_status")
            if status in ("SUCCEEDED", "FAILED"):
                break
        if status != "SUCCEEDED":
            raise RuntimeError(f"任务 {status}: {str(out)[:160]}")

        results = out.get("results") or []
        if not results or results[0].get("subtask_status") != "SUCCEEDED":
            raise RuntimeError(f"子任务失败: {str(results)[:160]}")
        with urllib.request.urlopen(results[0]["transcription_url"], timeout=60) as rr:
            res = json.loads(rr.read().decode())
        raw_sents = ((res.get("transcripts") or [{}])[0].get("sentences")) or []

        new, order = [], {}
        for s in raw_sents:
            txt = (s.get("text") or "").strip()
            if not txt:
                continue
            sid = s.get("speaker_id")
            if sid not in order:
                order[sid] = len(order) + 1   # 按首次出现顺序编号，比 0/1/2 好读
            new.append({
                "t": txt,
                "b": s.get("begin_time"),
                "e": s.get("end_time"),
                "spk": order[sid],
            })
        if not new:
            raise RuntimeError("精转没有返回任何文字")

        rec["sentences_stream"] = [dict(s) for s in rec.get("sentences") or []]
        rec["sentences"] = new
        rec["chars"] = sum(len(x["t"]) for x in new)
        rec["speaker_count"] = len(order)
        rec["refine_status"] = "done"
        rec["refine_mode"] = FILETRANS_MODEL
        rec["refine_error"] = ""
        log.info("精转完成 %s %.1fs %d句 %d人", rec["id"], time.time() - t0,
                 len(new), len(order))
        save(rec)
        return True
    except Exception as e:
        rec["refine_status"] = "failed"
        rec["refine_error"] = str(e)[:200]
        save(rec)
        log.warning("精转失败 %s: %s", rec.get("id"), e)
        return False


def dialogue_text(rec: dict) -> str:
    """喂给纪要模型的文本：有发言人就带前缀，没有就是纯流水句。"""
    out = []
    for s in rec.get("sentences") or []:
        t = (s.get("t") or "").strip()
        if not t:
            continue
        spk = s.get("spk")
        out.append(f"发言人{spk}：{t}" if spk else t)
    return "\n".join(out)


def finalize(rec: dict) -> None:
    """会议收尾唯一入口：先精转（拿说话人），再生成纪要。跑在线程里。

    精转失败不影响纪要 —— 退回用流式文本，页面上会标注是哪一种。
    """
    if rec.get("audio") and rec.get("sentences"):
        refine(rec)
    generate_minutes(rec)


# ---------------------------------------------------------------- 网页

_CSS = """
*{box-sizing:border-box;-webkit-text-size-adjust:100%}
body{margin:0;padding:0 0 40px;background:#f4f5f7;color:#1a1a1f;
 font:16px/1.7 -apple-system,BlinkMacSystemFont,"PingFang SC","Hiragino Sans GB","Microsoft YaHei",sans-serif}
.wrap{max-width:720px;margin:0 auto;padding:0 14px}
a{color:#1677ff;text-decoration:none}
header{position:sticky;top:0;z-index:9;background:rgba(244,245,247,.92);
 backdrop-filter:saturate(180%) blur(12px);border-bottom:1px solid #e6e8eb}
.hd{display:flex;align-items:center;gap:10px;height:52px}
.hd .back{font-size:15px;color:#1677ff;flex:none}
.hd h1{margin:0;font-size:16px;font-weight:600;flex:1;
 overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.card{background:#fff;border-radius:14px;padding:16px;margin:12px 0;
 box-shadow:0 1px 2px rgba(16,24,40,.05),0 1px 3px rgba(16,24,40,.04)}
.meta{display:flex;flex-wrap:wrap;gap:8px;margin:14px 0 2px;font-size:13px;color:#6b7280}
.meta span{background:#eef1f5;border-radius:7px;padding:2px 8px}
h2{font-size:15px;font-weight:600;margin:0 0 10px;display:flex;align-items:center;gap:7px}
h2::before{content:"";width:3px;height:14px;border-radius:2px;background:#1677ff}
.md p{margin:0 0 10px}
.md h2{font-size:15px;margin:18px 0 8px;padding:0}
.md h2::before{display:none}
.md ul,.md ol{margin:0 0 10px;padding-left:22px}
.md li{margin:3px 0}
.md strong{color:#111}
.tx p{margin:0 0 14px;padding-left:0}
.tx .ts{display:inline-block;font-variant-numeric:tabular-nums;font-size:12px;
 color:#8a929e;background:#f0f2f5;border-radius:5px;padding:1px 6px;margin-right:7px}
.tx .spk{display:inline-block;font-size:12px;font-weight:600;border-radius:5px;
 padding:1px 7px;margin-right:6px;vertical-align:1px}
.s1{background:#e8f0fe;color:#1a56db}
.s2{background:#e6f7ee;color:#0d7a43}
.s3{background:#fdf0e6;color:#a15c00}
.s4{background:#f3e8fd;color:#6b21a8}
.s5{background:#fde8ef;color:#9d174d}
.spkline{margin:0 0 13px}
.spkline p{margin:2px 0 0}
.spkline .ts{margin-right:0}
.audio audio{width:100%;margin:6px 0 10px}
.btn{display:flex;align-items:center;justify-content:center;gap:7px;height:44px;
 border-radius:11px;background:#1677ff;color:#fff;font-weight:500}
.item{display:block;background:#fff;border-radius:14px;padding:14px 16px;margin:10px 0;
 box-shadow:0 1px 2px rgba(16,24,40,.05)}
.item .t{font-size:16px;font-weight:600;color:#1a1a1f}
.item .s{font-size:13px;color:#6b7280;margin-top:5px;display:flex;gap:12px;flex-wrap:wrap}
.badge{font-size:12px;border-radius:5px;padding:1px 7px}
.b-pend{background:#fff4e5;color:#a15c00}
.b-ok{background:#e8f5ec;color:#1a7f37}
.b-done{background:#eef1f5;color:#6b7280}
.empty{text-align:center;color:#8a929e;padding:60px 20px;font-size:15px}
.tip{font-size:13px;color:#8a929e;margin-top:10px;text-align:center}
"""


def _e(s) -> str:
    return html.escape(str(s if s is not None else ""))


def _fmt_time(ts: float) -> str:
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(ts))


def _fmt_dur(sec: float) -> str:
    sec = int(sec or 0)
    if sec < 60:
        return f"{sec} 秒"
    m, s = divmod(sec, 60)
    if m < 60:
        return f"{m} 分 {s} 秒" if s else f"{m} 分钟"
    h, m = divmod(m, 60)
    return f"{h} 小时 {m} 分"


def _ts(ms) -> str:
    try:
        ms = int(ms)
    except Exception:
        return ""
    if ms < 0:
        ms = 0
    m, s = divmod(ms // 1000, 60)
    return f"{m}:{s:02d}"


def _md_lite(text: str) -> str:
    """够用的 Markdown 子集：标题/列表/加粗。不引三方库，避免喂给模型的内容反过来注入 HTML。"""
    out, in_ul = [], False
    for raw in (text or "").splitlines():
        line = raw.rstrip()
        if line.startswith("### "):
            line, tag = line[4:], "h3"
        elif line.startswith("## "):
            line, tag = line[3:], "h2"
        elif line.startswith("# "):
            line, tag = line[2:], "h2"
        else:
            tag = ""
        stripped = line.lstrip()
        is_li = stripped[:2] in ("- ", "* ", "· ") or (
            len(stripped) > 2 and stripped[0].isdigit() and stripped[1:3] in (". ", "、")
        )
        if is_li:
            if not in_ul:
                out.append("<ul>")
                in_ul = True
            body = stripped[2:] if stripped[:2] in ("- ", "* ", "· ") else stripped[2:]
            out.append(f"<li>{_inline(body)}</li>")
            continue
        if in_ul:
            out.append("</ul>")
            in_ul = False
        if not line.strip():
            continue
        if tag:
            out.append(f"<{tag}>{_inline(line)}</{tag}>")
        else:
            out.append(f"<p>{_inline(line)}</p>")
    if in_ul:
        out.append("</ul>")
    return "".join(out)


def _inline(s: str) -> str:
    s = _e(s)
    while "**" in s:
        i = s.find("**")
        j = s.find("**", i + 2)
        if j < 0:
            break
        s = s[:i] + "<strong>" + s[i + 2:j] + "</strong>" + s[j + 2:]
    return s


def _page(title: str, body: str) -> str:
    return (
        "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
        f"<title>{_e(title)}</title><style>{_CSS}</style></head><body>{body}</body></html>"
    )


def render_list(key: str) -> str:
    items = all_meetings()
    if not items:
        body = ("<header><div class=\"wrap hd\"><h1>会议记录</h1></div></header>"
                "<div class=\"wrap\"><div class=\"empty\">还没有会议记录<br>在机器人上录一场试试</div></div>")
        return _page("会议记录", body)

    rows = []
    for m in items:
        st = m.get("minutes_status")
        if st == "done":
            badge = '<span class="badge b-ok">纪要已生成</span>'
        elif st == "pending":
            badge = '<span class="badge b-pend">纪要生成中</span>'
        elif st == "failed":
            badge = '<span class="badge b-pend">纪要失败</span>'
        else:
            badge = '<span class="badge b-done">仅文字</span>'
        rs = m.get("refine_status")
        if rs == "pending":
            badge += '<span class="badge b-pend">精转中</span>'
        elif rs == "done" and m.get("speaker_count"):
            badge += f'<span class="badge b-done">{m["speaker_count"]} 人</span>'
        rows.append(
            f'<a class="item" href="/m/{_e(key)}/{_e(m["id"])}">'
            f'<div class="t">{_fmt_time(m.get("started_at", 0))}</div>'
            f'<div class="s"><span>{_fmt_dur(m.get("duration", 0))}</span>'
            f'<span>{m.get("sentences", 0)} 句 / {m.get("chars", 0)} 字</span>{badge}</div>'
            f'</a>'
        )
    body = (
        "<header><div class=\"wrap hd\"><h1>会议记录</h1>"
        f"<span style=\"font-size:13px;color:#8a929e\">{len(items)} 场</span></div></header>"
        f"<div class=\"wrap\">{''.join(rows)}</div>"
    )
    return _page("会议记录", body)


def render_detail(key: str, rec: dict) -> str:
    mid = rec["id"]
    title = _fmt_time(rec.get("started_at", 0))

    # 纪要区
    st = rec.get("minutes_status") or ""
    if st == "done" and rec.get("minutes"):
        note = ""
        if rec.get("minutes_mode") == "local":
            note = (f'<div class="tip" style="text-align:left;margin:0 0 10px">'
                    f'{_e(rec.get("minutes_error") or "本地要点抽取")}</div>')
        minutes_html = (f'<div class="card"><h2>会议纪要</h2>{note}'
                        f'<div class="md">{_md_lite(rec["minutes"])}</div></div>')
    elif st == "pending":
        minutes_html = ('<div class="card"><h2>会议纪要</h2>'
                        '<div style="color:#8a929e;font-size:15px">正在生成，请稍候…</div>'
                        '<div class="tip">本页 4 秒后自动刷新</div></div>')
    else:
        err = rec.get("minutes_error") or ""
        minutes_html = ('<div class="card"><h2>会议纪要</h2>'
                        f'<div style="color:#8a929e;font-size:15px">暂时没有纪要</div>'
                        + (f'<div class="tip">{_e(err)}</div>' if err else "")
                        + "</div>")

    # 文字记录区
    sents = rec.get("sentences") or []
    if sents:
        ps = []
        for s in sents:
            ts = _ts(s.get("b"))
            chip = f'<span class="ts">{ts}</span>' if ts else ""
            spk = s.get("spk")
            if spk:
                cls = f"s{(int(spk) - 1) % 5 + 1}"
                tag = f'<span class="spk {cls}">发言人{_e(spk)}</span>'
                ps.append(f'<div class="spkline">{tag}{chip}'
                          f'<p>{_e(s.get("t", ""))}</p></div>')
            else:
                ps.append(f'<p>{chip}{_e(s.get("t", ""))}</p>')
        cnt = rec.get("speaker_count") or 0
        if cnt:
            side = (f'<span style="margin-left:auto;font-size:12px;color:#8a929e;'
                    f'font-weight:400">{cnt} 位发言人</span>')
        else:
            side = ""
        note = ""
        if rec.get("refine_status") == "pending":
            note = ('<div class="tip" style="text-align:left;margin:0 0 12px">'
                    '正在做会后精转与说话人分离，稍后本页会更新</div>')
        elif rec.get("refine_status") == "failed":
            note = (f'<div class="tip" style="text-align:left;margin:0 0 12px">'
                    f'精转未成功，以下为实时识别结果'
                    f'（{_e(rec.get("refine_error") or "")}）</div>')
        elif rec.get("refine_status") == "done":
            note = ('<div class="tip" style="text-align:left;margin:0 0 12px">'
                    '已用录音整段重转，句子边界与发言人为离线结果</div>')
        text_html = (f'<div class="card"><h2>文字记录{side}</h2>{note}'
                     f'<div class="tx">{"".join(ps)}</div></div>')
    else:
        text_html = ('<div class="card"><h2>文字记录</h2>'
                     '<div style="color:#8a929e;font-size:15px">本次没有识别到文字</div></div>')

    # 录音区
    if audio_path(rec):
        au = f"/m/{key}/{mid}/audio"
        audio_html = (
            f'<div class="card"><h2>录音</h2>'
            f'<div class="audio"><audio controls preload="none" src="{au}"></audio></div>'
            f'<a class="btn" href="{au}?dl=1">下载录音文件</a>'
            f'<div class="tip">{_fmt_dur(rec.get("duration", 0))} · 16kHz 单声道 WAV</div></div>'
        )
    else:
        audio_html = ('<div class="card"><h2>录音</h2>'
                      '<div style="color:#8a929e;font-size:15px">本次没有录音文件</div></div>')

    body = (
        f'<header><div class="wrap hd"><a class="back" href="/m/{_e(key)}">‹ 记录</a>'
        f'<h1>{title}</h1></div></header>'
        f'<div class="wrap">'
        f'<div class="meta"><span>{_fmt_time(rec.get("started_at", 0))}</span>'
        f'<span>{_fmt_dur(rec.get("duration", 0))}</span>'
        f'<span>{len(sents)} 句 / {rec.get("chars", 0)} 字</span>'
        + (f'<span>{rec.get("speaker_count")} 人</span>'
           if rec.get("speaker_count") else "")
        + '</div>'
        f'{minutes_html}{text_html}{audio_html}'
        f'<div class="tip">Chan 会议记录</div>'
        f'</div>'
    )
    if st == "pending":
        body = '<meta http-equiv="refresh" content="4">' + body
    return _page(title, body)


def render_404() -> str:
    return _page("找不到", '<div class="wrap"><div class="empty">页面不存在或链接已失效</div></div>')
