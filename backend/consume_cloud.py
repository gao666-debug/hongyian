"""Receive IoTDA AMQP reports and persist through the authenticated local ingest API."""
import json
import os
from pathlib import Path
import ssl
import time

import requests
from proton import SSL, SSLDomain, Delivery
from proton.handlers import MessagingHandler
from proton.reactor import Container

from backend.settings import Settings, ROOT


def notification(body):
    if isinstance(body, bytes):
        body = body.decode('utf-8')
    payload = json.loads(body) if isinstance(body, str) else body
    if not isinstance(payload, dict):
        raise ValueError('Expected IoTDA notification')
    if payload.get('event') != 'report' or payload.get('resource') not in ('device.message', 'device.property'):
        raise ValueError('Unsupported notification type')
    return payload


DEAD_LETTER_DIR = ROOT / '.local' / 'dead-letter'


def archive_dead_letter(body, http_status):
    """把后端拒收的原始报文落到本地存档，保证「丢数据」可溯源。

    之所以不用 RELEASED 重投：422 是报文内容本身后端不认，重投只会无限循环并堵住队列头，
    因此这里仍是 REJECTED（出队），但原始报文已按天写入 JSONL，可人工回放。
    """
    DEAD_LETTER_DIR.mkdir(parents=True, exist_ok=True)
    path = DEAD_LETTER_DIR / (time.strftime('%Y%m%d') + '.jsonl')
    if isinstance(body, bytes):
        body = body.decode('utf-8', 'replace')
    record = {'at': int(time.time()), 'iso': time.strftime('%Y-%m-%dT%H:%M:%S'),
              'http': http_status, 'body': body if isinstance(body, str) else repr(body)}
    with open(path, 'a', encoding='utf-8') as handle:
        handle.write(json.dumps(record, ensure_ascii=False) + '\n')
        handle.flush()
        os.fsync(handle.fileno())
    return path


class Receiver(MessagingHandler):
    def __init__(self, settings):
        super().__init__(auto_accept=False, prefetch=10)
        self.settings = settings
        self.connected = False
        self.dead_letters = 0

    def on_start(self, event):
        host = os.environ['AMQP_HOST']
        key = os.environ['AMQP_ACCESS_KEY']
        code = os.environ['AMQP_ACCESS_CODE']
        queue = os.environ['AMQP_QUEUE']
        instance = os.getenv('AMQP_INSTANCE_ID', '')
        username = f'accessKey={key}|timestamp={int(time.time()*1000)}|instanceId={instance}'
        domain = SSLDomain(SSLDomain.MODE_CLIENT)
        ca = ssl.get_default_verify_paths().cafile or requests.certs.where()
        domain.set_trusted_ca_db(ca)
        domain.set_peer_authentication(SSLDomain.VERIFY_PEER_NAME)
        self.connection = event.container.connect(
            f'amqps://{host}:5671', user=username, password=code,
            heartbeat=60, ssl_domain=domain, reconnect=False,
            allowed_mechs='PLAIN', allow_insecure_mechs=False)
        event.container.create_receiver(self.connection, source=queue)

    def on_connection_opened(self, event):
        self.connected = True
        print('[cloud-consumer] AMQP connected', flush=True)

    def on_message(self, event):
        try:
            payload = notification(event.message.body)
            device = payload['notify_data']['header']['device_id']
            # 多设备：DEVICE_ID 支持逗号分隔，未配置的设备仍然拒收。
            # 注意这里和 app.py 的 check_device 必须保持一致，否则会出现
            # "云端收到了但后端永远没有数据"的情况（消息在此处被静默丢弃）。
            if not self.settings.has_device(device):
                event.delivery.update(Delivery.REJECTED)
                event.delivery.settle()
                return
            response = requests.post(
                os.getenv('AMQP_LOCAL_BACKEND', 'https://127.0.0.1:8443') + '/api/v1/iotda/notifications',
                json=payload, headers={'X-Webhook-Token': self.settings.webhook_token},
                verify=str(ROOT / '.local/tls/development-ca.crt'), timeout=(5, 10))
            if response.status_code == 422:
                # 不再静默丢弃：原始报文先落 dead-letter 存档，再出队。
                try:
                    target = archive_dead_letter(event.message.body, 422)
                    self.dead_letters += 1
                    print('[cloud-consumer] Unsupported message rejected and archived (total=%d) -> %s'
                          % (self.dead_letters, target), flush=True)
                except Exception as error:
                    print('[cloud-consumer] WARNING: dead-letter archive failed (%s); message body lost'
                          % type(error).__name__, flush=True)
                event.delivery.update(Delivery.REJECTED)
            else:
                response.raise_for_status()
                event.delivery.update(Delivery.ACCEPTED)
                print('[cloud-consumer] Device message persisted', flush=True)
            event.delivery.settle()
        except (ValueError, KeyError, TypeError):
            # 报文结构不符（解析失败）同样先存档再出队，便于事后排查。
            try:
                archive_dead_letter(event.message.body, 'parse-error')
            except Exception:
                pass
            event.delivery.update(Delivery.REJECTED)
            event.delivery.settle()
            print('[cloud-consumer] Invalid notification rejected and archived', flush=True)
        except requests.RequestException:
            event.delivery.update(Delivery.RELEASED)
            event.delivery.settle()
            event.connection.close()
            event.container.stop()
            print('[cloud-consumer] Local ingestion unavailable; reconnecting', flush=True)

    def on_transport_error(self, event):
        print('[cloud-consumer] Transport failure; reconnecting', flush=True)
        event.container.stop()

    def on_connection_error(self, event):
        print('[cloud-consumer] Connection rejected; check AMQP configuration', flush=True)
        event.container.stop()

    def on_disconnected(self, event):
        event.container.stop()


def main():
    if not SSL.present():
        raise SystemExit('AMQP SDK lacks TLS support: install pkg-config/libssl-dev and rebuild python-qpid-proton')
    settings = Settings.load()
    if settings.mode != 'live':
        raise SystemExit('AMQP consumer requires live mode')
    required = ['AMQP_HOST', 'AMQP_ACCESS_KEY', 'AMQP_ACCESS_CODE', 'AMQP_QUEUE']
    if any(not os.getenv(name) for name in required):
        raise SystemExit('Configure AMQP_HOST, AMQP_ACCESS_KEY, AMQP_ACCESS_CODE, AMQP_QUEUE in .env')
    while True:
        try:
            Container(Receiver(settings)).run()
        except KeyboardInterrupt:
            return
        except Exception as error:
            print('[cloud-consumer] Retry after', type(error).__name__, flush=True)
        time.sleep(5)


if __name__ == '__main__':
    main()
