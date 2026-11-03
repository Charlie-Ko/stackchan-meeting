# 桌面会议记录机器人（对标飞书录音豆 · 飞书妙记闭环）

> 约束先行文档。最后更新：2026-10-18
> 2026-10-18 定案：飞书免费版流式 ASR 配额不放行（且妙记仅 300 分钟/月），**ASR 主算力改阿里云百炼**（实时 paraformer-realtime-v2 / 文件 paraformer-v2，各带 10 小时/月免费）；飞书降级为"推送+存件"出口。详见 3.5.1b / 3.5.1c / 4 / 8b 节。
> 历史：曾按"飞书妙记文件进/纪要出"单线设计，详见第 2 节。

## 1. 项目目标
基于 M5Stack 硬件（Chan 桌面机器人 + Stick S3 便携版），做一款"桌面会议记录"设备，对标飞书录音豆：
- 放桌上 / 随身，一键开录
- 录完 → 上传 → 飞书妙记异步生成 → 取结构化纪要（逐字稿 / 总结 / 待办）→ 推送到飞书
- 设备端只管采集与上传，**不放任何密钥**

## 2. 关键认知修正（2026-10-15，作者查证）
- **妙记是异步文件服务，不是流式服务**。没有"实时推音频流进妙记"的接口。产品形态=真正的录音豆：先录音、结束/分段后上传、再出纪要。
- 正确链路（已核实）：音视频**先上传飞书云空间拿 file_token** → 用 file_token **生成妙记** → 轮询完成 → **取逐字稿/总结/待办**。
- 原方案"设备采集→云端 ASR 自建"被推翻：妙记本身就是成品服务，没必要自己拼 ASR+LLM+前端，且产物直接落飞书闭环更省。

## 3. 已核实的飞书妙记 API 事实（2026-10-15）
- 创建妙记：`POST /open-apis/minutes/v1/minutes/upload`，body `{file_token}`，**需 user_access_token**，权限"生成妙记文件"。异步，立即返回 `minute_token`/`minute_url`，产物后台生成。
- 查询状态：`GET /open-apis/minutes/v1/minutes/:minute_token`，轮询至 ready（错误码 2091003 = not ready）。
- 取逐字稿：`GET /open-apis/minutes/v1/minutes/:minute_token/transcript`（导出妙记文字记录）。总结/待办/章节另有接口。
- 妙记支持格式：音频 wav/mp3/m4a/aac/ogg/wma/amr；视频 avi/wmv/mov/mp4/m4v/mpeg/ogg/flv。**时长≤6h、大小≤6GB**。
- 云空间上传：`POST /open-apis/drive/v1/files/upload_all`，**单文件≤20MB**，超 20MB 必须分片（upload_prepare/part/finish）。5 QPS、10000 次/天。
- ⚠️ **鉴权是核心风险**：创建妙记强依赖 user_access_token（用户授权），ECS 网关需实现 OAuth 授权回调 + refresh_token 持久化。

## 3.5 实时流式 ASR 与说话人分离分析（2026-10-18）

### 3.5.1 飞书流式 ASR 接口（关键新事实，2026-10-18 实测）
- 接口：`POST /open-apis/speech_to_text/v1/speech/stream_recognize`
- **鉴权：`tenant_access_token`（Bearer）✅ 已实测通过**（App「桌面机器人」）。
- 音频：PCM_S16LE / 16kHz / 单声道；base64 编码。
- **请求体（已用真实接口探明，缺 `config` 会报 99992402 field validation failed）**：
  ```json
  {
    "config": {
      "stream_id": "唯一流ID",
      "sequence_id": 0,
      "action": "start",
      "engine_type": "16k_auto",
      "format": "pcm"
    },
    "speech": { "data": "<base64 PCM>", "is_end": true }
  }
  ```
- 返回：`text` + `start_time` / `end_time`（含时间戳，便于打点对齐）。
- ⚠️ **实测卡点**：返回 `99991400 request trigger frequency limit`，冷却 1 分钟后单发仍限流。
- ❌ **结论（2026-10-18 定案）：飞书流式 ASR 在免费版上不采用。** 作者为飞书免费用户，接口虽可达、字段虽打通，但配额不放行；继续磕这口子在投入产出上不划算。**实时层改用阿里云 `paraformer-realtime-v2`。**

### 3.5.1b 飞书免费版额度硬约束（必须知道的账）
飞书官方 2024-12 起执行的权益表（基础免费版 / 个人版同规则）：

| 权益 | 免费版 | 商业版/企业版 |
|---|---|---|
| 妙记语音转文字 | **300 分钟 / 用户 / 月**（会议录制、录音、本地文件上传、云文件导入 四场景合计） | 不限 |
| API 调用次数 | **1 万次 / 租户 / 月** | 不限 |
| 聊天记录存储 | 18 个月 | 永久 |

- 300 分钟 ≈ 5 小时/月。**一周两三场 1 小时会，半个月就烧光**，超出后录音还能录、但不再出文字。
- 会中字幕不占转写额度，但**我们走的是上传文件生成妙记，要占额度**。
- 1 万次 API/月：单次会议若按分段轮询，很容易吃掉大半，需控制轮询频率。
- **决策：飞书只当"出口"用（机器人推纪要、云空间存件、可选妙记），不承担 ASR 主算力。**

### 3.5.1c 阿里云百炼 ASR 选型（免费版用户的更优解）
| 模型 | 用途 | 单价 | 免费额度 | 折合 |
|---|---|---|---|---|
| `paraformer-realtime-v2` | **实时字幕主力** | 0.00024 元/秒 | **36000 秒/月（10 小时）**，每月 1 日发放、有效期 1 个月 | 0.864 元/小时 |
| `paraformer-v2` | 会后文件识别 + `diarization_enabled` 分人 | 0.00008 元/秒 | **36000 秒/月（10 小时）** | 0.288 元/小时 |
| `fun-asr-realtime` | 备选（噪声鲁棒性更好） | 0.00033 元/秒 | 36000 秒/月 | 1.188 元/小时 |
- 注意：以上是**百炼**价；老 ISI 实时识别 3.50 元/小时、一句话识别 3.50 元/千次，**贵 4 倍，不要走 ISI**。
- **关键结论：每月 10h 实时 + 10h 文件，个人会议记录场景等于免费。** 比飞书免费版 5h 转写额度还大方，且分人能力自带。
- 实时走 WebSocket：`wss://dashscope.aliyuncs.com/api-ws/v1/inference`，PCM 16k/16bit/单声道，百炼 SDK 或裸 WS 均可。
- ✅ **已实测跑通（2026-10-18 18:15）**：
  - API Key（`sk-ws-` 前缀）用**通用域名 `dashscope.aliyuncs.com` 直接可用，不需要 Workspace ID**。
  - `paraformer-realtime-v2` 可用；5.3 秒中文 TTS 音频识别**一字不差**（含标点）。
  - 协议：`run-task` → 二进制 PCM 分帧（3200B=100ms）→ `finish-task`；事件 `task-started` / `result-generated` / `task-finished` / `task-failed`。
  - 结果字段：`sentence{sentence_id, begin_time, end_time, text, channel_id, speaker_id, sentence_begin, sentence_end, words[]}`，`words[]` 带逐字时间戳；**`speaker_id` 恒为 null，再次确认实时不分人**。
  - ⚠️ **关键坑：`text` 是累积式的**，同一 `sentence_id` 每次回传整句当前全量。**直接拼接会变成复读机**，必须按 `sentence_id` 覆盖、以 `sentence_end=true` 为定稿。网关和固件都要按这个逻辑处理。
- ⚠️ ECS `i-2zejfwwf1jkyv4cp3grm`（cn-beijing）**未绑定 RAM 实例角色**（元数据 404），故需 AK：优先在 ECS 上绑 RAM 角色（免 AK，最干净），否则用环境变量注入 `DASHSCOPE_API_KEY`。

### 3.5.2 直连飞书 vs 经阿里云 ECS 网关（优劣对比）
| 维度 | 设备直连飞书 | 设备→ECS 网关→飞书（推荐） |
|---|---|---|
| 密钥安全 | ❌ 设备须持 app_secret / token，违反红线 | ✅ 密钥只在 ECS，设备零密钥 |
| token 刷新 | ❌ 2h 过期，设备做 OAuth/刷新不可靠 | ✅ 网关统一刷新、复用连接 |
| 协议复杂度 | ❌ 每 200–300ms 一次 TLS+HTTP 分片，ESP32 压力大、易断 | ✅ 设备只推一条简单流（TCP/WS），网关做分片转换 |
| 断网兜底 | ❌ 丢字无法补 | ✅ 缓存/重传、落盘补传 |
| 会后分人/纪要 | ❌ 设备做不了 | ✅ 网关跑 diarization + LLM 纪要 |
| 多设备/可观测 | ❌ 各自为战、无日志 | ✅ 统一管理、可排查 |
| 延迟 | 少一跳（约省几十 ms） | 多一跳（实时延迟 ~0.5s→~0.6s，可忽略） |
| 成本 | 0 | 最低配 ECS 约 60–100 元/月 |
- **结论：必过网关。** 理由排序：密钥安全 > 协议隔离 > 可靠性兜底 > 后处理能力 > 可运维；**性能不是理由**（多一跳可忽略）。

### 3.5.3 说话人分离（diarization）能力矩阵
| 方案 | 实时？ | 分人？ | 说明 |
|---|---|---|---|
| 飞书 `stream_recognize` | ✅ 实时 | ❌ **不带 speaker_id** | 只回文字+时间戳，流式不做分人 |
| 飞书妙记（异步） | ❌ 会后 | ✅ 声纹分人，10 人内 ~95%+，可关联飞书昵称、可手动改 | 分人+纪要最强，但非实时 |
| 阿里云 `paraformer-realtime-v2` | ✅ 实时 | ❌ 不支持 diarization | 实时只出字 |
| 阿里云 `paraformer-v2`（文件） | ❌ 会后 | ✅ `diarization_enabled=true`，回 `speaker_id`；建议 ≤2h | 会后分人主力 |
| 自建 pyannote / 3D-Speaker | ❌ 会后 | ✅ 可定制 | 重、需 GPU，暂不推荐 |
- **核心结论：实时阶段"出字"与"分人"不可兼得。** 产品必须分两层：
  - **实时层**：只做"看得见的字"（飞书 stream_recognize 或 paraformer-realtime-v2），屏上滚字。
  - **会后层**：整段音频走妙记（或 paraformer-v2 开 diarization）→ 分人逐字稿 + LLM 纪要。
- 想"实时就知道谁在说"：需自建声纹聚类，成本高、准确率不稳，**不建议 P0/P1 做**。
- ✅ **决策（2026-10-18，作者拍板）：实时层不做说话人区分，只出文字；分人只在会后妙记生成纪要时考虑。** 实时接口即使将来带了 speaker_id 也不接，保持实时链路极简。

## 4. 整体架构（实时层 + 会后层，双路）
```
                        ┌──────────── 实时层（会中，屏上出字，只出字不分人）────────────┐
[设备] 采集(单/双麦) ──WS/TCP PCM流──▶ [ECS 网关] ──WS 分片──▶ 阿里云 paraformer-realtime-v2
   │  ▲                                   │                              │            （百炼）
   │  └───────── 文字回推（屏上滚字）◀─────┘◀──────── text+时间戳 ◀────────┘
   │
   │  （同一份音频同时落 SD，含打点 JSON）
   ▼
                        ┌───────────────── 会后层（分人+纪要）────────────────────┐
   分段 WAV 上传 ──▶ [ECS 网关] ──┬─▶ 阿里云 paraformer-v2(diarization_enabled) 分人逐字稿 ★主路
                                  └─▶ 或 飞书妙记（免费版 300 分钟/月，额度够时用）
                                                   │
                                   LLM 纪要(DeepSeek) + speaker_id 对齐 ──▶ 飞书机器人推送 / 网页存档
```
- **设备端零密钥**，只持 ECS 网关地址 + 设备身份 token；飞书凭证/token 全在网关。
- **实时层**：只出字、不分人；走阿里云 `paraformer-realtime-v2`（飞书流式已弃用）；延迟目标 ~0.6s 量级。
- **会后层**：分人 + 结构化纪要。**主路 paraformer-v2（便宜 0.288元/h、分人可控）**，飞书妙记作可选（免费版仅 300 分钟/月，额度够且图省事时用）。纪要由 DeepSeek 生成。
- BLE **仅用于配网与控制**，不传音频主链路。
- 设备端始终"边传边落 SD"，断网不丢、恢复补传。

## 5. 设备与能力（2026-10-14 调研，仍有效）
### Chan 桌面机器人（StackChan / CoreS3）—— 桌面主站
- ESP32-S3，16MB Flash + 8MB PSRAM；**双麦克风 ES7210**（可做降噪/波束）；1W 扬声器；2.0" 屏；**microSD 卡槽**（主存储）；550mAh；Wi-Fi+BLE。
- 角色：桌面长会议，分段录 SD、屏显状态、表情反馈。

### Stick S3 —— 便携验证机
- ESP32-S3-PICO，8MB/8MB；**单 MEMS 麦 ES8311（SNR 65dB）**；1.14" 屏；250mAh；Wi-Fi+BLE；**无 SD 卡槽**。
- 角色：随身简化版 / 前期验证。约束：本地存储小，短段录音（受 PSRAM/Flash）或直接边录边传；长会议靠 Chan。

## 5b. Chan 官方固件架构调研（2026-10-18）

> 结论先行：**官方固件有真正的应用框架（mooncake），但 HAL 不开放连续录音的裸 PCM**。这决定了"做成官方 app"不是顺手的事，代价主要在音频链路和整套 UI 重写。

### 5b.1 仓库与更新状态
- 主仓库 `github.com/m5stack/StackChan`（1.4k star / 381 fork / 63 commits），四个子目录：`firmware` / `app`（iOS+Android）/ `remote`（遥控器）/ `server`（账号、AI Agent、App Center 后端，Go）。
- **无 GitHub Releases**，版本靠 commit 推进。README 明说"仓库更新可能略滞后于已发布固件"。
- 最近提交：firmware `2026-08-19`（新增 ILI9342E 屏支持，主线到 **v1.4.1**）；app `2026-04-29`（v1.1.4）；remote `2026-06-17`；server `2026-06-24`。
- 另有官方 **StackChan-BSP**（`m5stack/StackChan-BSP`，2026-08-28 仍在维护）——Arduino 开发用的板级支持包，依赖 M5Unified + IRremoteESP8266 + M5Unit-NFC，示例只有 Servo / RGB_LED / TouchSensor / IR / NFC / INA226，**不含音频示例**。

### 5b.2 官方固件架构（ESP-IDF v5.5.4 + CMake，C++）
```
apps/            mooncake 应用框架（AppAbility: onCreate/onOpen/onRunning/onClose）
                 launcher｜ai_agent(小智)｜avatar｜dance｜setup(配网)｜app_center
                 espnow｜ezdata｜app_template（官方二次开发模板）
hal/             Hal 单例 GetHAL()
                 audio.cpp｜hal_network｜hal_ota｜hal_account｜hal_app_center
                 hal_servo｜hal_ble｜hal_mcp｜hal_imu｜hal_rtc｜hal_head_touch
xiaozhi SDK      AFE + VAD + 唤醒词，独占 ES7210 麦克风管线
board drivers    ES7210 编解码｜ILI9342C/C 屏｜双舵机｜12×RGB｜NFC
```
- UI 是 **LVGL**（smooth_ui_toolkit + smooth_lvgl），不是 M5GFX 直接绘制；访问需 `LvglLockGuard`。
- 配网走 App/BLE（`startAppConfigServer`），不是热点网页。
- 舵机在 HAL 内部（`servo_init`），**未暴露公开的"点头/摇头"语义接口**给 app。
- App Center：`fetchAppList()` 返回的 `firmwareUrl` + `launchApp(url)` 下载的是**整包固件**，走 OTA 分区切换——不是插件热加载。**没有公开的第三方应用上传入口**，别指望把自己的 app 上架官方市场。
- 服务端地址可用 `CONFIG_STACKCHAN_SERVER_URL` 覆盖，默认指向 M5Stack 自家服务器。

### 5b.3 致命卡点：HAL 拿不到连续 PCM
`hal.h` 的 Audio 段只有三个公开方法：
```cpp
void setSpeakerVolume(uint8_t);
uint8_t getSpeakerVolume();
std::string startMicTest(std::function<void(MicTestStatus)>);  // 录 3 秒回放
void getMicWaveformFrame(std::vector<int16_t>& data);          // 只给 128 点波形
void clearupMicTest();
```
真正驱动录音的是 `xiaozhi_board_init()` 里的小智音频管线（AFE/VAD/唤醒词）。要在官方固件里做会议记录，只有两条路：
1. **绕开小智**，直接 `Board::GetInstance().GetAudioCodec()->EnableInput/InputData` 读 I2S（audio.cpp 内部就是这么干的）——可行，但要保证小智不启动、不抢 codec，且自己处理双麦参考通道；
2. **改小智音频回调**做 PCM 分流——要动 vendor SDK，升级即冲突。

### 5b.4 三条路线与代价

| 维度 | A 官方 app（mooncake） | B 独立 Arduino 固件（现状） | C Arduino + 官方 BSP |
|---|---|---|---|
| 开发成本 | 高（IDF 5.5.4 + 依赖拉齐） | 最低（已投入） | 中低 |
| 音频裸流 | 要绕小智管线 | ✅ 完全掌控 | ✅ 完全掌控 |
| UI | 全部重写为 LVGL | ✅ 现有 M5GFX 代码可留 | ✅ 不变 |
| 能力复用 | 配网/OTA/账号/舵机/表情 | 无，自己写 | 舵机/灯/触摸用官方封装 |
| 与 AI 助手共存 | ✅ launcher 内切换 | ❌ 刷固件切换 | ❌ 刷固件切换 |
| 工具链 | **需 ESP-IDF 5.5.4**（本机现装 6.0.1，不兼容） | Arduino/PlatformIO | Arduino/PlatformIO |

**当前建议：短期走 C，产品化再评估 A。** 理由：会议记录的核心是音频链路，而官方 HAL 恰好把这条链封在小智 SDK 里；同时 UI 要整套重写，投入产出比在"先跑通"阶段不划算。
**唯一会让结论反转的前提**：作者要求"平时当 AI 桌面宠物、开会时切到会议记录"——那只有 A 能做到，代价是要接受 IDF 5.5.4 + LVGL + 绕音频管线三件事。

## 6. 已采纳的 6 条建议（作者 2026-10-15）
1. **去掉 Mac + BLE 主链路**：无实时需求，BLE 带宽/macOS 调度/Mac 必须开机等问题全消失。BLE 只留配网+控制。最低配 ECS 做上传网关+任务轮询，不在上面跑 ASR。
2. **SD 卡变主存储（非兜底）**：分段录制（建议每 5–10 分钟一个文件），断电/断网不丢，网络恢复补传。格式待定（见第 7 节）。
3. **打点=离线侧文件+事后对齐**：设备只记"累计采样点数+备注"写 JSON 与音频同存；妙记生成后用逐字稿时间戳对齐到文稿位置。无往妙记写标记的接口，标记由网关自己生成（附在推送纪要消息里）。
4. **提前验证鉴权**：妙记以用户身份调用，ECS 须处理用户授权+token 刷新。作为 P0 第一步验证，不最后才发现。
5. **实时字幕=可选项，与妙记解耦**：要实时显示另接独立流式 ASR（如本地 SenseVoice），与妙记两条互不依赖的线。
6. **别忽略拾音质量**：妙记识别+说话人区分很吃音频。桌面近场 OK，但须实测 ES7210 增益与回声，影响远大于传输方案。

## 7. 设备端设计要点
- 录音格式：**首选 WAV（PCM 16kHz / 16bit / 单声道）**——妙记支持、ESP32 直写最简单。分段≤10 分钟 ≈ 15MB，落在云空间 `upload_all` 20MB 内，免分片。
  - Opus 妙记格式列表未明确支持（列表含 ogg 非 opus），若用 Opus 需本地转 WAV/MP3 后再传 → 暂定 WAV。
  - 更高采样/更长分段超 20MB 时，网关走分片上传。
- 分段：每 5–10 分钟落一个文件 + 同名 `.json` 打点（累计采样点数、备注、段序号、开始时间戳）。
- 网络：WiFi 上传到 ECS 网关；失败缓存 SD，恢复后补传。
- BLE：仅配网（填 Wi-Fi 凭证）+ 控制（开始/停止/状态），不传音频。
- 安全：设备端**不存飞书密钥**，只持 ECS 网关地址 + 设备身份 token。

## 8. 云端 ECS 网关设计
- 密钥：阿里云 `DASHSCOPE_API_KEY`（或 RAM 角色）；飞书 app_id/app_secret。**全部走环境变量，不进代码。**
- 接口：
  1. `POST /upload` 收设备音频（设备身份校验）。
  2. **会后主路**：转临时 OSS URL → 阿里云 `paraformer-v2`（`diarization_enabled=true`）→ 分人逐字稿。
  3. **可选路**：上传云空间 → file_token → 妙记（受 300 分钟/月限制）。
  4. LLM（DeepSeek）出结构化纪要 + 打点对齐。
  5. 推送飞书（机器人消息/卡片）或网页存档。
- **实时路**：`WS /stream` 设备 PCM ↔ 阿里云 `paraformer-realtime-v2`，边收边回 `text`。
- 任务状态机：received → uploaded → recognized/transcribing → ready → pushed → failed（带重试/补传）。

## 9. 落地顺序（作者指定，先点亮跑通）
- ✅ **P0-1 实时链路打通（2026-10-18 完成）**：百炼 `paraformer-realtime-v2` 直连验证通过 → 本地网关跑通 → 部署 ECS → nginx 反代 → **公网端到端识别一字不差**。
- ✅ **P0-2 设备固件（已交付待烧录）**：`firmware/chan-cores3/`，CoreS3 触摸开录 → 16k PCM WS 推流 → 屏上实时滚字 + 定稿/当前句双色区分。
- ⬜ **P1 设备端持久化**：SD 分段录制（WAV）+ 断网补传（固件已留 `ENABLE_SD_BACKUP` 开关，未实现）。
- ⬜ **P1.5 实机调优**：Chan 双麦增益/拾音距离实测，`magnification` 调整。
- ⬜ **P2 会后分人 + 纪要**：整段音频 → `paraformer-v2`（`diarization_enabled`）→ DeepSeek 纪要 → 飞书推送。
- **P1 设备端**：Chan/Stick 分段录制（WAV）+ SD 存储 + 上传 ECS + 断网补传。
- **P1.5 实时字幕**：设备 PCM 流 → ECS 网关（WS）→ **阿里云 `paraformer-realtime-v2`** → 屏上滚字；**只出字、不分人**（分人交会后）。飞书流式已弃用。
- **P2 打点对齐**：离线 JSON 打点 + 逐字稿时间戳对齐 + 网关生成标记附推送。
- **P3 可选**：多设备；说话人区分实测优化（paraformer-v2 为主，妙记额度内可对比）。

## 8b. 云端网关职责调整（2026-10-18）
- ASR 主算力：阿里云百炼（实时 paraformer-realtime-v2 / 文件 paraformer-v2），密钥 `DASHSCOPE_API_KEY` 只在 ECS 环境变量。
- 飞书职责收缩为：**机器人推送 + 云空间存件 + 可选妙记**（受 300 分钟/月 与 1 万次 API/月 限制）。
- 网关需新增：WS 双向代理（设备 PCM ↔ 阿里云 WS）、文本流回推、断线重连与会话续接。
- 计费护栏：本地累计当月用量，接近 10 小时免费额度时告警（避免静默计费）。

## 10. 待验证清单（开工前）
- [ ] 创建妙记是否**必须** user_access_token（tenant_access_token 是否可行）→ 决定网关授权架构（最高优先级，P0 第一步）
- [ ] 自建应用所需 scope：生成妙记、云空间上传下载、导出妙记文字、机器人推送
- [ ] Opus 是否被妙记接受；否则统一 WAV
- [ ] 分段文件大小 vs 20MB upload_all 上限（确认免分片分段时长）
- [ ] 妙记说话人区分效果（多人会议）；ES7210 双麦增益/回声实测
- [ ] ECS 运行时确认（Python3 等，上一轮探测中断，待重探）
- [x] 飞书鉴权：`tenant_access_token` ✅ 已通过（App「桌面机器人」，App ID `cli_xxxxxxxxxxxxxxxx`）
- [x] 流式字段结构：`config{stream_id,sequence_id,action,engine_type,format}` + `speech{data,is_end}` ✅ 已探明
- [x] 机器人能力 ✅（bot info 返回 app_name=桌面机器人）、云空间 drive ✅（root folder meta 正常）
- [x] **阿里云百炼开通 + `DASHSCOPE_API_KEY`** ✅ 已通：Key 有效（176 模型可查）、`paraformer-realtime-v2` 实测识别准确
- [x] **实时端到端** ✅ 已通：WS 握手 → run-task → 推帧 → 出字（脚本 `scripts/dashscope_asr_verify.py`）
- [ ] 会后文件识别实测（需公网可访问 URL → OSS 或临时上传）
- [ ] 分人实测：`paraformer-v2` + `diarization_enabled=true` 是否真回 `speaker_id`
- [x] ~~stream_recognize 配额放行~~ → **放弃**，免费版不做飞书流式（决定改阿里云）
- [ ] ~~实时接口是否带 `speaker_id`~~：已决策实时不分人，不阻塞；分人效果只在会后妙记验证即可
- [ ] 分人效果对比：妙记 vs 阿里云 `paraformer-v2`（`diarization_enabled`），用同一段多人会议音频

## 11. 约束与红线（作者规范）
- 密钥 / token / 密码不进代码（走环境变量 / 飞书 RAM 等效权限最小化）
- 设备端不放飞书密钥，凭证只在 ECS 网关
- 改完主动跑验证，不注释报错找借口
- 重复 3 遍的事必须 AI 化 / 自动化
- 先点亮跑通再谈其他（P0 手机走通优先）

## 12. 方案评审：作者 2026-10-23 的"WiFi 直连 ECS + 百炼 + 回屏"方案

**结论先行：这份方案的主干（设备 WiFi 直连 ECS → 百炼流式 ASR → 文本回屏）就是当前已实现并跑通的架构，不是新方案。** 真正缺的只有本地 VAD，已补。另外 3 处若照抄会让现有系统退化，已驳回。

### 12.1 逐条对照

| 方案要求 | 现状 | 处置 |
|---|---|---|
| WiFi 直连 ECS，不走 BLE/Mac 中转 | ✅ 早已是这条路（`gateway/app.py` + nginx 80 反代） | 保持 |
| 16k/单声道/S16LE PCM | ✅ `SAMPLE_RATE=16000`，`magnification=16` | 保持 |
| WebSocket 推音频分片 | ✅ 二进制帧，每帧 50ms / 1600B | 保持（比方案更细） |
| 服务端拼包 + 会话管理 | ✅ 每条连接一个百炼 task，`run-task`→推流→`finish-task` | 保持 |
| 百炼流式 ASR | ✅ `paraformer-realtime-v2`（已实测一字不差） | 保持 |
| 文本回传屏幕滚动展示 | ✅ 中间结果 `p` / 定稿 `f` 双色，专有名词加粗 | 保持 |
| 中间候选 vs 定稿区分 | ✅ `gPartial` / `gCommitted` | 保持 |
| token 鉴权 | ✅ `?token=` + nginx | 保持 |
| 断线自动重连 | ✅ `setReconnectInterval(3000)`，录中途收到 `ready` 即续推 | 保持 |
| **本地 VAD 静音裁剪** | ❌ 原来一帧不落地全传 | ✅ **本轮已实现** |
| 固定 buffer 池 | ✅ 静态 `micBuf`，零 malloc | 保持 |
| 会话生命周期（静音超时关 task） | ✅ `{"t":"stop"}` + 网关等最后一句定稿再断 | 保持 |
| base64 + seq + session_id 的 JSON 信封 | ❌ 未用，用的是裸二进制 | **驳回**（见 12.2） |
| `board = m5stack_core_s3` / `platform = espressif32@6.5.0` / registry lib_deps / `default.csv` | ❌ 与现有配置不同 | **驳回**（见 12.2） |
| FunASR 本地兜底 | ❌ 未做 | **推迟**（见 12.3） |

### 12.2 驳回的三处（照抄会退化）

1. **base64 + seq + session_id 的 JSON 信封**：WS 建在 TCP 上，天然有序、天然不丢，`seq` 是多余的；base64 让上行体积 +33%，还要在 ESP32 上做编解码耗 CPU。现有裸二进制帧更省更简单。`session_id` 在设备侧无意义——一条 WS 连接就是一个会话，网关用连接 ID 区分即可。
2. **platformio.ini 那一段**：`board` 名不对（现网是 `m5stack-cores3`）；`platform = espressif32@6.5.0` 拉的是 arduino-esp32 2.0.17（IDF 4.4.7），`UART_SCLK_DEFAULT` 在这版不存在，会直接编不过（本机已用条件编译绕开）；registry 的 `m5stack/M5Unified` 在本机会慢到超时（19KB/s），已改 git 源锁 tag。
3. **`board_build.partitions = default.csv`**：4MB 分区表没有 `otadata`/`app1`，一改就把 OTA 能力废掉。现网是 `default_16MB.csv`（双 app 分区），OTA 靠它。
4. **"不要传 wav、传裸 PCM"**：这条是对的，现有实现本来就是裸 PCM，无需改动。

### 12.3 推迟项与真实缺口

- **FunASR 本地兜底（推迟）**：ECS 2C2G 跑 FunASR 需要额外 1~2GB 内存 + 模型文件，与 nginx/网关抢资源；且百炼每月 10h 免费额度对个人会议场景够用，欠费风险低。等真被限流再上，先把 `/health` 的 `month_free_used_pct` 盯住。
- **真正的缺口（方案里没提但更重要）：会后没有音频落盘。** 设备只上传不存，ECS 只转发不落盘，于是"会后分人 + 纪要"这条 P2 链路无料可用。要做 P2，必须让网关在转发的同时把 PCM 追加写到 ECS 本地（WAV，16k/mono），否则实时字幕一关，内容就没了。列为 P1。

### 12.4 本轮新增：本地 VAD

- 文件：`firmware/chan-cores3/vad.h`（新）+ `config.h` VAD 参数段 + `.ino` 录音泵接入。
- 只决定"这一帧发不发"，**不改变会话状态**：静音不断流、不发 `stop`。
- 三条安全网防吞字：① 句首补发 400ms 预滚；② 开头 1s 预热全量上传并校准噪声底；③ 连续抑制超 8s 自动下调噪声底 6dB 并放行。
- 阈值自适应噪声底（±dB 余量），不是写死 dBFS——换会议室不用改代码。
- 验证：`scripts/vad_sim.py` 是算法镜像，5 个用例全 PASS（句首不吞、换气不切碎、-45dBFS 高底噪仍识别、全程静音省 76%、开机就说话不漏）。编译 RAM 21.3% / Flash 23.4%。
- 现场调阈值看串口 `[VAD]` 行（`VAD_DEBUG=1`），主要看 `nf=` 是否接近真实底噪。
