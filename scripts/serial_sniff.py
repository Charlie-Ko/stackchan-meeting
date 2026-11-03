#!/usr/bin/env python3
"""抓串口输出 N 秒（pio device monitor 需要交互终端，脚本里用不了它）。

用法：
    python3 scripts/serial_sniff.py /dev/cu.usbmodem1101 10
    python3 scripts/serial_sniff.py /dev/cu.usbmodem1101 600 /tmp/btn.log   # 边收边写文件
"""
import sys
import time

sys.path.insert(0, "/Users/charlie/.platformio-pylib")
import serial  # noqa: E402

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
out_path = sys.argv[3] if len(sys.argv) > 3 else None

s = serial.Serial(port, 115200, timeout=0.3)
# 脉冲 RTS 让板子复位，从头开始看：DTR/RTS 组合 = 经典 esptool 复位时序
s.setDTR(False)
s.setRTS(True)
time.sleep(0.1)
s.setRTS(False)
time.sleep(0.1)

out = open(out_path, "w", encoding="utf-8") if out_path else None
t0 = time.time()
buf = bytearray()
while time.time() - t0 < dur:
    chunk = s.read(4096)
    if chunk:
        buf.extend(chunk)
        if out:
            out.write(chunk.decode("utf-8", errors="replace"))
            out.flush()
s.close()
if out:
    out.close()
    print(f"已写入 {out_path}（{len(buf)} 字节，{dur:.0f}s）")
    sys.exit(0)

text = buf.decode("utf-8", errors="replace")
print(f"=== 收到 {len(buf)} 字节 ===")
print(text if text.strip() else "（无输出）")
