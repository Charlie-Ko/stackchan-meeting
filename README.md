# StackChan · 桌面会议记录机器人

M5Stack StackChan（CoreS3）桌面机器人 + 阿里云 ECS 网关 + 百炼实时语音识别。
按一下开口说话，屏幕实时滚出文字；会后可接分人与纪要。

```
[StackChan / StickS3]
  麦克风 16k PCM
      │  WebSocket 二进制帧（50ms/帧）
      ▼
[阿里云 ECS  nginx :80  →  127.0.0.1:8000 网关]
      │  透传 + 会话管理 + 去重
      ▼
[百炼 paraformer-realtime-v2]
      │  sentence 事件
      ▼
  定稿句回推 → 屏幕滚动显示
```

## 目录结构

| 路径 | 说明 |
|---|---|
| `firmware/chan-cores3/` | **主固件**，StackChan / CoreS3，PlatformIO + Arduino |
| `firmware/stick-s3-poc/` | Stick S3 便携版 P0 原型（验证用，未产品化） |
| `gateway/app.py` | ECS 网关，FastAPI + WebSocket 代理，对接百炼 |
| `gateway/nginx-meeting-gw.conf` | nginx 反代（80 → 8000，含 WebSocket Upgrade） |
| `gateway/legacy/asr-gateway-p0.py` | P0 遗留 Flask 网关（走阿里云 ISI，已被百炼取代，仅存档） |
| `scripts/dashscope_asr_verify.py` | 百炼 ASR 直连验证（跳过网关，排查用） |
| `scripts/vad_sim.py` | **VAD 离线验证**，把 `vad.h` 的算法搬成 Python 跑 5 个合成场景 |
| `scripts/mock_device.py` | 模拟设备推流，验证网关端到端 |
| `scripts/feishu_verify.py` | 飞书凭证与接口连通性验证 |
| `assets/terms.txt` | 专有名词词表模板，拷到 SD 卡根目录即用 |
| `docs/design.md` | **约束先行文档**：架构决策、选型对比、待验证清单 |
| `docs/journal/` | 开发日志（按日归档，含踩坑记录） |
| `var/usage.json` | 网关运行时用量统计（不纳入版本控制） |

## 快速上手

```bash
make            # 看所有可用命令
make build      # 编译主固件
make flash      # 烧录（自动探测串口，或 make flash PORT=/dev/cu.usbmodem1101）
make flash-ota IP=192.168.1.23   # 无线烧录（设备已联网）
make monitor    # 串口监视器
```

### 无线烧录（OTA）

固件内置 ArduinoOTA（主机名 `chan-cores3`，端口 3232，口令见 `config.h` 的 `OTA_PASSWORD`）。

```bash
make flash-ota IP=192.168.1.23     # 屏幕顶部显示的就是设备 IP
make flash-ota IP=chan-cores3.local  # 或者用 mDNS，前提是路由器支持
```

**前提：设备上已经在跑带 OTA 的固件** —— OTA 是"板上接收端 + 电脑发送端"两半，
接收端本身只能靠 USB 装进去。所以第一次永远 USB，第二次起才不用碰线。
升级期间屏幕会显示百分比，完成后自动重启。

### 手动烧固件（StackChan / CoreS3）

1. 装 **M5Unified ≥ 0.2.12**、**WebSockets (links2004) ≥ 2.6.1**、**ArduinoJson ≥ 7**
2. Arduino IDE 板卡选 **M5Stack CoreS3**；或 `cd firmware/chan-cores3 && pio run -t upload`
3. **烧完不需要改代码**：首次开机自动开热点 `Chan-XXXX`（密码 `changeme88`），
   手机连上后打开 `192.168.4.1` 填 WiFi，点「保存并重启」。
   WiFi、网关、令牌、专有名词全部存 NVS，以后改配置都走这个网页。

> `config.h` 里只剩**出厂默认值**（网关地址、令牌、UI 尺寸、舵机引脚等）。
> 除非要改"恢复出厂后的默认"，否则不用碰它。

状态栏：左「录音中 + 计时 + 红点」，右「WiFi + 电量」。
正文：**白色=已定稿，灰色=当前句正在识别**；命中词表的词**加粗 + 青绿**。

| 界面 | 按钮 |
|---|---|
| 主界面 | 开始录音 / 设置（长按 2.5 秒直进配网） |
| 录音中 | 暂停 / 结束 |
| 暂停中 | 继续 / 结束 |
| 总结页 | 新建会议 / 重启设备 |
| 设置页 | 重新配网 / 重启 / 返回，点「舵机」整行切开关 |

## 舵机开关（默认关）

出厂默认**关闭**：不开电源、不初始化串口、不发一个字节，绝对静音。

打开方式：设置页点「舵机」那一行 → 写 NVS → 立即生效。

三层静音保障（就算开着也不会污染录音）：

1. 总开关关闭 → 全链路不初始化
2. **录音期间 `lock()`：先卸扭矩，再断 VM_EN 电源** → 舵机不带电，本底噪声归零
3. 只有待机 / 结束页才 `unlock()` 慢慢回中位

| 项 | 值 |
|---|---|
| 舵机型号 | SCS0009 ×2（yaw 可 360°，pitch 0~90°） |
| 总线 | UART1 @1 Mbps，TX=GPIO6 / RX=GPIO7 |
| 舵机 ID | yaw=1，pitch=2 |
| 电源 | PY32 IO 扩展器 I2C 0x6F，pin0 = VM_EN |
| 零点 | yaw=460，pitch=620（原始步 0~1000） |

## 本地 VAD（静音不上传）

录音时设备端做能量 VAD，静音帧直接丢掉，不占上行带宽、不烧百炼额度。

**关键：只决定"这一帧发不发"，不改变会话状态** —— 静音不断流、不发 `stop`。

三条安全网防吞字：

| 机制 | 默认值 | 作用 |
|---|---|---|
| 预滚缓冲 | 400ms（8 帧） | 人声起始时补发，句首辅音不被切掉 |
| 预热校准 | 1s | 开头全量上传，同时用最低电平校准环境噪声底 |
| 超时放行 | 8s | 连续抑制太久说明噪声底估错了，自动降 6dB 并放行 |

阈值跟着噪声底浮动（`VAD_ON_MARGIN_DB` / `VAD_OFF_MARGIN_DB`），换会议室不用改代码。
参数全在 `config.h` 的 VAD 段；`VAD_DEBUG=1` 时串口会打 `[VAD] ↑起/↓止`，主要看 `nf=` 是否接近真实底噪。

改完阈值先跑离线验证（不需要硬件）：

```bash
python3 scripts/vad_sim.py
```

## 专有名词加粗

ASR 对人名和内部黑话最容易同音错字，屏上把已知词加粗变色，一眼能核对。

三级来源，优先级从低到高：

| 优先级 | 来源 | 怎么改 |
|---|---|---|
| 1 | 内置兜底（`terms.h` 里 `kBuiltin`） | 改代码、重烧 |
| 2 | SD 卡 `/terms.txt` | 拔卡拷文件，插卡开机自动加载 |
| 3 | **配网网页「专有名词」** | 手机粘贴，存 NVS，最方便 |

格式：一行一个词，逗号分隔别名，`#` 开头是注释，最多 96 条、每条 ≤8 个汉字。

```
前端,前端开发
后端
评审,评审会
张工
```

内置词表按**长度降序**匹配，所以「前端开发」不会先命中「前端」。
折行时专有名词整体不拆行，避免「前」在上一行、「端」在下一行导致加粗失效。

## 网关（已部署在 ECS）

```bash
# 代码
/opt/meeting-gw/app.py
/opt/meeting-gw/.env          # 600 权限，含 DASHSCOPE_API_KEY / DEVICE_TOKEN

# 服务
systemctl status meeting-gw
journalctl -u meeting-gw -f
```

接口：

| 路径 | 用途 |
|---|---|
| `GET /asr/health` | 健康检查 + 当月用量 |
| `GET /asr/usage` | 按月累计音频秒数 |
| `WS  /asr/ws/device?token=xxx` | 设备接入 |

部署/更新：

```bash
make deploy          # = scp app.py + systemctl restart
make deploy-nginx    # 更新 nginx 配置并 reload
```

## 验证

```bash
# 健康
curl http://gw.example.com/asr/health

# 直连百炼（不经过网关）
python scripts/dashscope_asr_verify.py /tmp/asr_test.wav

# 模拟设备走公网网关
python scripts/mock_device.py \
  --url ws://gw.example.com/asr/ws/device \
  --token <DEVICE_TOKEN> --wav /tmp/asr_test.wav
```

生成测试音频（macOS）：

```bash
say -v Tingting "这是一段用于测试的会议发言，我们今天讨论三个议题。" -o /tmp/a.aiff
afconvert -f WAVE -d LEI16@16000 -c 1 /tmp/a.aiff /tmp/asr_test.wav
```

## 设备协议

上行文本帧：
- `{"t":"ping"}` → 回 `{"t":"pong"}`
- `{"t":"stop"}` → 结束会话，等百炼吐完最后一句

下行：
```json
{"t":"ready"}
{"t":"p","s":1,"x":"当前句"}                  // 实时，累积式整句
{"t":"f","s":1,"x":"定稿","b":0,"e":5280}     // 定稿
{"t":"done","q":"http://<host>/m/<key>/<id>"} // 结束；q 是手机端链接，设备拿去画二维码
{"t":"err","x":"..."}
```

二进制帧：16kHz / 16bit / 单声道 PCM，建议每帧 1600 或 3200 字节（50/100ms）。

## 手机端会议记录（扫二维码）

会议结束后，结束页会显示一个二维码。手机扫开是一个网页，三块内容：

| 区块 | 说明 |
|---|---|
| 会议纪要 | 大模型（默认 `qwen-plus`）根据转写文本生成，Markdown |
| 文字记录 | 逐句转写，带时间戳 |
| 录音 | 浏览器内直接播放 + 下载 16k 单声道 WAV |

页面顶部的「‹ 记录」回到历史列表，按时间倒序，每场都能点进去看同样三块。

**实现要点**
- 一场会议一个文件：`meetings/<id>.json`（id 是 8 位十六进制时间戳），录音仍在 `recordings/`，只记文件名。
- 纪要异步生成，页面在 `pending` 时每 4 秒自动刷新，不阻塞设备收尾。
- 大模型不可用（欠费/限流/断网）时自动退回**本地抽取式摘要**，页面上会标明来源，不会白页。
- 鉴权靠 URL 里一段 8 位 key（二维码要短，塞不下 32 位 token）：
  - `/m/<key>` 列表
  - `/m/<key>/<id>` 详情
- 换纪要模型只改环境变量（走 OpenAI 兼容协议）：
  ```bash
  MINUTES_BASE_URL=https://api.deepseek.com/v1   # 默认百炼 compatible-mode
  MINUTES_MODEL=deepseek-chat                     # 默认 qwen-plus
  MINUTES_API_KEY=sk-xxx                          # 不填则用 DASHSCOPE_API_KEY
  ```

**网关环境变量（`/opt/meeting-gw/.env`）**
```bash
WEB_KEY=<你的访问密钥>                       # 手机端访问密钥，也是二维码里的那一段
PUBLIC_BASE=http://gw.example.com/m           # 二维码地址前缀，nginx 需把 /m/ 反代到 8000
MEETING_DIR=/opt/meeting-gw/meetings
```

改完 `make deploy`（会一起传 `app.py` 和 `meeting.py`）+ `make deploy-nginx`（新增 `/m/` 反代）。

**本机私有配置（不进仓库）**

地址和令牌不写在代码里，仓库里只留模板。拿到项目后先建两份文件：

```bash
cp secrets.mk.example secrets.mk        # make 用的网关地址 / 设备令牌 / 手机端密钥
cp firmware/chan-cores3/config_secrets.h.example firmware/chan-cores3/config_secrets.h
```

- `secrets.mk`：`GW` / `TK` / `WEB_BASE` / `WEB_KEY`，`make` 自动 include，
  `make health` `make recs` `make web` 都靠它。
- `config_secrets.h`：出厂默认的设备令牌 `DEFAULT_GW_TOKEN`。
  `config.h` 用 `__has_include` 兜底，没有这份文件也能编译（令牌为空，靠配网页面填）。

这两份文件都在 `.gitignore` 里。**换机器、开源、外包之前先确认它们没被 `git add -f` 加进来。**

## 已知坑（都踩过）

1. **`sentence.text` 是累积式的**：同一句每次回传整句全量，直接拼会变复读机。必须按 `sentence_id` 覆盖，`sentence_end=true` 才定稿。
2. **CoreS3 麦克风与扬声器共用 I2S_NUM_1**（BCK=34, WS=33），录音前必须 `M5.Speaker.end()`，否则初始化失败或啸叫。
3. **停录后不能立刻断 WS**：要等百炼把最后一句吐完（网关已处理，等 10 秒）。
4. **飞书免费版不适合做 ASR**：妙记 300 分钟/月、API 1 万次/月，流式接口配额不放行。飞书只留作推送出口。
5. **ECS 安全组只放行 80/8643**，8000 公网不通，所以走 nginx 反代。
6. 百炼 API Key 是 `sk-` 开头，**不是 AccessKey**；通用域名 `dashscope.aliyuncs.com` 不需要 Workspace ID。
7. **本机编译必须关代理**：PlatformIO 源走代理慢到超时（代理 11.7s vs 直连 1.8s）。`make build` 已内置处理。

## 成本

| 项 | 单价 | 免费额度 |
|---|---|---|
| paraformer-realtime-v2 | 0.864 元/小时 | 每月 10 小时 |
| paraformer-v2（会后分人） | 0.288 元/小时 | 每月 10 小时 |

免费额度每月 1 日自动发放，当月不用作废。`/asr/health` 里的 `month_free_used_pct` 可监控。

## 进度

- [x] 实时链路打通（百炼 → 本地网关 → ECS → 公网端到端）
- [x] 设备固件（CoreS3 触摸开录、屏上实时滚字）
- [x] 本地 VAD 静音裁剪（`vad.h` + `scripts/vad_sim.py` 离线验证 5 场景全过）
- [ ] StackChan 实机烧录验证（麦克风增益、拾音距离、VAD 阈值现场标定）
- [ ] **会后音频落盘**（网关转发同时写 WAV，否则 P2 分人无料可用）
- [ ] SD 卡分段备份（断网补传）
- [ ] 会后：整段音频 → `paraformer-v2` 开 `diarization_enabled` 分人
- [ ] 纪要：DeepSeek 出结构化待办/决议
- [ ] 出口：飞书机器人推送

详细决策与选型对比见 [docs/design.md](docs/design.md)。
