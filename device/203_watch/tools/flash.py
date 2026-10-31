# -*- coding: utf-8 -*-
"""WS63 烧录 v10 —— 一键全流程

阶段A: ROM 握手 -> YMODEM 传 loaderboot -> 等 Load Cmd Loop
阶段B: 逐段 CMD_DL_IMAGE -> 头包 -> 数据 -> EOT

用法: python flash_v10.py COM10 4        (只烧段4)
      python flash_v10.py COM10 1,2,4,3,6
"""
import struct
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM10'
SEGS = [int(x) for x in (sys.argv[2] if len(sys.argv) > 2 else '4').split(',')]
FW = sys.argv[3] if len(sys.argv) > 3 else r'D:\ws63\firmware_203watch.fwpkg'
HS_WAIT = int(sys.argv[4]) if len(sys.argv) > 4 else 180
NOHANDSHAKE = (len(sys.argv) > 5 and sys.argv[5] == 'nohs')

SOH, STX, EOT, ACK, NAK, CAN, C = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43
CMD_ACK, CMD_DL_IMAGE, CMD_RESET = 0xE1, 0xD2, 0x87

TAB = []
for _i in range(256):
    _c = _i << 8
    for _ in range(8):
        _c = ((_c << 1) ^ 0x1021) & 0xFFFF if (_c & 0x8000) else (_c << 1) & 0xFFFF
    TAB.append(_c)


def crc16(data):
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ TAB[((crc >> 8) ^ b) & 0xFF]
    return crc


def hs_frame():
    payload = bytes([0x00, 0xC2, 0x01, 0x00, 0x08, 0x01, 0x00, 0x00])
    body = (0xDEADBEEF).to_bytes(4, 'little') + (18).to_bytes(2, 'little') + \
           bytes([0xF0, 0x0F]) + payload
    return body + crc16(body).to_bytes(2, 'little')


def frame(t, payload=b''):
    size = 8 + len(payload) + 2
    body = (0xDEADBEEF).to_bytes(4, 'little') + size.to_bytes(2, 'little') + \
           bytes([t, (~t) & 0xFF]) + payload
    return body + crc16(body).to_bytes(2, 'little')


def parse_fwpkg(path):
    d = open(path, 'rb').read()
    _f, _c, num, _t = struct.unpack('IHHI', d[:12])
    out = []
    for i in range(num):
        o = 12 + i * 52
        name, st, sz, addr, bsz, typ = struct.unpack('32sIIIII', d[o:o + 52])
        out.append({'name': name.split(b'\x00')[0].decode(), 'data': d[st:st + sz],
                    'size': sz, 'addr': addr, 'burn': bsz, 'type': typ})
    return out


class Dev:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0, write_timeout=20)
        self.buf = bytearray()

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def poll(self, dur):
        start = len(self.buf)
        t = time.time()
        while time.time() - t < dur:
            n = self.s.in_waiting
            if n:
                self.buf += self.s.read(n)
            else:
                time.sleep(0.0005)
        return bytes(self.buf[start:])

    def find_ack_frame(self):
        i = self.buf.find(b'\xef\xbe\xad\xde')
        while i >= 0:
            if i + 12 <= len(self.buf):
                if int.from_bytes(self.buf[i + 4:i + 6], 'little') == 0x0C and self.buf[i + 6] == 0xE1:
                    code = self.buf[i + 8]
                    del self.buf[:i + 12]
                    return code
            i = self.buf.find(b'\xef\xbe\xad\xde', i + 1)
        return None

    def wait_ack_frame(self, timeout):
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < timeout:
            n = self.s.in_waiting
            if n:
                self.buf += self.s.read(n)
            code = self.find_ack_frame()
            if code is not None:
                return code
            time.sleep(0.0005)
        return None

    def wait_byte(self, want, timeout, keep=False):
        t0 = time.perf_counter()
        other = bytearray()
        while time.perf_counter() - t0 < timeout:
            n = self.s.in_waiting
            if n:
                self.buf += self.s.read(n)
            while self.buf:
                c = self.buf[0]
                del self.buf[0]
                if c == want:
                    if not keep:
                        self.buf.clear()
                    return True, bytes(other)
                other.append(c)
            time.sleep(0.0004)
        return False, bytes(other)

    def wait_ack_byte(self, timeout):
        t0 = time.perf_counter()
        other = bytearray()
        while time.perf_counter() - t0 < timeout:
            n = self.s.in_waiting
            if n:
                self.buf += self.s.read(n)
            while self.buf:
                c = self.buf[0]
                del self.buf[0]
                if c in (ACK, NAK, CAN):
                    return c, time.perf_counter() - t0, bytes(other)
                other.append(c)
            time.sleep(0.0002)
        return None, time.perf_counter() - t0, bytes(other)


def build_head(name, size):
    hdr = name.encode() + b'\x00' + str(size).encode() + b'\x00'
    body = hdr + b'\x00' * (128 - len(hdr))
    cs = crc16(body)
    return bytes([SOH, 0x00, 0xFF]) + body + bytes([(cs >> 8) & 0xFF, cs & 0xFF])


def build_blocks(data, pkt=1024):
    out = []
    blk = 1
    off = 0
    while off < len(data):
        chunk = data[off:off + pkt]
        body = chunk + b'\x1a' * (pkt - len(chunk))
        cs = crc16(body)
        head = STX if pkt == 1024 else SOH
        out.append((blk, bytes([head, blk & 0xFF, (255 - blk) & 0xFF]) + body +
                    bytes([(cs >> 8) & 0xFF, cs & 0xFF])))
        off += len(chunk)
        blk = (blk + 1) & 0xFF
    return out


def ymodem_send(dev, name, data, label=''):
    """通用 YMODEM 发送：等 C -> 头包 -> 数据块 -> EOT"""
    ok, other = dev.wait_byte(C, 2.5)
    if not ok:
        print('  %s 等不到 C（期间 %r），照样发头包' % (label, bytes(other[:40])), flush=True)

    dev.s.write(build_head(name, len(data)))
    c, dtb, other = dev.wait_ack_byte(3.0)
    if c != ACK:
        print('  %s 头包失败 c=%s %.0fms 其它=%s' % (label, c, dtb * 1000, other[:60].hex(' ')), flush=True)
        return False

    blocks = build_blocks(data)
    t_start = time.perf_counter()
    for i, (blk, pkt) in enumerate(blocks):
        good = False
        for attempt in range(4):
            dev.s.write(pkt)
            c, dtb, other = dev.wait_ack_byte(1.2)
            if c == ACK:
                good = True
                break
            if c == CAN:
                break
            print('    块%d 重试%d c=%s 其它=%s' % (blk, attempt + 1, c, other[:40].hex(' ')), flush=True)
        if not good:
            print('  %s !!! 块 %d/%d 失败' % (label, i + 1, len(blocks)), flush=True)
            return False
        if (i + 1) % 64 == 0:
            print('    %d/%d %.1fs' % (i + 1, len(blocks), time.perf_counter() - t_start), flush=True)

    for _ in range(4):
        dev.s.write(bytes([EOT]))
        c, _, _ = dev.wait_ack_byte(1.5)
        if c == ACK:
            break
    ok, other = dev.wait_byte(C, 4)
    if ok:
        body = b'\x00' * 128
        cs = crc16(body)
        dev.s.write(bytes([SOH, 0x00, 0xFF]) + body + bytes([(cs >> 8) & 0xFF, cs & 0xFF]))
        dev.wait_ack_frame(3)
    print('  %s 完成 %.1fs' % (label, time.perf_counter() - t_start), flush=True)
    return True


def rom_stage(dev, loader):
    hs = hs_frame()
    print('=== 请按一次 RST（%d 秒窗口）===' % HS_WAIT, flush=True)
    t0 = time.time()
    sent = 0
    while time.time() - t0 < HS_WAIT:
        dev.s.write(hs)
        sent += 1
        t1 = time.time()
        while time.time() - t1 < 0.025:
            n = dev.s.in_waiting
            if n:
                dev.buf += dev.s.read(n)
            else:
                time.sleep(0.0005)
        if dev.find_ack_frame() is not None:
            print('[%.1fs] ROM 握手成功' % (time.time() - t0), flush=True)
            break
        if sent % 200 == 0:
            print('   [%.0fs] %d 帧...' % (time.time() - t0, sent), flush=True)
    else:
        print('ROM 握手超时', flush=True)
        return False

    # 清回显
    t0 = time.time()
    last = time.time()
    while (time.time() - t0) < 3:
        n = dev.s.in_waiting
        if n:
            dev.buf += dev.s.read(n)
            last = time.time()
        elif (time.time() - last) > 0.4:
            break
        else:
            time.sleep(0.002)
    dev.buf.clear()

    print('>>> 传 loaderboot (%d 字节)' % loader['size'], flush=True)
    if not ymodem_send(dev, loader['name'], loader['data'], 'loaderboot'):
        return False

    print('>>> 等 loaderboot 启动...', flush=True)
    tail = dev.poll(2.5)
    txt = tail.decode('utf-8', 'replace')
    print(txt[:600], flush=True)
    if 'SFC_FLASH_NOT_SUPPORT' in txt or 'Flash Init Fail' in txt:
        print('!!! loaderboot 自身 flash 初始化失败，擦除必然无响应', flush=True)
        print('!!! 请拔掉扩展板、确保供电充足后重试', flush=True)
        return False
    if 'Cmd Loop' in txt or 'Loadboot Uart Init' in txt:
        print('>>> 已进入 Cmd Loop', flush=True)
        return True
    print('>>> 未见 Cmd Loop 提示，尝试继续', flush=True)
    return True


def cmd_stage(dev, seg):
    foff = seg['addr']
    erase = max(seg['burn'], (seg['size'] + 0xFFF) & ~0xFFF)
    print('\n===== %s  addr=0x%X size=0x%X erase=0x%X ====='
          % (seg['name'], foff, seg['size'], erase), flush=True)
    payload = foff.to_bytes(4, 'little') + seg['size'].to_bytes(4, 'little') + \
        erase.to_bytes(4, 'little') + bytes([0])
    dev.buf.clear()
    dev.s.write(frame(CMD_DL_IMAGE, payload))
    code = dev.wait_ack_frame(45)
    print('擦除 ACK = %s' % ('0x%02X' % code if code is not None else 'None'), flush=True)
    if code != 0x5A:
        return False
    return ymodem_send(dev, seg['name'], seg['data'], seg['name'])


def main():
    imgs = parse_fwpkg(FW)
    loader = imgs[0]
    dev = Dev(PORT)
    print('串口 %s 打开' % PORT, flush=True)
    dev.poll(0.3)

    if NOHANDSHAKE:
        print('=== 跳过 ROM 握手，假定设备已在 Load Cmd Loop ===', flush=True)
        tail = dev.poll(1.5)
        if tail:
            print('  当前串口输出: %r' % bytes(tail[:200]), flush=True)
        dev.buf.clear()
    elif not rom_stage(dev, loader):
        print('ROM 阶段失败', flush=True)
        dev.close()
        return 1

    ok = True
    for idx in SEGS:
        if idx == 0:
            continue
        if not cmd_stage(dev, imgs[idx]):
            ok = False
            print('!! 段 %d 失败' % idx, flush=True)
            break

    if ok:
        print('\n全部完成，发 CMD_RESET', flush=True)
        dev.s.write(frame(CMD_RESET, b''))
        time.sleep(0.5)

    print('\n=== 监听 10 秒 ===', flush=True)
    tail = dev.poll(10.0)
    print(tail[:1200].decode('utf-8', 'replace'), flush=True)
    dev.close()
    return 0 if ok else 1


sys.exit(main() or 0)
