#pragma once

// ============================================================
//  Chan 会议记录固件 · 编译期常量
//
//  注意：这里的 WiFi / 网关只是【出厂默认值】。
//  运行时配置存在 NVS（Flash 分区），通过手机配网页面修改，
//  不需要改这个文件、更不需要重新烧录。
//  只有想改"恢复出厂后的默认值"时才动这里。
// ============================================================

#define FW_VERSION          "0.5.1"

// 屏幕自检：开机依次刷 红/绿/蓝/洋红 + 直绘/精灵各一帧，用来排查"画面不刷新"
// 平时必须是 0，只在定位显示问题时临时改 1
#define DISPLAY_SELFTEST    0

// ============ 出厂默认：WiFi ============
// 留空 = 首次开机自动进入配网热点，等手机配置
#define DEFAULT_WIFI_SSID   ""
#define DEFAULT_WIFI_PASS   ""

// ============ 出厂默认：网关 ============
// 走 ECS 的 nginx 反代（80 端口已在安全组放行）
#define DEFAULT_GW_HOST     "gw.example.com"
#define DEFAULT_GW_PORT     80
#define DEFAULT_GW_PATH     "/asr/ws/device"

// 设备令牌不进仓库：本机放一份 config_secrets.h（已 gitignore）
// 仓库里默认为空，实际使用时靠配网页面或 NVS 填入
#if __has_include("config_secrets.h")
#include "config_secrets.h"
#endif
#ifndef DEFAULT_GW_TOKEN
#define DEFAULT_GW_TOKEN    ""
#endif

#define DEFAULT_DEVICE_NAME "chan-01"

// ============ 配网热点 ============
#define AP_SSID_PREFIX      "Chan-"        // 后面拼 MAC 后两位，如 Chan-A1B2
#define AP_PASS             "changeme88"   // WPA2 最少 8 位，配网时建议改掉
#define AP_CHANNEL          6
#define AP_IP_STR           "192.168.4.1"
// 配网空闲超时：10 分钟没人操作就重启回正常模式
#define PROVISION_IDLE_MS   (10UL * 60UL * 1000UL)

// ============ 音频参数（别改，与百炼要求一致）============
#define SAMPLE_RATE         16000
#define FRAME_SAMPLES       800     // 800 samples = 50ms = 1600 字节
#define FRAME_MS            (FRAME_SAMPLES * 1000 / SAMPLE_RATE)   // = 50

// ============ 本地 VAD（静音帧不上传 · 省流量与 ASR 费用）============
// 【已关闭】原设计是"静音帧直接丢弃"，但云端 ASR 正是靠句间静音判断一句话说完了
// 才给定稿。丢掉静音 = 模型永远等不到句尾，只能靠超时硬切，于是出现
// "定稿比中间结果还短""回显跟不上"——实测真实环境底噪约 -40dBFS，VAD 门限之下
// 几乎什么都拦不住（仅省 3%），所以它连省流量都没做到，只剩副作用。
// 结论：宁可多传 32KB/s 的静音，也要把连续音频完整交给模型。
#define VAD_ENABLE          0
#define VAD_WARMUP_MS       1000    // 预热期：全量上传并校准噪声底（开机就说话也不会被吞）
#define VAD_PREROLL         8       // 人声前保留帧数：8×50ms=400ms，防止吞掉句首
#define VAD_HOLD_MS         900     // 静音持续多久才停发：盖住句子中间的换气停顿
#define VAD_MAX_SUPPRESS_MS 8000    // 连续抑制超过这么久就下调噪声底并放行（安全网）
#define VAD_ON_MARGIN_DB    9.0f    // 高于噪声底 9dB 判为人声起始
#define VAD_OFF_MARGIN_DB   5.0f    // 回落到噪声底+5dB 以下开始计时静音
#define VAD_ABS_MIN_DB      -55.0f  // 绝对门限：比这更安静的一律不算人声
#define VAD_NOISE_MIN_DB    -72.0f  // 噪声底估计的下限
#define VAD_NOISE_MAX_DB    -35.0f  // 噪声底估计的上限（16 倍增益下别高估）
#define VAD_DEBUG           1       // 1=串口打印人声起止，现场调阈值时开

// ============ 物理按键 ============
// StackChan 机身只有两颗按钮，按 M5 官方文档：
//   屏幕左边 / USB-C 旁 = 电源键（AXP2101 的 PWRKEY，短按可读、长按 6 秒关机）
//   屏幕下面 / SD 卡槽旁 = RST 复位键（直接拉 EN 复位芯片，软件根本读不到）
// 所以"按一下开始录音"只能用电源键，经 AXP2101 的 PEK 中断读，M5Unified 已把它
// 映射成 M5.BtnPWR（wasClicked / wasPressed）。
//
// ⚠️ 别再把 GPIO0 当按键：CoreS3/StackChan 的 ES7210 麦克风 MCK 就是 GPIO0，
//    录音一开始它变 4MHz 时钟，digitalRead 会读到低电平 → 假按键 → 一开录就自动停。
#define BTN_MAIN_GPIO       (-1)    // -1 = 不走 GPIO 直读（保留字段，别改成 0）
#define BTN_REPEAT_MS       900     // 两次动作最小间隔，防连击

// AW9523(0x58) 端口打点：P1_2 = 触摸 INT、P0_2 = 功放使能，都不是按键。
// 排障时开 1 看原始电平，平时 0。
#define BTN_SCAN_EXPANDER   0

// 按键原始状态打点：每 3 秒一行 [BTNRAW]。定位按键用，平时 0。
#define BTN_TRACE           1

// ============ 舵机（StackChan 底座 · SCS0009 串行总线）============
// 出厂默认关闭。关闭时：不开舵机电源、不初始化串口、不发任何指令 —— 彻底静音。
// 打开方式：设置页点「舵机」那一行切换，写 NVS，不用重新烧录。
#define DEFAULT_SERVO_ENABLED 0

#define SERVO_TX_PIN        6       // 官方底座舵机总线 TX
#define SERVO_RX_PIN        7       // 官方底座舵机总线 RX
#define SERVO_BAUD          1000000
#define SERVO_UART_NUM      1       // UART1（与 USB 调试串口不冲突）

#define SERVO_ID_YAW        1       // 水平（可 360° 连续转）
#define SERVO_ID_PITCH      2       // 俯仰（0~90°）

// 官方出厂零点（原始步，0~1000）
#define SERVO_ZERO_YAW      460
#define SERVO_ZERO_PITCH    620

// 1 度 = 0.32 原始步（舵机一步 0.3125°）
#define SERVO_HOME_YAW_DEG  0
#define SERVO_HOME_PITCH_DEG 10

// 舵机电源使能：PY32 IO 扩展器（I2C 0x6F）pin 0 = VM_EN
#define PY32_ADDR           0x6F
#define PY32_PIN_VMEN       0

// ============ 专有名词词表（屏显加粗）============
// 三级来源，优先级从低到高：内置默认 → SD 卡文件 → 配网网页保存
#define TERMS_SD_FILE       "/terms.txt"   // SD 卡根目录，一行一个词
// 默认关：CoreS3 / StackChan 的 SD 卡和 LCD 共用同一组 SPI 引脚（SCLK36/COPI37/CIPO35，CS4），
// 之前给 SD 另开一条 HSPI 总线，会把这几个脚从屏幕的 SPI3 上抢走，而且 end() 之后也不归还
//   —— 表现就是开机第一屏之后屏幕再也不刷新（一直停在"载入词表…"）。
// 词表改用配网网页填写（存 NVS），不碰 SD。真要读卡得复用屏幕那条总线，另开 `*_SD_ENABLE` 单独做。
#define TERMS_SD_ENABLE     0
#define TERMS_MAX           96             // 最多多少条
#define TERMS_MAX_LEN       24             // 单条最长字节数（UTF-8，8 个汉字）
#define TERMS_BOLD_STEP     1              // 加粗：右移多少像素重绘一次

// ============ 运行行为 ============
#define WIFI_CONNECT_MS     20000   // 开机连 WiFi 超时 → 界面上提示"连不上"（后台仍继续重试）
#define WIFI_RETRY_MS       10000   // 连不上时每过这么久重试一次，直到连上为止
#define WS_RECONNECT_OFF_MS (24UL * 60UL * 60UL * 1000UL)  // "关闭自动重连"= 1 天（0 在库里等于立即重连）
#define AUD_STATS           1       // 录音时每 2s 打印峰值/有效值/削波率
#define WS_CONNECT_MS       10000   // 建 WS 会话超时
#define STOP_TIMEOUT_MS     8000    // 等最后一句定稿的超时
#define LONG_PRESS_MS       2500    // 主界面长按进配网

// ============ 界面 ============
// 二维码用 M5GFX 内置 qrcode 绘制；若某些库版本编译报错，改成 0 即可
#define UI_SHOW_QR          1

#define UI_SCREEN_W         320
#define UI_SCREEN_H         240
#define UI_TOP_H            26      // 顶部状态栏高
#define UI_BTN_Y            190     // 底部按钮栏起始 y
#define UI_BTN_H            42
#define UI_BODY_TOP         32      // 正文区顶部
#define UI_LINE_H           18
#define UI_MARGIN           8

// 屏幕只看尾部，限制内存里的定稿字数
#define MAX_COMMIT_CHARS    400

// ============ OTA（WiFi 无线烧录）============
// 第一次必须走 USB；之后只要在同一局域网，就能：
//   make ota IP=<屏幕上的 IP>
// mDNS 名字 = <OTA_HOSTNAME>.local
#define OTA_HOSTNAME        "chan-cores3"
#define OTA_PASSWORD        "changeme"      // 局域网内改固件的口令，别留空
#define OTA_PORT            3232

