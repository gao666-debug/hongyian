# -*- coding: utf-8 -*-
"""一直等，直到看到开机自检，再多录 60 秒后退出。

用法: cap_wait.py COM10 1200 out.log
这样用户什么时候按 RST 都不会错过。
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM10'
MAXW = int(sys.argv[2]) if len(sys.argv) > 2 else 1200
OUT = sys.argv[3] if len(sys.argv) > 3 else 'D:\\ws63\\boot_wait.log'
KEY = b'BOARD CHECK'
TAIL = 75          # 抓到自检头之后再录多少秒

s = serial.Serial(PORT, 115200, timeout=0.2)
s.reset_input_buffer()

t0 = time.time()
buf = bytearray()
started = False
tail_t0 = 0.0

print('waiting for BOARD CHECK ... (max %ds)' % MAXW, flush=True)
while time.time() - t0 < MAXW:
    d = s.read(8192)
    if d:
        buf += d
        if not started and KEY in bytes(buf):
            started = True
            tail_t0 = time.time()
            print('[%.1fs] 抓到自检头，继续录 %ds' % (time.time() - t0, TAIL), flush=True)
    if started and (time.time() - tail_t0) > TAIL:
        break
    if len(buf) > 400000:
        del buf[:100000]

s.close()
data = bytes(buf)
open(OUT, 'wb').write(data)
txt = data.decode('utf-8', 'replace')
lines = [l for l in txt.replace('\r', '').split('\n') if '[bc]' in l]
print('bytes=%d  bc_lines=%d  got_banner=%s' % (len(data), len(lines), KEY.decode() in txt))
