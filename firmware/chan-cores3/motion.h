#pragma once

// ============================================================
//  motion.h · StackChan 底座舵机（SCS0009 串行总线）
//
//  设计原则：录音优先，宁可不动，也不能响。
//
//  开关逻辑（三层静音）：
//    1. 总开关关闭（出厂默认）→ 不开电源、不装串口、不发一个字节
//    2. 录音中 lock()        → 先卸扭矩，再断 VM_EN 电源 → 彻底无声
//    3. 仅在待机/暂停/结束页 → 才允许轻微动作
//
//  协议：飞特 SCS 系列（帧 FF FF ID LEN CMD ADDR DATA... ~SUM）
//  寄存器：TORQUE_ENABLE=40，GOAL_POSITION_L=42（2 字节大端）
//  总线：UART1 @1Mbps，TX=GPIO6 / RX=GPIO7
//  电源：PY32 IO 扩展器 I2C 0x6F，pin0 = VM_EN
// ============================================================

#include <M5Unified.h>
#include <driver/uart.h>
#include <esp_idf_version.h>
#include "config.h"
#include "settings.h"

namespace motion {

static bool     s_ready  = false;   // 串口 + 电源都就绪
static bool     s_locked = false;   // 录音中：已断电
static uint8_t  s_phase  = 0;       // 简单动作状态机
static uint32_t s_tick   = 0;

// ---------------- PY32 IO 扩展器（舵机电源）----------------
// 直接用 I2C 裸读写，绕开 M5Unified 各版本 IOExpander 接口差异
// 注意：readRegister8/writeRegister8 的 freq 参数没有默认值，必须显式给
static const uint32_t PY32_FREQ = 100000;

static inline bool pyRead(uint8_t reg, uint8_t &v) {
  return m5::In_I2C.readRegister(PY32_ADDR, reg, &v, 1, PY32_FREQ);
}
static inline bool pyWrite(uint8_t reg, uint8_t v) {
  return m5::In_I2C.writeRegister8(PY32_ADDR, reg, v, PY32_FREQ);
}
static bool pySet(uint8_t reg, uint8_t bit, bool on) {
  uint8_t v = 0;
  if (!pyRead(reg, v)) return false;
  if (on) v |= (uint8_t)(1u << bit);
  else    v &= (uint8_t)~(1u << bit);
  return pyWrite(reg, v);
}

// 探一下底座在不在（读版本寄存器，0/0xFF 视为不存在）
static bool pyProbe() {
  uint8_t ver = 0;
  if (!pyRead(0x02, ver)) return false;
  return ver != 0x00 && ver != 0xFF;
}

static bool powerOn() {
  // GPIO_M_L(0x03) pin0 = 输出；GPIO_PU_L(0x09) pin0 = 上拉；GPIO_O_L(0x05) pin0 = 高
  pySet(0x03, PY32_PIN_VMEN, true);
  pySet(0x09, PY32_PIN_VMEN, true);
  pySet(0x05, PY32_PIN_VMEN, true);
  delay(180);   // 等舵机内部上电
  return true;
}

static void powerOff() {
  pySet(0x05, PY32_PIN_VMEN, false);
}

// ---------------- SCS 串行协议 ----------------
static bool uartBegin() {
  uart_config_t c = {};
  c.baud_rate  = SERVO_BAUD;
  c.data_bits  = UART_DATA_8_BITS;
  c.parity     = UART_PARITY_DISABLE;
  c.stop_bits  = UART_STOP_BITS_1;
  c.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
  // 时钟源名字跨 IDF 版本不同：IDF 5.x 用 UART_SCLK_DEFAULT，IDF 4.x 用 UART_SCLK_APB
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  c.source_clk = UART_SCLK_DEFAULT;
#else
  c.source_clk = UART_SCLK_APB;
#endif

  if (uart_driver_install((uart_port_t)SERVO_UART_NUM, 256, 256, 0, NULL, 0) != ESP_OK) {
    return false;
  }
  if (uart_param_config((uart_port_t)SERVO_UART_NUM, &c) != ESP_OK) return false;
  if (uart_set_pin((uart_port_t)SERVO_UART_NUM, SERVO_TX_PIN, SERVO_RX_PIN,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return false;
  return true;
}

static void uartEnd() {
  uart_driver_delete((uart_port_t)SERVO_UART_NUM);
}

// 一帧写指令：FF FF ID LEN CMD ADDR DATA... ~SUM
static void scsWrite(uint8_t id, uint8_t addr, const uint8_t *d, uint8_t n) {
  uint8_t b[16];
  uint8_t len = (uint8_t)(3 + n);   // LEN = CMD + ADDR + DATA 的字节数
  b[0] = 0xFF;
  b[1] = 0xFF;
  b[2] = id;
  b[3] = len;
  b[4] = 0x03;                      // INST_WRITE
  b[5] = addr;

  uint8_t sum = (uint8_t)(id + len + 0x03 + addr);
  for (uint8_t i = 0; i < n; i++) {
    b[6 + i] = d[i];
    sum = (uint8_t)(sum + d[i]);
  }
  b[6 + n] = (uint8_t)(~sum);

  uart_write_bytes((uart_port_t)SERVO_UART_NUM, (const char *)b, 7 + n);
}

static inline void scsTorque(uint8_t id, bool on) {
  uint8_t v = on ? 1 : 0;
  scsWrite(id, 40, &v, 1);
}

// time 单位 20ms（飞特定义），0 = 用速度控制
static inline void scsMove(uint8_t id, int raw, uint16_t timeUnit, uint16_t speed) {
  uint8_t d[6];
  d[0] = (uint8_t)((raw >> 8) & 0xFF);   // 大端：高字节先
  d[1] = (uint8_t)(raw & 0xFF);
  d[2] = (uint8_t)((timeUnit >> 8) & 0xFF);
  d[3] = (uint8_t)(timeUnit & 0xFF);
  d[4] = (uint8_t)((speed >> 8) & 0xFF);
  d[5] = (uint8_t)(speed & 0xFF);
  scsWrite(id, 42, d, 6);
}

// 角度 → 原始步（1 度 = 0.32 步）
static inline int deg2raw(int deg, int zero) {
  int v = zero + (deg * 32) / 100;
  return constrain(v, 0, 1000);
}

// ---------------- 对外 ----------------
static inline bool enabled()   { return s_ready && !s_locked; }
static inline bool powered()   { return s_ready; }
static inline bool available() { return s_ready; }

// 按当前配置启动；返回 false 表示底座不在或开关关着（都不影响录音）
static bool begin() {
  if (!settings::get().servoEnabled) {
    Serial.println("[MOT] disabled by config");
    return false;
  }
  if (!pyProbe()) {
    Serial.println("[MOT] no StackChan base (PY32 not found)");
    return false;
  }
  if (!uartBegin()) {
    Serial.println("[MOT] uart init failed");
    return false;
  }
  powerOn();
  s_ready  = true;
  s_locked = false;

  // 上电后慢慢回中位，避免突然甩头
  scsTorque(SERVO_ID_YAW, true);
  scsTorque(SERVO_ID_PITCH, true);
  delay(20);
  scsMove(SERVO_ID_YAW,   deg2raw(SERVO_HOME_YAW_DEG,   SERVO_ZERO_YAW),   25, 0);
  scsMove(SERVO_ID_PITCH, deg2raw(SERVO_HOME_PITCH_DEG, SERVO_ZERO_PITCH), 25, 0);
  Serial.println("[MOT] ready");
  return true;
}

static void end() {
  if (!s_ready) return;
  scsTorque(SERVO_ID_YAW, false);
  scsTorque(SERVO_ID_PITCH, false);
  delay(30);
  powerOff();
  uartEnd();
  s_ready  = false;
  s_locked = false;
  Serial.println("[MOT] stopped");
}

// 录音开始前调：先卸力再断电 —— 舵机既不保位也不带电，本底噪声归零
static void lock() {
  if (!s_ready || s_locked) return;
  scsTorque(SERVO_ID_YAW, false);
  scsTorque(SERVO_ID_PITCH, false);
  delay(20);
  powerOff();
  s_locked = true;
  s_phase  = 0;
  Serial.println("[MOT] locked (power off) for recording");
}

// 录音结束后调：重新上电并慢慢回中位（录音已停，这时的声音无所谓）
static void unlock() {
  if (!s_ready || !s_locked) return;
  powerOn();
  scsTorque(SERVO_ID_YAW, true);
  scsTorque(SERVO_ID_PITCH, true);
  delay(20);
  scsMove(SERVO_ID_YAW,   deg2raw(SERVO_HOME_YAW_DEG,   SERVO_ZERO_YAW),   20, 0);
  scsMove(SERVO_ID_PITCH, deg2raw(SERVO_HOME_PITCH_DEG, SERVO_ZERO_PITCH), 20, 0);
  s_locked = false;
  Serial.println("[MOT] unlocked");
}

// 轻微点头（只在非录音状态生效）
static void nod() {
  if (!enabled() || s_phase != 0) return;
  s_phase = 1;
  s_tick  = millis();
  scsTorque(SERVO_ID_PITCH, true);
  scsMove(SERVO_ID_PITCH, deg2raw(SERVO_HOME_PITCH_DEG + 8, SERVO_ZERO_PITCH), 12, 0);
}

// 左右轻摆（表示"在听"）
static void sway() {
  if (!enabled() || s_phase != 0) return;
  s_phase = 3;
  s_tick  = millis();
  scsTorque(SERVO_ID_YAW, true);
  scsMove(SERVO_ID_YAW, deg2raw(SERVO_HOME_YAW_DEG + 6, SERVO_ZERO_YAW), 15, 0);
}

// 主循环里调，推进上面那两个小动作
static void update() {
  if (!enabled() || s_phase == 0) return;
  uint32_t dt = millis() - s_tick;

  if (s_phase == 1 && dt > 350) {          // 点头到底 → 回来
    s_phase = 2;
    s_tick  = millis();
    scsMove(SERVO_ID_PITCH, deg2raw(SERVO_HOME_PITCH_DEG, SERVO_ZERO_PITCH), 15, 0);
  } else if (s_phase == 2 && dt > 400) {
    s_phase = 0;
  } else if (s_phase == 3 && dt > 400) {   // 摆头 → 回中
    s_phase = 4;
    s_tick  = millis();
    scsMove(SERVO_ID_YAW, deg2raw(SERVO_HOME_YAW_DEG, SERVO_ZERO_YAW), 18, 0);
  } else if (s_phase == 4 && dt > 450) {
    s_phase = 0;
  }
}

// 改配置后重启生效；设置页只负责写 NVS + 提示
static void applySetting(bool on) {
  settings::setServoEnabled(on);
}

}  // namespace motion
