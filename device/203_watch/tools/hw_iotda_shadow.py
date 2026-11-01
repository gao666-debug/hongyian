#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
hw_iotda_shadow.py —— 用设备影子反查平台的物模型真相（PC 端）

控制台「在线但没数据」时用它：设备影子返回的内容里带着
service_id 和平台实际接受过的属性，能直接看出上报是不是被拒了、服务 ID 叫什么。

用法：
    python hw_iotda_shadow.py <接入域名> <设备ID> <设备密钥>

注意：会占用同一个 clientId，板子会被挤下线几十秒，之后自己重连。
"""
import hashlib
import hmac
import json
import socket
import struct
import sys
import time
from datetime import datetime, timezone


def enc_remlen(n):
    out = bytearray()
    while True:
        d = n % 128
        n //= 128
        if n > 0:
            d |= 0x80
        out.append(d)
        if n == 0:
            return bytes(out)


def put_str(s):
    b = s.encode("utf-8")
    return struct.pack(">H", len(b)) + b


def make_connect(cid, user, pwd, keepalive=120):
    vh = put_str("MQTT") + bytes([0x04, 0xC2]) + struct.pack(">H", keepalive)
    return bytes([0x10]) + enc_remlen(len(vh + put_str(cid) + put_str(user) + put_str(pwd))) + \
        vh + put_str(cid) + put_str(user) + put_str(pwd)


def make_subscribe(pid, topic, qos=0):
    body = struct.pack(">H", pid) + put_str(topic) + bytes([qos])
    return bytes([0x82]) + enc_remlen(len(body)) + body


def make_publish(topic, payload):
    body = put_str(topic) + payload
    return bytes([0x30]) + enc_remlen(len(body)) + body


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c:
            raise ConnectionError("对端关闭")
        buf += c
    return buf


def read_packet(sock):
    head = recv_exact(sock, 1)[0]
    mult, length = 1, 0
    while True:
        b = recv_exact(sock, 1)[0]
        length += (b & 0x7F) * mult
        if not (b & 0x80):
            break
        mult *= 128
    return head, (recv_exact(sock, length) if length else b"")


def parse_publish(body):
    tlen = struct.unpack(">H", body[:2])[0]
    topic = body[2:2 + tlen].decode("utf-8", "replace")
    payload = body[2 + tlen:]
    return topic, payload


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2

    domain, device_id, secret = sys.argv[1].strip(), sys.argv[2].strip(), sys.argv[3].strip()
    ts = datetime.now(timezone.utc).strftime("%Y%m%d%H")
    # 华为云：key 是时间戳，消息是设备密钥（和常见写法相反）
    pwd = hmac.new(ts.encode(), secret.encode(), hashlib.sha256).hexdigest()
    cid = f"{device_id}_0_1_{ts}"

    sock = socket.create_connection((domain, 1883), timeout=10)
    sock.sendall(make_connect(cid, device_id, pwd))
    head, body = read_packet(sock)
    code = body[1] if len(body) >= 2 else 0xFF
    print(f"[1] CONNACK 0x{code:02x}")
    if code != 0:
        return 1

    rid = "probe1"
    shadow_resp = f"$oc/devices/{device_id}/sys/shadow/get/response/request_id={rid}"
    cmd_topic = f"$oc/devices/{device_id}/sys/commands/#"

    pid = 1
    sock.sendall(make_subscribe(pid, shadow_resp))
    sock.sendall(make_subscribe(pid + 1, cmd_topic))

    # 收 SUBACK
    sock.settimeout(3.0)
    for _ in range(2):
        try:
            h, _b = read_packet(sock)
            print(f"[2] 订阅回执 type=0x{h >> 4}")
        except socket.timeout:
            print("[2] 订阅回执超时")
            break

    print(f"[3] 请求设备影子 -> {shadow_resp}")
    sock.sendall(make_publish(f"$oc/devices/{device_id}/sys/shadow/get/request_id={rid}", b""))

    sock.settimeout(6.0)
    got = 0
    deadline = time.time() + 12
    while time.time() < deadline:
        try:
            h, b = read_packet(sock)
        except socket.timeout:
            break
        except Exception as e:
            print(f"[!] 连接异常：{e}")
            break
        ptype = h >> 4
        if ptype == 3:
            topic, payload = parse_publish(b)
            got += 1
            print(f"[4] 下行 <- {topic}")
            try:
                print("    " + json.dumps(json.loads(payload), ensure_ascii=False, indent=2)[:1500])
            except Exception:
                print(f"    原始: {payload[:400]}")
            if rid in topic:
                break
        else:
            print(f"[4] 其他报文 type=0x{ptype:x} len={len(b)}")

    if got == 0:
        print("[4] 没有收到影子响应")
        print("    常见原因：产品没开「设备影子」，或平台不认识这个主题")
        print("    这种情况请改用控制台「设备详情 -> 消息跟踪」看上报处理结果")

    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
