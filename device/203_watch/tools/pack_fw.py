# -*- coding: utf-8 -*-
"""按 203_watch 固件包格式重新打包 fwpkg"""
import struct, os, sys

B = r'D:\ws\src\output\ws63\acore'
segs = [
    ('root_loaderboot_sign.bin',  os.path.join(B, 'boot_bin'),  31808,   0x200000, 31808),
    ('root_params_sign.bin',      os.path.join(B, 'param_bin'), 1920,    0x200000, 1920),
    ('ssb_sign.bin',              os.path.join(B, 'boot_bin'),  21696,   0x201000, 21696),
    ('flashboot_sign.bin',        os.path.join(B, 'boot_bin'),  49472,   0x220000, 49472),
    ('flashboot_backup_sign.bin', os.path.join(B, 'boot_bin'),  49472,   0x210000, 49472),
    ('ws63_all_nv.bin',           os.path.join(B, 'nv_bin'),    8192,    0x5fc000, 16384),
    ('ws63-liteos-app-sign.bin',  os.path.join(B, 'ws63-liteos-app'), 1313280, 0x230000, 1313280),
]

hdr_len = 12 + 52 * len(segs)
entries = bytearray()
body = bytearray()
off = hdr_len

for name, d, size, addr, burn in segs:
    p = os.path.join(d, name)
    if not os.path.exists(p):
        print('MISSING', p)
        sys.exit(1)
    data = open(p, 'rb').read()
    # app 段每次编译长度会小幅变化，以实际文件为准；其余段必须严格匹配
    if 'liteos-app' in name:
        size = len(data)
        burn = (size + 0xFFF) & ~0xFFF      # 擦除按 4KB 扇区对齐
    elif len(data) != size:
        print('SIZE MISMATCH', name, len(data), 'expect', size)
        sys.exit(1)
    nb = name.encode('ascii').ljust(32, b'\0')[:32]
    # 目录项 52 字节 = name[32] + offset(4) + size(4) + load_addr(4) + burn_size(4) + reserve(4)
    entries += nb + struct.pack('<5I', off, size, addr, burn, 0)
    body += data
    off += size

total = off
# 头部须与 flash_v10.parse_fwpkg 的 'IHHI' 对齐：flag / crc / num / reserved
out = struct.pack('<IHHI', 0xefbeaddf, 0, len(segs), 0) + bytes(entries) + bytes(body)
dst = r'D:\ws63\firmware_203watch_v2.fwpkg'
open(dst, 'wb').write(out)
print('written %s  %d bytes  (expect %d)' % (dst, len(out), hdr_len + sum(s[2] for s in segs)))
for s in segs:
    print('  %-30s %8d @0x%06x burn=%d' % (s[0], s[2], s[3], s[4]))
