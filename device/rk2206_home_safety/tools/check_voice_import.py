# -*- coding: utf-8 -*-
"""语音词导入表自检：把 Excel/CSV 导入表与固件宏逐条对齐。

烧录前跑一次，能挡掉三类"只有听到声音才发现"的错误：

  1. 发送协议里的前缀固件不处理  -> 说了没反应
  2. 接收协议里的消息号固件不发  -> 永远不播
  3. 同一端口接收消息号重复      -> 平台直接阻断导入

用法：

    cd device/rk2206_home_safety
    python tools/check_voice_import.py                 # 默认读 ../../docs/语音词导入_小颐.csv
    python tools/check_voice_import.py --csv 路径
    python tools/check_voice_import.py --xlsx 路径     # 直接从 xlsx 读

退出码非 0 表示有问题，改完再导入平台。
"""
from __future__ import print_function

import argparse
import csv
import io
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                    # device/rk2206_home_safety
REPO = os.path.dirname(os.path.dirname(ROOT))   # repo 根

DEFAULT_CSV = os.path.join(REPO, "docs", "语音词导入_小颐.csv")
HEADER = os.path.join(ROOT, "include", "su_03t.h")
SOURCE = os.path.join(ROOT, "src", "su_03t.c")

FRAME_RE = re.compile(r"^([0-9A-Fa-f]{2})((?:\s+[0-9A-Fa-f]{2})*)$")


def read_header():
    """从 su_03t.h 取出合法前缀集合与播报词条编号集合。"""
    text = io.open(HEADER, encoding="utf-8").read()

    prefixes = set()
    for name, value in re.findall(r"#define\s+(SU03T_PFX_\w+)\s+(0x[0-9A-Fa-f]+)", text):
        prefixes.add(int(value, 16))
    # 出厂遗留前缀（固件 handle_legacy 里处理）
    prefixes.update({0x00, 0x01, 0x03})

    says = {}
    for name, value in re.findall(r"#define\s+(SU03T_SAY_\w+)\s+(\d+)", text):
        says[int(value)] = name

    # 报警句 4~9 没有单独的宏，固件里是 su03t_process_voices 的索引表
    src = io.open(SOURCE, encoding="utf-8").read()
    m = re.search(r"uint8_t\s+indices\s*\[\s*\d+\s*\]\s*=\s*\{([^}]*)\}", src)
    if m:
        for v in m.group(1).split(","):
            v = v.strip()
            if v.isdigit():
                says.setdefault(int(v), "alarm")
    return prefixes, says


def parse_frame(text):
    """'AA 55 13 01 55 AA' -> (head, msgno, params, tail) 或 None"""
    parts = text.replace(",", " ").split()
    if len(parts) < 3:
        return None
    try:
        raw = [int(p, 16) for p in parts]
    except ValueError:
        return None
    if len(raw) < 4:
        return None
    head = (raw[0], raw[1])
    msgno = raw[2]
    if len(raw) >= 6 and (raw[-2], raw[-1]) == (0x55, 0xAA):
        body = raw[3:-2]
        tail = (raw[-2], raw[-1])
    else:
        body = raw[3:]
        tail = None
    return head, msgno, body, tail


def read_rows_csv(path):
    with io.open(path, encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


def read_rows_xlsx(path):
    """不依赖 openpyxl，直接解析 xlsx 内部 XML。"""
    ns = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"
    z = zipfile.ZipFile(path)
    shared = []
    if "xl/sharedStrings.xml" in z.namelist():
        import xml.etree.ElementTree as ET
        for si in ET.fromstring(z.read("xl/sharedStrings.xml")).findall(ns + "si"):
            shared.append("".join(t.text or "" for t in si.iter(ns + "t")))
    import xml.etree.ElementTree as ET
    root = ET.fromstring(z.read("xl/worksheets/sheet1.xml"))
    table = []
    for row in root.iter(ns + "row"):
        if int(row.get("r")) < 14:
            continue
        cells = {}
        for c in row:
            ref = c.get("r")
            col = "".join(ch for ch in ref if ch.isalpha())
            v = c.find(ns + "v")
            if v is None:
                continue
            if c.get("t") == "s":
                cells[col] = shared[int(v.text)]
            else:
                cells[col] = v.text
        if any(cells.values()):
            table.append(cells)
    if not table:
        return []
    header = table[0]
    rows = []
    for cells in table[1:]:
        rows.append({header.get(k, k): val for k, val in cells.items()})
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default=DEFAULT_CSV)
    ap.add_argument("--xlsx", default="")
    args = ap.parse_args()

    if args.xlsx:
        if not os.path.exists(args.xlsx):
            print("找不到文件：%s" % args.xlsx)
            return 2
        rows = read_rows_xlsx(args.xlsx)
        src = args.xlsx
    else:
        if not os.path.exists(args.csv):
            print("找不到文件：%s" % args.csv)
            print("用 --csv 或 --xlsx 指定导入表")
            return 2
        rows = read_rows_csv(args.csv)
        src = args.csv

    prefixes, says = read_header()
    prefix_txt = ", ".join("0x%02X" % p for p in sorted(prefixes))
    print("导入表：%s" % src)
    print("固件合法前缀：%s" % prefix_txt)
    print("固件播报词条：%s" % (", ".join(str(k) for k in sorted(says)) or "无"))
    print("-" * 60)

    errors = []
    warns = []
    seen_rx = {}
    n_cmd = n_say = 0

    for i, row in enumerate(rows, start=2):
        name = (row.get("行为名称") or "").strip()
        trig = (row.get("触发方式") or "").strip()
        port = (row.get("串口") or "").strip()
        words = (row.get("命令词") or "").strip()
        tx = (row.get("发送协议") or "").strip()
        rx = (row.get("接收协议") or "").strip()
        reply = (row.get("回复语") or "").strip()

        if not name:
            if words or tx or rx:
                errors.append("第 %d 行没填行为名称" % i)
            continue
        if trig not in ("命令词", "串口输入"):
            errors.append("第 %d 行「%s」触发方式为「%s」，只能是 命令词 / 串口输入" % (i, name, trig))
        if port != "UART1":
            errors.append("第 %d 行「%s」串口填了「%s」，平台只接受 UART1" % (i, name, port))

        if trig == "命令词":
            n_cmd += 1
            if not words:
                errors.append("第 %d 行「%s」是命令词但没填命令词" % (i, name))
            if "小颐" in words:
                errors.append("第 %d 行「%s」命令词里带了唤醒词「小颐」，会变成重复唤醒" % (i, name))
            if not tx:
                errors.append("第 %d 行「%s」没有发送协议，说了不会有任何反应" % (i, name))
                continue
            parsed = parse_frame(tx)
            if not parsed:
                errors.append("第 %d 行「%s」发送协议「%s」格式不对（要 十六进制、空格分隔）" % (i, name, tx))
                continue
            head, msgno, body, tail = parsed
            if head != (0xAA, 0x55):
                warns.append("第 %d 行「%s」发送协议帧头是 %02X %02X，不是 AA 55" % (i, name, head[0], head[1]))
            if msgno not in prefixes:
                errors.append("第 %d 行「%s」前缀 0x%02X 固件不处理，发过去也没反应" % (i, name, msgno))
            elif body:
                param = body[0]
                if msgno == 0x13 and not (1 <= param <= 3):
                    errors.append("第 %d 行「%s」灯光参数 %d 超出 1~3" % (i, name, param))
                if msgno == 0x11 and not (1 <= param <= 2):
                    errors.append("第 %d 行「%s」静音参数 %d 超出 1~2" % (i, name, param))
                if msgno == 0x10 and not (1 <= param <= 2):
                    errors.append("第 %d 行「%s」求助参数 %d 超出 1~2" % (i, name, param))
                if msgno == 0x12 and not (1 <= param <= 4):
                    errors.append("第 %d 行「%s」服药参数 %d 超出 1~4" % (i, name, param))
                if msgno == 0x14 and not (1 <= param <= 6):
                    errors.append("第 %d 行「%s」查询参数 %d 超出 1~6" % (i, name, param))
                if msgno == 0x15 and not (1 <= param <= 9):
                    errors.append("第 %d 行「%s」翻页参数 %d 超出 1~9" % (i, name, param))
            if reply:
                warns.append("第 %d 行「%s」命令词自带回复「%s」，会与主板应答连播两句" % (i, name, reply))

        elif trig == "串口输入":
            n_say += 1
            if not rx:
                errors.append("第 %d 行「%s」是串口输入但没填接收协议，永远不会播" % (i, name))
                continue
            if not reply:
                errors.append("第 %d 行「%s」没有回复语，触发了也没声音" % (i, name))
            parsed = parse_frame(rx)
            if not parsed:
                errors.append("第 %d 行「%s」接收协议「%s」格式不对" % (i, name, rx))
                continue
            head, msgno, body, tail = parsed
            if head != (0xAA, 0x55):
                errors.append("第 %d 行「%s」接收协议帧头是 %02X %02X，固件发的是 AA 55"
                              % (i, name, head[0], head[1]))
            if tail != (0x55, 0xAA):
                errors.append("第 %d 行「%s」接收协议帧尾是 %s，固件发的是 55 AA"
                              % (i, name, "空" if tail is None else "%02X %02X" % tail))
            if body != [0x00]:
                errors.append("第 %d 行「%s」接收协议参数应为 00（固件固定发 0），实际是 %s"
                              % (i, name, " ".join("%02X" % b for b in body) or "空"))
            if msgno in seen_rx:
                errors.append("第 %d 行「%s」接收消息号 0x%02X 与第 %d 行重复，平台会阻断导入"
                              % (i, name, msgno, seen_rx[msgno]))
            else:
                seen_rx[msgno] = i
            if msgno not in says:
                errors.append("第 %d 行「%s」消息号 0x%02X(%d) 固件里没有对应 SU03T_SAY_*"
                              % (i, name, msgno, msgno))

    print("命令词 %d 条，串口播报 %d 条，共 %d 条" % (n_cmd, n_say, n_cmd + n_say))

    missing = sorted(set(says) - set(seen_rx))
    if missing:
        warns.append("固件会发但导入表没配的播报号：%s（对应提示音不会响）"
                     % ", ".join("%d" % m for m in missing))

    for w in warns:
        print("  ! " + w)
    for e in errors:
        print("  x " + e)

    print("-" * 60)
    if errors:
        print("发现 %d 个错误，改完再导入平台。" % len(errors))
        return 1
    print("通过：导入表与固件一致，可以导入并烧录。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
