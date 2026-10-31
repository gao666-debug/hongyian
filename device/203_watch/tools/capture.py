# -*- coding: utf-8 -*-
"""按秒采集串口日志，到时自动结束。
用法: cap_sec.py COM10 45 D:/ws63/v12_boot.log
"""
import sys
import time
import datetime

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM10'
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0
OUT = sys.argv[3] if len(sys.argv) > 3 else 'D:/ws63/cap.log'

ser = serial.Serial(PORT, 115200, timeout=0.2)
t0 = time.time()
with open(OUT, 'w', encoding='utf-8', errors='replace') as f:
    f.write('# start %s\n' % datetime.datetime.now().strftime('%H:%M:%S'))
    while time.time() - t0 < SECS:
        data = ser.read(4096)
        if data:
            f.write(data.decode('utf-8', 'replace'))
            f.flush()
    f.write('\n# end %s\n' % datetime.datetime.now().strftime('%H:%M:%S'))
ser.close()
print('captured %s (%.1fs)' % (OUT, time.time() - t0))
