# -*- coding: utf-8 -*-
"""生成 SU-03T 语音词导入表（xlsx + csv）。

单一数据源：改下面的 COMMANDS / SAYS，跑一次就行。

    python tools/gen_voice_import.py              # 生成到 docs/ 和 D:\\17978\\Documents\\
    python tools/gen_voice_import.py --out 路径.xlsx

做法：以官方模板 语音词模板.xlsx 为骨架，保留第 1~14 行（说明 + 表头 + 帧头帧尾），
把第 15 行起的数据行整体重写，共享字符串表在后面追加，前面的索引不动，
这样第 1~14 行的自定义说明与下拉校验都原样保留。
"""
from __future__ import print_function

import argparse
import io
import os
import re
import shutil
import sys
import zipfile
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                     # device/rk2206_home_safety
REPO = os.path.dirname(os.path.dirname(ROOT))    # repo 根

TEMPLATE = r"D:\17978\Documents\语音词模板.xlsx"
DOCS = os.path.join(REPO, "docs")
XLSX_OUT = os.path.join(DOCS, "语音词导入_小颐.xlsx")
DOCUMENTS = r"D:\17978\Documents\语音词导入_小颐.xlsx"
CSV_OUT = os.path.join(DOCS, "语音词导入_小颐.csv")

HEAD_ROW = 14      # 表头所在行，数据从 15 开始

# ---------------- 命令词 ----------------
# (行为名, 命令词(用 | 分隔), 发送协议, 备注)
COMMANDS = [
    ("VoiceHelpOn", "救命|快救我|帮我叫救护车|快来救我",
     "AA 55 10 01 55 AA", "紧急呼救，等同按板子红色按键"),
    ("VoiceHelpOff", "没事了|不用了|取消求救|报警取消",
     "AA 55 10 02 55 AA", "解除求助/确认报警"),
    ("VoiceMute", "静音|别响了|安静一点",
     "AA 55 11 01 55 AA", "静音蜂鸣与播报，报警状态保留"),
    ("VoiceUnmute", "取消静音|恢复播报|打开声音",
     "AA 55 11 02 55 AA", "恢复播报"),
    ("VoiceMedTake", "我吃过药了|吃过药了|药吃了|吃完药了|我吃药了|确认吃药",
     "AA 55 12 04 55 AA", "自动确认当前待服药时段（不区分第几次）"),
    ("VoiceLightOn", "开灯|把灯打开|打开灯|灯打开",
     "AA 55 13 01 55 AA", "覆盖自动夜灯"),
    ("VoiceLightOff", "关灯|把灯关了|关闭灯光|灯关掉",
     "AA 55 13 02 55 AA", "报警指示灯不受影响"),
    ("VoiceLightAuto", "自动灯光|灯光自动|打开自动夜灯",
     "AA 55 13 03 55 AA", "交回自动"),
    ("VoiceTemp", "温度|温度多少|现在温度多少|几度|冷不冷|热不热",
     "AA 55 14 01 55 AA", "播报出厂数值词条 1 号"),
    ("VoiceHumi", "湿度|湿度多少|现在湿度多少|潮不潮",
     "AA 55 14 02 55 AA", "播报出厂数值词条 2 号"),
    ("VoiceGas", "燃气|燃气多少|燃气浓度多少|有没有燃气泄漏",
     "AA 55 14 03 55 AA", "燃气偏高/正常两套应答"),
    ("VoiceLux", "光照|光照多少|光线够不够|房间亮不亮",
     "AA 55 14 04 55 AA", "播报出厂数值词条 3 号"),
    ("VoiceTempHumi", "温湿度多少|温度和湿度多少",
     "AA 55 14 05 55 AA", "温度、湿度两句，第二句延后 1.4 秒"),
    # 平台限制行为名 2~15 个字符，VoiceTempHumiLux(16) / VoicePageContact(16) 超限，故缩写。
    ("VoiceTHLux", "温湿度光照多少|温度湿度光照|温湿度和光照",
     "AA 55 14 06 55 AA", "温度、湿度、光照三句，逐句延后 1.4 秒"),
    ("VoicePage1", "看第一页记录|第一页",
     "AA 55 15 01 55 AA", "LCD 记录第一页"),
    ("VoicePage2", "看第二页记录|第二页",
     "AA 55 15 02 55 AA", "LCD 记录第二页"),
    ("VoicePageHome", "返回主页|回到首页|主页",
     "AA 55 15 03 55 AA", "LCD 主页（指标页）"),
    ("VoicePageDiag", "看诊断|设备诊断|诊断页",
     "AA 55 15 04 55 AA", "LCD 诊断页（维护用）"),
    ("VoicePageLiving", "看生活提醒|吃药提醒|生活提醒",
     "AA 55 15 05 55 AA", "LCD 生活提醒页"),
    ("VoiceContact", "看联系人|看消息|有没有消息",
     "AA 55 15 06 55 AA", "LCD 联系人页"),
    ("VoicePagePair", "手机配对|配对页",
     "AA 55 15 07 55 AA", "LCD 手机配对页（维护用）"),
    ("VoicePageNext", "下一页|翻页",
     "AA 55 15 08 55 AA", "七页循环，按当前页 +1"),
    ("VoicePagePrev", "上一页|往回翻",
     "AA 55 15 09 55 AA", "七页循环，按当前页 -1"),
]

# ---------------- 串口播报 ----------------
# (词条号, 播报文本, 备注)
SAYS = [
    (4, "检测到长时间没有活动，请确认一下情况", "固件词条号 4 报警"),
    (5, "房间温湿度异常，请检查", "固件词条号 5 报警"),
    (6, "检测到燃气浓度偏高，请立即开窗通风", "固件词条号 6 报警"),
    (7, "检测到可能跌倒，请确认是否需要帮助", "固件词条号 7 报警"),
    (8, "已发出紧急求助，请等待家人回应", "固件词条号 8 报警"),
    (9, "该吃药了", "固件词条号 9 服药提醒"),
    (10, "已开灯", "固件词条号 10"),
    (11, "已关灯", "固件词条号 11"),
    (12, "灯光已自动", "固件词条号 12"),
    (13, "已求助", "固件词条号 13 语音呼救"),
    (14, "已解除求助", "固件词条号 14"),
    (15, "已静音", "固件词条号 15"),
    (16, "声音已恢复", "固件词条号 16"),
    (17, "药已确认", "固件词条号 17"),
    (18, "没有要确认的药", "固件词条号 18"),
    (19, "现在没数据，稍后再问", "固件词条号 19"),
    (20, "燃气偏高，要注意", "固件词条号 20"),
    (21, "燃气正常", "固件词条号 21"),
    (22, "正忙，稍后再试", "固件词条号 22"),
    (23, "语音已就绪", "固件词条号 23 上电自检句"),
]

# ---------------- 语音词设置 ----------------
# 「语音词」工作表 B13~B20。留空 = 平台视为"不修改该项"，不会清空已有配置，
# 所以导入到**已有产品**前必须先把旧的免唤醒词/防误识别词手动删掉，或者干脆新建产品。
# 免唤醒命令词必须能在命令词模板里找到，凭空的词会直接阻断导入 —— 这里刻意留空。
SETTINGS = {
    "B13": "小颐小颐",                              # 唤醒词
    "B14": "我在|你说|有什么可以帮到你",              # 唤醒回复
    "B15": "",                                      # 免唤醒命令词：留空，必须喊唤醒词
    "B16": "",                                      # 防误识别词：留空，避免日常聊天误触发
    "B17": "无",                                    # 开机播报：关
    "B18": "有需要再叫我|没事我先退下",                # 超时退出回复
    "B19": "退下|你走吧|先这样|不聊了",                # 退出命令
    "B20": "有需要再叫我",                           # 退出回复
}

# 单元格样式：命令词行 A~D 无样式，F/G/H 用 8；串口输入行 F/G/H 用 8。
ST_REPLY = "8"


def rows_payload():
    """返回 [(行号偏移从0开始, {列字母: 文本}, [需要样式的列])]"""
    out = []
    for name, words, tx, note in COMMANDS:
        out.append(({"A": name, "B": "命令词", "C": "UART1", "D": words,
                     "E": tx, "H": note}, ["F", "G", "H"]))
    for idx, text, note in SAYS:
        frame = "AA 55 %02X 00 55 AA" % idx
        out.append(({"A": "Say%02d" % idx, "B": "串口输入", "C": "UART1",
                     "F": frame, "G": text, "H": note}, ["F", "G", "H"]))
    return out


def read_shared(xml):
    items = re.findall(r"<si>(.*?)</si>", xml, re.S)
    return items


def build_shared_xml(items):
    body = "".join("<si>%s</si>" % it for it in items)
    return ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n'
            '<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
            'count="%d" uniqueCount="%d">%s</sst>' % (len(items), len(items), body))


def text_of(si):
    return "".join(re.findall(r"<t[^>]*>(.*?)</t>", si, re.S))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="")
    ap.add_argument("--template", default=TEMPLATE)
    args = ap.parse_args()

    if not os.path.exists(args.template):
        print("找不到官方模板：%s" % args.template)
        return 2

    # 平台硬限制：行为名 2~15 个字符。超长的整份表会被拦在导入校验，
    # 且只报一句「长度需要在2-15个字符」，改起来来回好几轮，所以在这里先挡。
    bad = [n for n, _w, _t, _note in COMMANDS if not (2 <= len(n) <= 15)]
    if bad:
        print("行为名超长（平台限制 2~15 字符）：%s" % "、".join(bad))
        return 3

    zin = zipfile.ZipFile(args.template)
    sheet = zin.read("xl/worksheets/sheet1.xml").decode("utf-8")
    sheet2 = zin.read("xl/worksheets/sheet2.xml").decode("utf-8")
    shared = zin.read("xl/sharedStrings.xml").decode("utf-8")

    items = read_shared(shared)
    index_of = {}
    for i, it in enumerate(items):
        index_of.setdefault(text_of(it), i)

    def sid(text):
        if text in index_of:
            return index_of[text]
        items.append("<t>%s</t>" % escape(text))
        index_of[text] = len(items) - 1
        return index_of[text]

    # 1) 砍掉第 15 行及以后的数据行
    head, sep, tail = sheet.partition("</sheetData>")

    def drop_data_rows(match):
        no = int(match.group(1) or match.group(2))
        return "" if no > HEAD_ROW else match.group(0)

    # 数据行有两种写法：<row r="15" ...>...</row> 与空行 <row r="15"/>，
    # 必须两种都匹配，否则会残留同名空行，把新数据挤到后面去。
    head = re.sub(r'<row r="(\d+)"[^>]*/>|<row r="(\d+)".*?</row>',
                  drop_data_rows, head, flags=re.S)

    # 2) 重写数据行
    row_no = HEAD_ROW + 1
    chunks = []
    for cells, styled in rows_payload():
        attrs = ' spans="1:8" x14ac:dyDescent="0.25"'
        body = ""
        for col in "ABCDEFGH":
            text = cells.get(col, "")
            if text == "":
                continue
            s = ' s="%s"' % ST_REPLY if col in styled else ""
            body += '<c r="%s%d"%s t="s"><v>%d</v></c>' % (col, row_no, s, sid(text))
        chunks.append('<row r="%d"%s>%s</row>' % (row_no, attrs, body))
        row_no += 1

    last = row_no - 1
    sheet = head + "".join(chunks) + sep + tail
    sheet = re.sub(r'<dimension ref="[^"]*"/>', '<dimension ref="A1:H%d"/>' % last, sheet, count=1)

    # 3) 重写「语音词」工作表的 8 项设置（B13~B20）。
    # 模板里这些格子全是官方示例（你好小美 / 打开空调 / 那个|嗯嗯|啊啊 / 欢迎使用……），
    # 原样导入会：唤不醒、免唤醒词引用不存在的命令词而阻断导入、日常说话被误触发。
    def set_cell(xml, ref, text):
        pat = re.compile(r'<c r="%s"(?P<attrs>[^>]*?)(?:/>|>.*?</c>)' % ref, re.S)
        m = pat.search(xml)
        if not m:
            return xml
        if text == "":
            return xml[:m.start()] + xml[m.end():]
        attrs = re.sub(r'\s+t="[^"]*"', "", m.group("attrs"))
        new = '<c r="%s"%s t="s"><v>%d</v></c>' % (ref, attrs, sid(text))
        return xml[:m.start()] + new + xml[m.end():]

    for ref, text in SETTINGS.items():
        sheet2 = set_cell(sheet2, ref, text)

    # 4) 写出 xlsx：其余部件原样复制
    out = args.out or XLSX_OUT
    tmp = out + ".tmp"
    zout = zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED)
    for item in zin.infolist():
        if item.filename == "xl/worksheets/sheet1.xml":
            zout.writestr(item, sheet.encode("utf-8"))
        elif item.filename == "xl/worksheets/sheet2.xml":
            zout.writestr(item, sheet2.encode("utf-8"))
        elif item.filename == "xl/sharedStrings.xml":
            zout.writestr(item, build_shared_xml(items).encode("utf-8"))
        else:
            zout.writestr(item, zin.read(item.filename))
    zout.close()
    zin.close()
    shutil.move(tmp, out)

    # 5) CSV 副本（给校验脚本和评审看）
    with io.open(CSV_OUT, "w", encoding="utf-8-sig", newline="") as f:
        f.write(u"行为名称,触发方式,串口,命令词,发送协议,接收协议,回复语,备注\r\n")
        for cells, _ in rows_payload():
            f.write(u",".join(cells.get(c, "") for c in "ABCDEFGH") + u"\r\n")

    print("数据行 %d 条（命令词 %d + 串口播报 %d），最后一行 %d"
          % (len(COMMANDS) + len(SAYS), len(COMMANDS), len(SAYS), last))
    print("已写出：%s" % out)
    print("已写出：%s" % CSV_OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
