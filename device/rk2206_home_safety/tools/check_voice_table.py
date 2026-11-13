#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""核对 SU-03T 词条表与固件宏定义是否一致。

背景：模块上的词条号是“人工在平台里填”的，固件里的编号是“宏定义”的，
两边一旦错位，现象往往是“报警了但播的是温度”或“完全没声音”，很难查。
这个脚本把两边逐条比对，不一致就直接报错，烧录前跑一次即可。

用法：
    python tools/check_voice_table.py              # 只做一致性检查
    python tools/check_voice_table.py --frames     # 同时输出可粘贴的串口帧
    python tools/check_voice_table.py --csv XXX.csv
"""

import argparse
import csv
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_CSV = os.path.join(ROOT, "docs", "su03t_词条表.csv")
HEADER = os.path.join(ROOT, "include", "su_03t.h")

# 固件 handle_extended() 里各前缀的参数上限，超出会被直接丢弃。
PFX_LIMIT = {0x10: 2, 0x11: 2, 0x12: 4, 0x13: 3, 0x14: 5, 0x15: 3}
LEGACY_PFX = {0x00, 0x01, 0x03}
LEGACY_LIMIT = {0x00: 2, 0x01: 2, 0x03: 2}


def read_text(path):
    with io.open(path, "r", encoding="utf-8-sig") as f:
        return f.read()


def parse_header(path):
    """从 su_03t.h 取出 SU03T_SAY_* 与 SU03T_PFX_* 的编号。"""
    text = read_text(path)
    say = {}
    pfx = {}
    for name, value in re.findall(r"#define\s+(SU03T_SAY_[A-Z0-9_]+)\s+(\d+)", text):
        say[name] = int(value)
    for name, value in re.findall(r"#define\s+(SU03T_PFX_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|\d+)", text):
        pfx[name] = int(value, 0)
    return say, pfx


def parse_csv(path):
    rows = []
    with io.open(path, "r", encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f):
            row = {(k or "").strip(): (v or "").strip() for k, v in row.items()}
            rows.append(row)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default=DEFAULT_CSV)
    ap.add_argument("--frames", action="store_true", help="输出每条播报的串口帧")
    args = ap.parse_args()

    errors = []
    warns = []

    if not os.path.exists(args.csv):
        print("找不到词条表：%s" % args.csv)
        return 2
    say, pfx = parse_header(HEADER)
    rows = parse_csv(args.csv)

    if not say:
        errors.append("su_03t.h 里没有解析到 SU03T_SAY_* 宏，检查文件是否被改过")

    # 1. 播报词条：CSV 里 4~23 必须与头文件宏一一对应，且不重复、不缺号。
    csv_say = {}
    for row in rows:
        cat = row.get("类别", "")
        idx_text = row.get("词条号", "")
        if cat not in ("报警播报", "应答播报", "数值播报"):
            continue
        if not idx_text.isdigit():
            errors.append("播报行词条号不是数字：%r（%s）" % (idx_text, row.get("用途", "")))
            continue
        idx = int(idx_text)
        if idx in csv_say:
            errors.append("词条号 %d 重复：%s / %s" % (idx, csv_say[idx].get("用途", ""), row.get("用途", "")))
        csv_say[idx] = row
        expect_hex = "0x%02x" % idx
        given_hex = row.get("十六进制", "").lower()
        if given_hex and given_hex != expect_hex:
            errors.append("第 %d 条十六进制写成 %s，应为 %s" % (idx, given_hex, expect_hex))

    header_idx = set(say.values())
    csv_idx = set(k for k in csv_say if k >= 10)
    missing = sorted(header_idx - csv_idx)
    extra = sorted(csv_idx - header_idx)
    if missing:
        errors.append("头文件有、词条表缺：%s" % ", ".join(str(i) for i in missing))
    if extra:
        errors.append("词条表有、头文件没定义：%s" % ", ".join(str(i) for i in extra))

    for idx in range(4, 24):
        if idx not in csv_say:
            errors.append("词条 %d 在表里不存在" % idx)

    text_field = {}
    for idx, row in csv_say.items():
        text = row.get("建议口令/播报文本", "")
        if not text:
            warns.append("词条 %d 没有写播报文本" % idx)
        text_field[idx] = text

    # 同号不同义最容易出事：报警 8 和应答 13 都是“紧急求助”，是设计如此，不报错。
    # 但“用途”完全为空的需要提示。
    for idx, row in csv_say.items():
        if not row.get("用途"):
            warns.append("词条 %d 用途为空" % idx)

    # 2. 识别指令：前缀必须被固件处理，参数必须在范围内。
    cmds = []
    seen_cmd = {}
    for row in rows:
        cat = row.get("类别", "")
        if cat not in ("识别指令", "兼容指令"):
            continue
        frame = row.get("串口帧", "") or row.get("十六进制", "")
        parts = frame.replace(",", " ").split()
        if len(parts) != 2:
            errors.append("指令帧格式不对：%r（%s）" % (frame, row.get("用途", "")))
            continue
        try:
            hi, lo = int(parts[0], 16), int(parts[1], 16)
        except ValueError:
            errors.append("指令帧不是十六进制：%r" % frame)
            continue
        cmds.append((hi, lo, row.get("用途", ""), row.get("建议口令/播报文本", "")))
        key = (hi, lo)
        if key in seen_cmd:
            warns.append("指令 %02X %02X 重复出现：%s / %s" % (hi, lo, seen_cmd[key], row.get("用途", "")))
        seen_cmd[key] = row.get("用途", "")

        if hi in PFX_LIMIT:
            if lo < 1 or lo > PFX_LIMIT[hi]:
                errors.append("指令 %02X %02X 参数超范围（%02X 只到 %d）" % (hi, lo, hi, PFX_LIMIT[hi]))
        elif hi in LEGACY_PFX:
            if lo < 1 or lo > LEGACY_LIMIT[hi]:
                errors.append("兼容指令 %02X %02X 参数超范围" % (hi, lo))
        else:
            errors.append("指令前缀 %02X 固件不处理（可用：10~15 或兼容 00/01/03）" % hi)

    # 3. 每个固件前缀至少要有一条指令，否则那类语音等于没接。
    used = set(hi for hi, _, _, _ in cmds if hi in PFX_LIMIT)
    for hi in sorted(PFX_LIMIT):
        if hi not in used:
            errors.append("前缀 %02X 在词条表里没有任何指令，固件这一段等于空着" % hi)

    # 输出
    print("词条表：%s" % os.path.basename(args.csv))
    print("播报词条 %d 条（%d~%d），识别指令 %d 条" % (
        len([k for k in csv_say if k >= 4]), min(csv_say) if csv_say else 0,
        max(csv_say) if csv_say else 0, len(cmds)))
    print("头文件宏：SAY %d 个，PFX %d 个" % (len(say), len(pfx)))

    for w in warns:
        print("  ! %s" % w)
    if errors:
        print("\n发现 %d 处不一致，建议改完再烧：" % len(errors))
        for e in errors:
            print("  x %s" % e)
    else:
        print("\n一致：词条号、十六进制、指令前缀都能对上，可以按表配置模块。")

    if args.frames:
        print("\n=== 播报自检帧（发到 UART2，115200 8N1）===")
        for idx in sorted(csv_say):
            if idx > 23:
                continue
            if idx <= 3:
                print("%2d  AA 55 %02X <double 小端> 55 AA   %s" % (idx, idx, text_field.get(idx, "")))
            else:
                print("%2d  AA 55 %02X 00 55 AA                 %s" % (idx, idx, text_field.get(idx, "")))
        print("\n提示：编译时加 -DSU03T_SELF_TEST=1，上电即按顺序自播 4~23，")
        print("      比手工发帧省事；哪条没声音，就是那号词条没烧进去。")

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
