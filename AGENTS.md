# AGENTS.md — StackChan

给任何在本目录工作的 AI 任务读的约束。**先读完再动手。**

## 这是什么

M5Stack CoreS3 会议记录机器人：设备录音 → WebSocket 上传 → 阿里云网关转写 →
百炼 ASR → 屏幕回显 → 散会出二维码 → 手机看纪要。

- 固件：`firmware/chan-cores3/`（**PlatformIO + Arduino**，`.ino`）
- 网关：`gateway/`（FastAPI，部署在阿里云 `/opt/meeting-gw`，systemd `meeting-gw`）
- ASR：百炼 `qwen-audio-3.0-asr-flash-streaming`（实时）+ `qwen-audio-3.1-asr-flash-filetrans`（会后精转）

## 环境铁律

**本项目用 PlatformIO，不用 ESP-IDF。** 两者在这台机器上是两套独立工具链，
别混着调（`~/.espressif` 是 AtomS3R 那摊的，跟这里无关）。

| 用途 | 命令 | 绝对路径 |
|---|---|---|
| 编译 / 烧录固件 | `make flash`（USB） | `/opt/homebrew/bin/pio` |
| OTA 烧录 | `make flash-ota` | 同上 |
| 串口监视 | `make monitor` | 同上 |
| 部署网关 | `make deploy` | ssh 别名 `aliyun` |
| 看最近录音 | `make recs` / `make rec` | — |
| 打开手机端页面 | `make web` | — |

环境有疑问先跑 `tc`，**但只跑 `tc` 不够 —— 要看能不能真跑，用 `tc --verify`**。
事实源在 `~/dev-env/TOOLCHAIN.md`。
**不要用裸 `python3` 跑脚本** —— 这台机器有 4 套 python，见 TOOLCHAIN.md。

### 编译挂了先查这条

`pio run` 报 `ModuleNotFoundError: No module named 'click'` 或其他 import 错误时，
不是缺包，是 **pio 的子进程找不到 `~/.platformio-pylib`**（寄生依赖，见 TOOLCHAIN.md §7）。
启动器 `/opt/homebrew/bin/pio` 已显式 `export PYTHONPATH`，正常不会复发；
若复发了，先看那个文件里的这行还在不在。

## 改代码前必读

- `docs/asr-accuracy.md` — ASR 分层排查手册 + 全部踩坑记录（§8 换模型复盘、§10 realtime A/B、§11 参考项目评估、§12 精转）
- 里面写"已实测否定"的方案，不要再试一遍，除非有新证据

## 硬性约束

1. **密钥不进代码**。`DASHSCOPE_API_KEY` 只在 `gateway/.env`（服务器上）和本地 `.env`，都在 `.gitignore` 里。
2. **不要注释掉报错来"通过"**。找到根因再改。
3. **录音是边收边写、结束才补 RIFF 头的**。任何要读 WAV 的逻辑（精转、分析）都必须等 `_rec_close()` 之后。
4. **改完必须自己验**：固件改动要能编译通过；网关改动要 `py_compile` + 重启后 `curl` 一次页面。
5. 提交前 `git status` 检查，别把 `recordings/` `meetings/` `usage.json` 带进去（已在 .gitignore，但别加回）。

## 已知遗留

- 动态范围差 14dB（远说 −33dBFS / 近说 −19.5dBFS）—— 硬件增益已到芯片上限，属物理限制
- 设备端 `noise_filter_level` 必须保持 0（是低通 IIR，开了会砍清辅音）
- `magnification` 提高没用（纯数字增益，只放大底噪）
