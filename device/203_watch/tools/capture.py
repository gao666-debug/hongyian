# -*- coding: utf-8 -*-
"""按秒采集串口日志，端口被复位/重新枚举导致句柄失效时自动重开继续抓。

烧录完板子会重启，USB 转串口可能短暂掉线，普通脚本会直接抛异常退出、
把开机那几行最关键的自检日志丢掉。

用法: capture.py COM10 45 boot.log
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM10'
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0
OUT = sys.argv[3] if len(sys.argv) > 3 else 'cap.log'
BAUD = int(sys.argv[4]) if len(sys.argv) > 4 else 115200

t0 = time.time()
total = 0
with open(OUT, 'w', encoding='utf-8', errors='replace') as f:
    while time.time() - t0 < SECS:
        ser = None
        try:
            ser = serial.Serial(PORT, BAUD, timeout=0.2)
            while time.time() - t0 < SECS:
                data = ser.read(4096)
                if data:
                    f.write(data.decode('utf-8', 'replace'))
                    f.flush()
                    total += len(data)
        except Exception as exc:              # 端口被复位/占用，等一下重开
            f.write('\n# [capture] 端口异常，重开: %r\n' % (exc,))
            f.flush()
            time.sleep(0.5)
        finally:
            try:
                if ser is not None:
                    ser.close()
            except Exception:
                pass
print('captured %s (%.1fs, %d bytes)' % (OUT, time.time() - t0, total))
