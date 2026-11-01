#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
hw_iotda_probe.py —— 华为云 IoTDA 三元组连通性自检（PC 端）

在烧板子之前先跑这个，能一次性排除「设备ID写错 / 密钥写错 / 域名错 / 物模型属性名对不上」
这几类问题，省得看串口日志猜。

用法：
    python hw_iotda_probe.py <接入域名> <设备ID> <设备密钥> [服务ID]

示例：
    python hw_iotda_probe.py xxxx.st1.iotda-device.cn-east-3.myhuaweicloud.com \
        6a1b2c3d_dev1 12345678 Health

依赖：只用标准库。
"""
import hashlib
import hmac
import json
import socket
import struct
import sys
import time
from datetime import datetime, timezone


def enc_remlen(n: int) -> bytes:
    out = bytearray()
    while True:
        d = n % 128
        n //= 128
        if n > 0:
            d |= 0x80
        out.append(d)
        if n == 0:
            return bytes(out)


def put_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack(">H", len(b)) + b


def make_connect(client_id: str, username: str, password: str, keepalive: int = 120) -> bytes:
    vh = put_str("MQTT") + bytes([0x04, 0xC2]) + struct.pack(">H", keepalive)
    payload = put_str(client_id) + put_str(username) + put_str(password)
    body = vh + payload
    return bytes([0x10]) + enc_remlen(len(body)) + body


def make_publish(topic: str, payload: bytes) -> bytes:
    body = put_str(topic) + payload
    return bytes([0x30]) + enc_remlen(len(body)) + body


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("对端关闭了连接")
        buf += chunk
    return buf


def read_packet(sock: socket.socket):
    head = recv_exact(sock, 1)[0]
    mult = 1
    length = 0
    while True:
        b = recv_exact(sock, 1)[0]
        length += (b & 0x7F) * mult
        if not (b & 0x80):
            break
        mult *= 128
    body = recv_exact(sock, length) if length else b""
    return head, body


CONNACK_CODES = {
    0x00: "成功",
    0x01: "协议版本不正确",
    0x02: "clientId 格式非法，或心跳间隔不符合平台要求",
    0x03: "服务不可用",
    0x04: "用户名或密码错误",
    0x05: "未授权",
}


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2

    domain = sys.argv[1].strip()
    device_id = sys.argv[2].strip()
    secret = sys.argv[3].strip()
    service_id = sys.argv[4].strip() if len(sys.argv) > 4 else "Health"

    ts = datetime.now(timezone.utc).strftime("%Y%m%d%H")
    password = hmac.new(ts.encode(), secret.encode(), hashlib.sha256).hexdigest()
    client_id = f"{device_id}_0_1_{ts}"

    print("=" * 68)
    print(f"接入地址 : {domain}:1883")
    print(f"UTC 时间戳: {ts}")
    print(f"ClientId : {client_id}")
    print(f"Username : {device_id}")
    print(f"Password : {password}")
    print("=" * 68)

    try:
        sock = socket.create_connection((domain, 1883), timeout=10)
    except Exception as e:
        print(f"[失败] TCP 连接不上：{e}")
        return 1
    print("[1/4] TCP 连接成功")

    try:
        sock.sendall(make_connect(client_id, device_id, password))
        head, body = read_packet(sock)
        if head >> 4 != 2:
            print(f"[失败] 期望 CONNACK(0x20)，收到 0x{head:02x}")
            return 1
        code = body[1] if len(body) >= 2 else 0xFF
        print(f"[2/4] CONNACK 返回码 = 0x{code:02x} ({CONNACK_CODES.get(code, '未知')})")
        if code != 0:
            print("[失败] 鉴权没过，重点核对：设备ID、密钥、域名是否同一区域同一实例")
            return 1
    except Exception as e:
        print(f"[失败] 握手异常：{e}")
        return 1

    payload = {
        "services": [{
            "service_id": service_id,
            "properties": {
                "heart_rate": 72,
                "spo2": 98,
                "accel_x": 12,
                "accel_y": -8,
                "accel_z": 986,
                "steps": 1234,
                "fall_state": 0,
                "fall_count": 0,
            },
        }]
    }
    raw = json.dumps(payload, separators=(",", ":")).encode()
    topic = f"$oc/devices/{device_id}/sys/properties/report"

    try:
        sock.sendall(make_publish(topic, raw))
        print(f"[3/4] 属性上报已发出：{len(raw)} 字节 -> {topic}")
    except Exception as e:
        print(f"[失败] 发布失败：{e}")
        return 1

    # 平台成功不回复（QoS0），这里等一下看有没有被踢
    time.sleep(2)
    sock.settimeout(1.0)
    try:
        head, body = read_packet(sock)
        print(f"[4/4] 收到下行报文 0x{head:02x}，长度 {len(body)}")
        print(f"      原始内容: {body[:200]}")
    except socket.timeout:
        print("[4/4] 无下行报文（正常，QoS0 不回 ACK）")
    except Exception as e:
        print(f"[4/4] 连接被断开：{e}")

    sock.close()
    print("-" * 68)
    print("结论：三元组正确，设备应该已经在线。")
    print("去控制台「设备详情 -> 物模型数据」看有没有心跳/属性；")
    print("如果属性报错，多半是产品物模型里没有这些属性名，导入 iotda_product_model.json 再试。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
